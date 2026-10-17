/* ===========================================================================
 * rendezvous.c - the Infested match server: introduces players, never
 * carries a match.
 *
 * A host registers from its game socket and gets a six-digit game code; a
 * survivor asks for the public games or joins by code. The server tells each
 * side the other's public address (as seen here) and LAN address; the two
 * then punch through their own NATs and talk directly.
 *
 * One UDP socket, one thread, no allocation after start-up: every table is a
 * fixed array. The wire format is the game's own header,
 * src/game/mods/ZombieRendezvousWire.h; every body is a fixed-size struct,
 * so each handler checks its length once and copies it out with memcpy.
 *
 * The relay: a survivor whose punch failed asks for a session (RELAY_REQ);
 * the server tells the host (RELAY_OPEN), and from then on each side wraps
 * its game datagrams in RELAY_DATA, which the server passes to the other
 * side if the sender is one of the session's two endpoints. Hard caps keep
 * it from eating the hosting budget: sessions at once, bytes a second per
 * session and in total, and bytes a calendar month (in memory: a restart
 * starts the count again). Sessions close after 60 s of silence from
 * either side, or after 3 hours.
 *
 * The protocol is rdv_handle(), a pure function of (packet, source, time)
 * returning the datagrams to send; main() is the socket loop around it.
 * Built with -DRDV_NO_MAIN, the file is a library for the tests
 * (tests/test_zombie_rendezvous.py).
 *
 *     cc -O2 -o rendezvous server/rendezvous/rendezvous.c
 *     ./rendezvous [--port 27961] [--bind 0.0.0.0] [-v]
 * =========================================================================== */
#define _POSIX_C_SOURCE 200809L
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "../../src/game/mods/ZombieRendezvousWire.h"

#define RDV_MAX_GAMES       16384           /* registered at once */
#define RDV_MAX_IPS         16384           /* rate-limit buckets */
#define RDV_MAP_SLOTS       32768           /* each hash map: twice its table, a power of two */
#define RDV_EXPIRE_MS       30000           /* a host heartbeats every 5 s */
#define RDV_JOIN_PER_MIN    60              /* JOINs per source IP */
#define RDV_CODES_PER_MIN   30              /* new game codes per source IP */
#define RDV_MAX_OUT         2               /* datagrams one packet can cause */
#define RDV_DATAGRAM_MAX    (ZM_RDV_GAME_DATAGRAM_MAX + ZM_RDV_RELAY_OVERHEAD)
#define RDV_RELAY_HARD      1024            /* relay sessions the tables hold */
#define RDV_RELAY_IDLE_MS   60000           /* either side silent this long: closed */
#define RDV_RELAY_LIFE_MS   (3u * 3600u * 1000u)
#define RDV_RELAY_PER_IP    4               /* sessions from one survivor address */
#define RDV_RELAY_PER_GAME  6               /* three survivors, and stale ones from rejoins */

typedef struct rdv_endpoint { uint32_t ip; uint16_t port; } rdv_endpoint;
typedef struct rdv_out { rdv_endpoint to; int len; unsigned char data[RDV_DATAGRAM_MAX]; } rdv_out;

/* The relay's caps; main() takes them from the command line. */
typedef struct rdv_relay_limits {
    int      maxSessions;                   /* 0: relay off */
    double   sessionBytesPerSec;            /* both directions together */
    double   totalBytesPerSec;
    uint64_t monthBytes;                    /* relayed (sent) bytes a calendar month */
} rdv_relay_limits;
static rdv_relay_limits s_limits = { 64, 384.0 * 1024, 8.0 * 1024 * 1024, 150ull << 30 };

static int s_verbose = 1;
#define rdv_log(...) do { if (s_verbose) { rdv_stamp(); printf(__VA_ARGS__); fflush(stdout); } } while (0)

static void rdv_stamp(void)
{
    char buf[32];
    time_t t = time(NULL);
    struct tm tm;
    gmtime_r(&t, &tm);
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S ", &tm);
    fputs(buf, stdout);
}

/* ---------------------------------------------------------------------------
 * A uint32 -> table index map: open addressing, linear probing, tombstones.
 * Rebuilt in place when tombstones pile up.
 * ------------------------------------------------------------------------- */
enum { SLOT_EMPTY = 0, SLOT_USED = 1, SLOT_DEAD = 2 };
typedef struct rdv_map {
    uint32_t key[RDV_MAP_SLOTS];
    int32_t  val[RDV_MAP_SLOTS];
    uint8_t  state[RDV_MAP_SLOTS];
    int      used, dead;
} rdv_map;

static uint32_t map_hash(uint32_t k)
{
    k ^= k >> 16; k *= 0x7FEB352Du; k ^= k >> 15; k *= 0x846CA68Bu; k ^= k >> 16;
    return k & (RDV_MAP_SLOTS - 1);
}

static int map_find(const rdv_map* m, uint32_t key)
{
    uint32_t i = map_hash(key);
    for (int n = 0; n < RDV_MAP_SLOTS; n++, i = (i + 1) & (RDV_MAP_SLOTS - 1)) {
        if (m->state[i] == SLOT_EMPTY) return -1;
        if (m->state[i] == SLOT_USED && m->key[i] == key) return (int)i;
    }
    return -1;
}

static int32_t map_get(const rdv_map* m, uint32_t key)
{
    int i = map_find(m, key);
    return i < 0 ? -1 : m->val[i];
}

static void map_rebuild(rdv_map* m);

/* The key must not be in the map. Callers keep used + dead well under the
 * slot count (the tables are half the map's size). */
static void map_put(rdv_map* m, uint32_t key, int32_t val)
{
    if (m->used + m->dead >= RDV_MAP_SLOTS * 3 / 4) map_rebuild(m);
    uint32_t i = map_hash(key);
    while (m->state[i] == SLOT_USED) i = (i + 1) & (RDV_MAP_SLOTS - 1);
    if (m->state[i] == SLOT_DEAD) m->dead--;
    m->state[i] = SLOT_USED;
    m->key[i] = key;
    m->val[i] = val;
    m->used++;
}

static void map_del(rdv_map* m, uint32_t key)
{
    int i = map_find(m, key);
    if (i < 0) return;
    m->state[i] = SLOT_DEAD;
    m->used--;
    m->dead++;
}

static void map_rebuild(rdv_map* m)
{
    static uint32_t key[RDV_MAP_SLOTS];
    static int32_t val[RDV_MAP_SLOTS];
    int n = 0;
    for (int i = 0; i < RDV_MAP_SLOTS; i++)
        if (m->state[i] == SLOT_USED) { key[n] = m->key[i]; val[n] = m->val[i]; n++; }
    memset(m->state, 0, sizeof(m->state));
    m->used = m->dead = 0;
    for (int i = 0; i < n; i++) map_put(m, key[i], val[i]);
}

/* ---------------------------------------------------------------------------
 * State
 * ------------------------------------------------------------------------- */
typedef struct rdv_game {
    int          used;
    uint32_t     token, code;
    rdv_endpoint seen, local;
    uint8_t      version, flags, survivors, capacity;
    uint64_t     seenMs;
    int32_t      older, newer;              /* registration order, for the list */
} rdv_game;

typedef struct rdv_bucket {                 /* token buckets, refilled per minute */
    int      used;
    uint32_t ip;
    double   joins, codes;
    uint64_t lastMs;
} rdv_bucket;

static rdv_game   s_games[RDV_MAX_GAMES];
static int32_t    s_freeGame[RDV_MAX_GAMES];
static int        s_freeGames;
static int32_t    s_oldest = -1, s_newest = -1;
static int        s_gameCount;
static rdv_map    s_byToken, s_byCode, s_byIp;
typedef struct rdv_relay {
    int          used;
    uint32_t     id, code;
    rdv_endpoint a, b;                      /* a: the survivor, b: the host */
    uint64_t     createdMs, lastA, lastB, tokenMs;
    double       tokens;
    uint64_t     bytes;
} rdv_relay;

static rdv_bucket s_buckets[RDV_MAX_IPS];
static int32_t    s_freeBucket[RDV_MAX_IPS];
static int        s_freeBuckets;
static uint64_t   s_rng;
static rdv_relay  s_relays[RDV_RELAY_HARD];
static int32_t    s_freeRelay[RDV_RELAY_HARD];
static int        s_freeRelays, s_relayCount;
static rdv_map    s_byRelay;
static double     s_totalTokens;
static uint64_t   s_totalTokenMs, s_monthUsed, s_relayDropped;
static int        s_monthKey = -1;

void rdv_reset(uint64_t seed)
{
    memset(s_games, 0, sizeof(s_games));
    memset(s_buckets, 0, sizeof(s_buckets));
    memset(&s_byToken, 0, sizeof(s_byToken));
    memset(&s_byCode, 0, sizeof(s_byCode));
    memset(&s_byIp, 0, sizeof(s_byIp));
    for (int i = 0; i < RDV_MAX_GAMES; i++) s_freeGame[i] = RDV_MAX_GAMES - 1 - i;
    for (int i = 0; i < RDV_MAX_IPS; i++) s_freeBucket[i] = RDV_MAX_IPS - 1 - i;
    s_freeGames = RDV_MAX_GAMES;
    s_freeBuckets = RDV_MAX_IPS;
    s_oldest = s_newest = -1;
    s_gameCount = 0;
    s_rng = seed ? seed : 0x9E3779B97F4A7C15ull;
    memset(s_relays, 0, sizeof(s_relays));
    memset(&s_byRelay, 0, sizeof(s_byRelay));
    for (int i = 0; i < RDV_RELAY_HARD; i++) s_freeRelay[i] = RDV_RELAY_HARD - 1 - i;
    s_freeRelays = RDV_RELAY_HARD;
    s_relayCount = 0;
    s_totalTokens = 0; s_totalTokenMs = 0;
    s_monthUsed = s_relayDropped = 0;
}

void rdv_set_relay_limits(int maxSessions, double sessionBytesPerSec, double totalBytesPerSec, uint64_t monthBytes)
{
    if (maxSessions > RDV_RELAY_HARD) maxSessions = RDV_RELAY_HARD;
    s_limits.maxSessions = maxSessions < 0 ? 0 : maxSessions;
    s_limits.sessionBytesPerSec = sessionBytesPerSec;
    s_limits.totalBytesPerSec = totalBytesPerSec;
    s_limits.monthBytes = monthBytes;
}

/* main() calls it with year * 12 + month (UTC): a new month, a new budget. */
void rdv_set_month(int key)
{
    if (key != s_monthKey) { s_monthKey = key; s_monthUsed = 0; }
}

int      rdv_relay_count(void) { return s_relayCount; }
uint64_t rdv_relay_month_bytes(void) { return s_monthUsed; }
uint64_t rdv_relay_dropped(void) { return s_relayDropped; }
int      rdv_out_bytes(void) { return (int)sizeof(rdv_out); }

void rdv_set_verbose(int on) { s_verbose = on; }
int  rdv_game_count(void) { return s_gameCount; }
int  rdv_join_limit(void) { return RDV_JOIN_PER_MIN; }
int  rdv_expire_ms(void) { return RDV_EXPIRE_MS; }

static uint32_t rdv_random(void)            /* xorshift64*: unpredictable enough for codes */
{
    s_rng ^= s_rng >> 12; s_rng ^= s_rng << 25; s_rng ^= s_rng >> 27;
    return (uint32_t)((s_rng * 0x2545F4914F6CDD1Dull) >> 32);
}

static const char* ip_text(uint32_t ip)
{
    static char buf[4][16];
    static int n;
    char* b = buf[n++ & 3];
    snprintf(b, 16, "%u.%u.%u.%u", ip >> 24, (ip >> 16) & 255, (ip >> 8) & 255, ip & 255);
    return b;
}

static void game_drop(int32_t g, const char* why)
{
    rdv_game* G = &s_games[g];
    rdv_log("game %06u %s\n", G->code, why);
    map_del(&s_byToken, G->token);
    map_del(&s_byCode, G->code);
    if (G->older >= 0) s_games[G->older].newer = G->newer; else s_oldest = G->newer;
    if (G->newer >= 0) s_games[G->newer].older = G->older; else s_newest = G->older;
    G->used = 0;
    s_freeGame[s_freeGames++] = g;
    s_gameCount--;
}

static void relay_close(int32_t r, const char* why)
{
    rdv_relay* R = &s_relays[r];
    rdv_log("relay %08x for game %06u %s (%llu bytes)\n", R->id, R->code, why, (unsigned long long)R->bytes);
    map_del(&s_byRelay, R->id);
    R->used = 0;
    s_freeRelay[s_freeRelays++] = r;
    s_relayCount--;
}

void rdv_expire(uint64_t now)
{
    for (int r = 0; r < RDV_RELAY_HARD; r++) {
        rdv_relay* R = &s_relays[r];
        if (!R->used) continue;
        if (now - R->lastA > RDV_RELAY_IDLE_MS || now - R->lastB > RDV_RELAY_IDLE_MS) relay_close(r, "went quiet");
        else if (now - R->createdMs > RDV_RELAY_LIFE_MS) relay_close(r, "ran out of time");
    }
    /* Oldest registration first is not oldest heartbeat first, so scan all. */
    for (int32_t g = s_oldest; g >= 0;) {
        int32_t next = s_games[g].newer;
        if (now - s_games[g].seenMs > RDV_EXPIRE_MS) game_drop(g, "expired");
        g = next;
    }
    for (int i = 0; i < RDV_MAX_IPS; i++) {
        rdv_bucket* b = &s_buckets[i];
        if (b->used && now - b->lastMs > 60000) {
            map_del(&s_byIp, b->ip);
            b->used = 0;
            s_freeBucket[s_freeBuckets++] = i;
        }
    }
}

/* Takes one JOIN (codes = 0) or one new code (codes = 1) from ip's bucket. */
static int rate_allow(uint32_t ip, int codes, uint64_t now)
{
    int32_t i = map_get(&s_byIp, ip);
    if (i < 0) {
        if (!s_freeBuckets) return 1;      /* full of distinct IPs: no limit rather than a lockout */
        i = s_freeBucket[--s_freeBuckets];
        rdv_bucket* b = &s_buckets[i];
        b->used = 1; b->ip = ip; b->lastMs = now;
        b->joins = RDV_JOIN_PER_MIN; b->codes = RDV_CODES_PER_MIN;
        map_put(&s_byIp, ip, i);
    }
    rdv_bucket* b = &s_buckets[i];
    double minutes = (double)(now - b->lastMs) / 60000.0;
    b->lastMs = now;
    b->joins += minutes * RDV_JOIN_PER_MIN;
    b->codes += minutes * RDV_CODES_PER_MIN;
    if (b->joins > RDV_JOIN_PER_MIN) b->joins = RDV_JOIN_PER_MIN;
    if (b->codes > RDV_CODES_PER_MIN) b->codes = RDV_CODES_PER_MIN;
    double* tokens = codes ? &b->codes : &b->joins;
    if (*tokens < 1.0) return 0;
    *tokens -= 1.0;
    return 1;
}

/* ---------------------------------------------------------------------------
 * Packets
 * ------------------------------------------------------------------------- */
static rdv_out* out_begin(rdv_out* out, int* count, rdv_endpoint to, unsigned char type)
{
    rdv_out* o = &out[(*count)++];
    RdvHeader h;
    memset(&h, 0, sizeof(h));
    h.magic = ZM_RDV_MAGIC; h.protocol = ZM_RDV_PROTOCOL; h.type = type;
    o->to = to;
    memcpy(o->data, &h, sizeof(h));
    o->len = (int)sizeof(h);
    return o;
}

static void out_add(rdv_out* o, const void* body, int len)
{
    memcpy(o->data + o->len, body, (size_t)len);
    o->len += len;
}

static RdvAddr wire_addr(rdv_endpoint e)
{
    RdvAddr a;
    memset(&a, 0, sizeof(a));
    a.ip = e.ip; a.port = e.port;
    return a;
}

static void join_fail(rdv_out* out, int* n, rdv_endpoint src, uint32_t code, int reason)
{
    RdvJoinFail f;
    memset(&f, 0, sizeof(f));
    f.code = code; f.reason = (unsigned char)reason;
    out_add(out_begin(out, n, src, ZM_RDV_JOIN_FAIL), &f, sizeof(f));
}

static int on_register(const unsigned char* body, int len, rdv_endpoint src, uint64_t now, rdv_out* out)
{
    RdvRegister r;
    int n = 0;
    if (len < (int)sizeof(r)) return 0;
    memcpy(&r, body, sizeof(r));
    if (!r.token) return 0;
    int32_t g = map_get(&s_byToken, r.token);
    if (g < 0) {
        if (!s_freeGames || !rate_allow(src.ip, 1, now)) return 0;
        uint32_t code = 0;
        for (int tries = 0; tries < 1000 && !code; tries++) {
            uint32_t c = 100000u + rdv_random() % 900000u;
            if (map_get(&s_byCode, c) < 0) code = c;
        }
        if (!code) return 0;
        g = s_freeGame[--s_freeGames];
        rdv_game* G = &s_games[g];
        memset(G, 0, sizeof(*G));
        G->used = 1; G->token = r.token; G->code = code;
        G->older = s_newest; G->newer = -1;
        if (s_newest >= 0) s_games[s_newest].newer = g; else s_oldest = g;
        s_newest = g;
        map_put(&s_byToken, r.token, g);
        map_put(&s_byCode, code, g);
        s_gameCount++;
        rdv_log("game %06u registered by %s:%u (version %u)\n", code, ip_text(src.ip), src.port, r.version);
    } else if (s_games[g].seen.ip != src.ip || s_games[g].seen.port != src.port) {
        rdv_log("game %06u moved to %s:%u\n", s_games[g].code, ip_text(src.ip), src.port);
        /* Its router gave it a new port: its relayed survivors follow. */
        for (int r = 0; r < RDV_RELAY_HARD; r++) {
            rdv_relay* R = &s_relays[r];
            if (R->used && R->code == s_games[g].code && R->b.ip == s_games[g].seen.ip &&
                R->b.port == s_games[g].seen.port) R->b = src;
        }
    }
    rdv_game* G = &s_games[g];
    G->seen = src;
    G->local.ip = r.local.ip; G->local.port = r.local.port;
    G->version = r.version; G->flags = r.flags;
    G->survivors = r.survivors; G->capacity = r.capacity;
    G->seenMs = now;
    RdvRegistered reply;
    memset(&reply, 0, sizeof(reply));
    reply.code = G->code;
    reply.seen = wire_addr(src);
    out_add(out_begin(out, &n, src, ZM_RDV_REGISTERED), &reply, sizeof(reply));
    return n;
}

static int on_unregister(const unsigned char* body, int len, rdv_endpoint src)
{
    RdvUnregister u;
    if (len < (int)sizeof(u)) return 0;
    memcpy(&u, body, sizeof(u));
    int32_t g = map_get(&s_byToken, u.token);
    /* Only from where it registered: nobody else can close a game. */
    if (g >= 0 && s_games[g].seen.ip == src.ip && s_games[g].seen.port == src.port) game_drop(g, "closed");
    return 0;
}

static int game_live(const rdv_game* G, uint64_t now)
{
    return now - G->seenMs <= RDV_EXPIRE_MS;
}

static int on_list(const unsigned char* body, int len, rdv_endpoint src, uint64_t now, rdv_out* out)
{
    RdvListReq q;
    int n = 0;
    if (len < (int)sizeof(q)) return 0;
    memcpy(&q, body, sizeof(q));
    rdv_out* o = out_begin(out, &n, src, ZM_RDV_LIST);
    RdvListHead head;
    memset(&head, 0, sizeof(head));
    int headAt = o->len;
    out_add(o, &head, sizeof(head));
    for (int32_t g = s_oldest; g >= 0 && head.count < ZM_RDV_LIST_MAX; g = s_games[g].newer) {
        const rdv_game* G = &s_games[g];
        if (G->version != q.version || !(G->flags & ZM_RDV_PUBLIC) || (G->flags & ZM_RDV_STARTED) ||
            G->survivors >= G->capacity || !game_live(G, now)) continue;
        RdvListEntry e;
        e.code = G->code; e.survivors = G->survivors; e.capacity = G->capacity;
        e.flags = G->flags; e.version = G->version;
        out_add(o, &e, sizeof(e));
        head.count++;
    }
    memcpy(o->data + headAt, &head, sizeof(head));
    return n;
}

static int on_join(const unsigned char* body, int len, rdv_endpoint src, uint64_t now, rdv_out* out)
{
    RdvJoin j;
    int n = 0;
    if (len < (int)sizeof(j)) return 0;
    memcpy(&j, body, sizeof(j));
    if (!rate_allow(src.ip, 0, now)) { join_fail(out, &n, src, j.code, ZM_RDV_FAIL_BUSY); return n; }
    int32_t g = map_get(&s_byCode, j.code);
    if (g < 0 || !game_live(&s_games[g], now)) { join_fail(out, &n, src, j.code, ZM_RDV_FAIL_NOT_FOUND); return n; }
    const rdv_game* G = &s_games[g];
    if (G->version != j.version) { join_fail(out, &n, src, j.code, ZM_RDV_FAIL_VERSION); return n; }
    /* A started match still takes a JOIN: a crashed survivor rejoins by
     * code, and the host decides whether its HELLO reclaims a seat. */
    if (!(G->flags & ZM_RDV_STARTED) && G->survivors >= G->capacity) {
        join_fail(out, &n, src, j.code, ZM_RDV_FAIL_FULL);
        return n;
    }
    rdv_log("game %06u: introducing %s:%u\n", G->code, ip_text(src.ip), src.port);
    RdvIntro peer, intro;
    memset(&peer, 0, sizeof(peer));
    memset(&intro, 0, sizeof(intro));
    peer.code = intro.code = G->code;
    peer.seen = wire_addr(G->seen);
    peer.local = wire_addr(G->local);
    intro.seen = wire_addr(src);
    intro.local.ip = j.local.ip; intro.local.port = j.local.port;
    out_add(out_begin(out, &n, src, ZM_RDV_PEER), &peer, sizeof(peer));
    out_add(out_begin(out, &n, G->seen, ZM_RDV_INTRO), &intro, sizeof(intro));
    return n;
}

static int on_probe(const unsigned char* body, int len, rdv_endpoint src, rdv_out* out)
{
    RdvProbe p;
    int n = 0;
    if (len < (int)sizeof(p)) return 0;
    memcpy(&p, body, sizeof(p));
    RdvProbeReply r;
    memset(&r, 0, sizeof(r));
    r.nonce = p.nonce;
    r.seen = wire_addr(src);
    out_add(out_begin(out, &n, src, ZM_RDV_PROBE_REPLY), &r, sizeof(r));
    return n;
}

static int same_endpoint(rdv_endpoint x, rdv_endpoint y) { return x.ip == y.ip && x.port == y.port; }

static void relay_fail(rdv_out* out, int* n, rdv_endpoint src, uint32_t code, int reason)
{
    RdvRelayFail f;
    memset(&f, 0, sizeof(f));
    f.code = code; f.reason = (unsigned char)reason;
    out_add(out_begin(out, n, src, ZM_RDV_RELAY_FAIL), &f, sizeof(f));
}

static void relay_announce(rdv_out* out, int* n, const rdv_relay* R)
{
    RdvRelayOk ok;
    RdvRelayOpen open;
    memset(&ok, 0, sizeof(ok));
    memset(&open, 0, sizeof(open));
    ok.code = open.code = R->code;
    ok.session = open.session = R->id;
    open.seen = wire_addr(R->a);
    out_add(out_begin(out, n, R->a, ZM_RDV_RELAY_OK), &ok, sizeof(ok));
    out_add(out_begin(out, n, R->b, ZM_RDV_RELAY_OPEN), &open, sizeof(open));
}

static int on_relay_req(const unsigned char* body, int len, rdv_endpoint src, uint64_t now, rdv_out* out)
{
    RdvRelayReq q;
    int n = 0;
    if (len < (int)sizeof(q)) return 0;
    memcpy(&q, body, sizeof(q));
    int32_t g = map_get(&s_byCode, q.code);
    if (g < 0 || !game_live(&s_games[g], now)) { relay_fail(out, &n, src, q.code, ZM_RDV_FAIL_NOT_FOUND); return n; }
    const rdv_game* G = &s_games[g];
    if (G->version != q.version) { relay_fail(out, &n, src, q.code, ZM_RDV_FAIL_VERSION); return n; }
    /* A retry (the OK or the OPEN lost): the same session again. */
    int perIp = 0, perGame = 0;
    for (int r = 0; r < RDV_RELAY_HARD; r++) {
        const rdv_relay* R = &s_relays[r];
        if (!R->used) continue;
        if (R->code == q.code && same_endpoint(R->a, src)) { relay_announce(out, &n, R); return n; }
        if (R->a.ip == src.ip) perIp++;
        if (R->code == q.code) perGame++;
    }
    if (!rate_allow(src.ip, 0, now)) { relay_fail(out, &n, src, q.code, ZM_RDV_FAIL_BUSY); return n; }
    if (s_relayCount >= s_limits.maxSessions || !s_freeRelays || s_monthUsed >= s_limits.monthBytes ||
        perIp >= RDV_RELAY_PER_IP || perGame >= RDV_RELAY_PER_GAME) {
        rdv_log("game %06u: relay refused for %s:%u (%d sessions, %llu MB this month)\n", q.code,
                ip_text(src.ip), src.port, s_relayCount, (unsigned long long)(s_monthUsed >> 20));
        relay_fail(out, &n, src, q.code, ZM_RDV_FAIL_NO_RELAY);
        return n;
    }
    uint32_t id = 0;
    for (int tries = 0; tries < 100 && !id; tries++) {
        uint32_t c = rdv_random();
        if (c && map_get(&s_byRelay, c) < 0) id = c;
    }
    if (!id) return 0;
    int32_t r = s_freeRelay[--s_freeRelays];
    rdv_relay* R = &s_relays[r];
    memset(R, 0, sizeof(*R));
    R->used = 1; R->id = id; R->code = q.code;
    R->a = src; R->b = G->seen;
    R->createdMs = R->lastA = R->lastB = R->tokenMs = now;
    R->tokens = s_limits.sessionBytesPerSec;
    map_put(&s_byRelay, id, r);
    s_relayCount++;
    rdv_log("relay %08x: %s:%u <-> game %06u at %s:%u\n", id, ip_text(src.ip), src.port, q.code,
            ip_text(R->b.ip), R->b.port);
    relay_announce(out, &n, R);
    return n;
}

static void refill(double* tokens, uint64_t* lastMs, double perSec, uint64_t now)
{
    *tokens += (double)(now - *lastMs) * perSec / 1000.0;
    *lastMs = now;
    if (*tokens > perSec) *tokens = perSec;      /* a second's worth of burst */
}

static int on_relay_data(const unsigned char* packet, int packetLen, const unsigned char* body, int len,
                         rdv_endpoint src, uint64_t now, rdv_out* out)
{
    RdvRelayData d;
    int n = 0;
    if (len < (int)sizeof(d) || len - (int)sizeof(d) > ZM_RDV_GAME_DATAGRAM_MAX) return 0;
    memcpy(&d, body, sizeof(d));
    int32_t r = map_get(&s_byRelay, d.session);
    if (r < 0) return 0;
    rdv_relay* R = &s_relays[r];
    rdv_endpoint to;
    if (same_endpoint(src, R->a)) { to = R->b; R->lastA = now; }
    else if (same_endpoint(src, R->b)) { to = R->a; R->lastB = now; }
    else return 0;                          /* not one of the two: never forwarded */
    refill(&R->tokens, &R->tokenMs, s_limits.sessionBytesPerSec, now);
    if (!s_totalTokenMs) { s_totalTokenMs = now; s_totalTokens = s_limits.totalBytesPerSec; }
    refill(&s_totalTokens, &s_totalTokenMs, s_limits.totalBytesPerSec, now);
    if (R->tokens < packetLen || s_totalTokens < packetLen) { s_relayDropped++; return 0; }
    R->tokens -= packetLen;
    s_totalTokens -= packetLen;
    R->bytes += (uint64_t)packetLen;
    s_monthUsed += (uint64_t)packetLen;
    rdv_out* o = &out[n++];
    o->to = to;
    memcpy(o->data, packet, (size_t)packetLen);  /* passed on as it came: header, session, datagram */
    o->len = packetLen;
    return n;
}

static int on_relay_close(const unsigned char* body, int len, rdv_endpoint src)
{
    RdvRelayClose c;
    if (len < (int)sizeof(c)) return 0;
    memcpy(&c, body, sizeof(c));
    int32_t r = map_get(&s_byRelay, c.session);
    if (r >= 0 && (same_endpoint(src, s_relays[r].a) || same_endpoint(src, s_relays[r].b))) relay_close(r, "closed");
    return 0;
}

/* One datagram in; fills out[] (room for RDV_MAX_OUT) and returns how many to send. */
int rdv_handle(const unsigned char* data, int len, uint32_t srcIp, uint16_t srcPort, uint64_t now, rdv_out* out)
{
    RdvHeader h;
    rdv_endpoint src;
    src.ip = srcIp; src.port = srcPort;
    if (len < (int)sizeof(h) || len > RDV_DATAGRAM_MAX) return 0;
    memcpy(&h, data, sizeof(h));
    if (h.magic != ZM_RDV_MAGIC || h.protocol != ZM_RDV_PROTOCOL) return 0;
    const unsigned char* body = data + sizeof(h);
    int bodyLen = len - (int)sizeof(h);
    switch (h.type) {
    case ZM_RDV_REGISTER:   return on_register(body, bodyLen, src, now, out);
    case ZM_RDV_UNREGISTER: return on_unregister(body, bodyLen, src);
    case ZM_RDV_LIST_REQ:   return on_list(body, bodyLen, src, now, out);
    case ZM_RDV_JOIN:       return on_join(body, bodyLen, src, now, out);
    case ZM_RDV_PROBE:      return on_probe(body, bodyLen, src, out);
    case ZM_RDV_RELAY_REQ:  return on_relay_req(body, bodyLen, src, now, out);
    case ZM_RDV_RELAY_DATA: return on_relay_data(data, len, body, bodyLen, src, now, out);
    case ZM_RDV_RELAY_CLOSE: return on_relay_close(body, bodyLen, src);
    default:                return 0;
    }
}

/* ---------------------------------------------------------------------------
 * The socket loop
 * ------------------------------------------------------------------------- */
#ifndef RDV_NO_MAIN
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <unistd.h>

static volatile sig_atomic_t s_stop;
static void on_signal(int sig) { (void)sig; s_stop = 1; }

static uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)(ts.tv_nsec / 1000000);
}

static uint64_t seed_from_os(void)
{
    uint64_t seed = 0;
    FILE* f = fopen("/dev/urandom", "rb");
    if (f) { if (fread(&seed, sizeof(seed), 1, f) != 1) seed = 0; fclose(f); }
    return seed ^ ((uint64_t)time(NULL) << 20) ^ now_ms();
}

int main(int argc, char** argv)
{
    const char* bindAddr = "0.0.0.0";
    int port = ZM_RDV_DEFAULT_PORT;
    int verbose = 0;
    int maxRelays = 64;
    double sessionKBps = 384, totalMbit = 64, monthGB = 150;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--port") && i + 1 < argc) port = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--bind") && i + 1 < argc) bindAddr = argv[++i];
        else if (!strcmp(argv[i], "-v") || !strcmp(argv[i], "--verbose")) verbose = 1;
        else if (!strcmp(argv[i], "-q") || !strcmp(argv[i], "--quiet")) verbose = -1;
        else if (!strcmp(argv[i], "--max-relays") && i + 1 < argc) maxRelays = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--relay-kbps") && i + 1 < argc) sessionKBps = atof(argv[++i]);
        else if (!strcmp(argv[i], "--relay-total-mbps") && i + 1 < argc) totalMbit = atof(argv[++i]);
        else if (!strcmp(argv[i], "--relay-gb-month") && i + 1 < argc) monthGB = atof(argv[++i]);
        else {
            fprintf(stderr,
                    "usage: %s [--port N] [--bind ADDR] [-v | -q]\n"
                    "          [--max-relays N] (64; 0 turns the relay off)\n"
                    "          [--relay-kbps KB/s per session] (384)\n"
                    "          [--relay-total-mbps Mbit/s for all sessions] (64)\n"
                    "          [--relay-gb-month GB relayed a calendar month] (150)\n", argv[0]);
            return 2;
        }
    }
    {   /* The wire is little-endian; the structs are copied as they lie. */
        uint32_t one = 1;
        unsigned char b;
        memcpy(&b, &one, 1);
        if (b != 1) { fprintf(stderr, "rendezvous: needs a little-endian machine\n"); return 1; }
    }
    rdv_reset(seed_from_os());
    rdv_set_verbose(verbose >= 0);
    rdv_set_relay_limits(maxRelays, sessionKBps * 1024.0, totalMbit * 1000.0 * 1000.0 / 8.0,
                         (uint64_t)(monthGB * 1024.0 * 1024.0 * 1024.0));

    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) { perror("socket"); return 1; }
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, bindAddr, &addr.sin_addr) != 1) { fprintf(stderr, "bad --bind %s\n", bindAddr); return 2; }
    if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) != 0) { perror("bind"); return 1; }
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    rdv_log("listening on udp %s:%d\n", bindAddr, port);
    if (maxRelays > 0)
        rdv_log("relay: %d sessions, %.0f KB/s each, %.0f Mbit/s in all, %.0f GB a month\n",
                maxRelays, sessionKBps, totalMbit, monthGB);
    else
        rdv_log("relay off\n");

    static rdv_out out[RDV_MAX_OUT];
    unsigned char buf[2048];   /* bigger than any valid packet: an oversize one reads as too long */
    uint64_t lastExpire = now_ms(), lastStats = lastExpire;
    uint64_t packets = 0;
    while (!s_stop) {
        struct pollfd p = { fd, POLLIN, 0 };
        int ready = poll(&p, 1, 1000);
        if (ready < 0 && errno != EINTR) { perror("poll"); break; }
        for (int burst = 0; burst < 256; burst++) {
            struct sockaddr_in from;
            socklen_t fromLen = sizeof(from);
            ssize_t n = recvfrom(fd, buf, sizeof(buf), 0, (struct sockaddr*)&from, &fromLen);
            if (n < 0) break;               /* EAGAIN: drained */
            packets++;
            int count = rdv_handle(buf, (int)n, ntohl(from.sin_addr.s_addr), ntohs(from.sin_port), now_ms(), out);
            for (int i = 0; i < count; i++) {
                struct sockaddr_in to;
                memset(&to, 0, sizeof(to));
                to.sin_family = AF_INET;
                to.sin_addr.s_addr = htonl(out[i].to.ip);
                to.sin_port = htons(out[i].to.port);
                sendto(fd, out[i].data, (size_t)out[i].len, 0, (struct sockaddr*)&to, sizeof(to));
            }
        }
        uint64_t now = now_ms();
        if (now - lastExpire >= 1000) {
            lastExpire = now;
            rdv_expire(now);
            time_t wall = time(NULL);
            struct tm tm;
            gmtime_r(&wall, &tm);
            rdv_set_month(tm.tm_year * 12 + tm.tm_mon);
        }
        if (verbose > 0 && now - lastStats >= 60000) {
            lastStats = now;
            rdv_log("%d games, %d relays, %llu MB relayed this month, %llu relay packets over the caps, %llu packets\n",
                    rdv_game_count(), rdv_relay_count(), (unsigned long long)(rdv_relay_month_bytes() >> 20),
                    (unsigned long long)rdv_relay_dropped(), (unsigned long long)packets);
        }
    }
    rdv_log("stopping\n");
    close(fd);
    return 0;
}
#endif
