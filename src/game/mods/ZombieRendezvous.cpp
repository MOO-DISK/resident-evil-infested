#include "ZombieRendezvous.h"
#include "ZombieNet.h"
#include "../../DebugPrint.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>

// ============================================================================
// ZombieRendezvous.cpp - the game codes, the public list and the NAT punch.
// See ZombieRendezvous.h.
//
// The host keeps registering every 5 s for the whole match: the server drops
// a code 30 s after its last heartbeat, and a crashed survivor's REJOIN finds
// the host by the same code. A survivor talks to the server only until the
// punch lands; after that ZombieNet owns the socket and the punches still in
// flight come in through net_handle.
// ============================================================================

char g_zmRdvServer[96] = ZM_RDV_DEFAULT_SERVER;
bool g_zmRdvPublic = true;

#define RDV_REGISTER_MS        5000   // the heartbeat (the server keeps 30 s)
#define RDV_REGISTER_FAST_MS   1000   // ... until the first answer
#define RDV_REGISTER_MIN_MS    250    // a changed listing goes at once, but not faster
#define RDV_NO_ANSWER_MS       8000   // the host's lobby says the server is silent
#define RDV_LIST_MS            3000
#define RDV_JOIN_MS            1000
#define RDV_JOIN_TIMEOUT_MS    6000
#define RDV_REJOIN_PUNCH_MS    2000   // JOIN again while punching: a lost INTRO
#define RDV_PUNCH_MS           150
#define RDV_PUNCH_TIMEOUT_MS   10000
#define RDV_HOST_PUNCH_MS      12000  // how long the host punches at one survivor
#define RDV_PUNCH_SLOTS        8

static PlatNetAddr s_server;
static bool        s_serverOk;

// ---- the host ----
struct RdvPunchTarget {
    bool         used;
    PlatNetAddr  seen, local;
    unsigned int nonce, untilMs, lastMs;
};
static bool         s_hostOn;
static unsigned int s_token, s_code;
static unsigned int s_hostBeganMs, s_lastRegisterMs;
static int          s_hostState = ZM_RDV_HOST_OFF;
static RdvRegister  s_lastRegister;
static PlatNetAddr  s_hostLocal;          // asked once: it opens a socket to find out
static RdvPunchTarget s_targets[RDV_PUNCH_SLOTS];

// ---- a survivor ----
enum { C_IDLE, C_ASK, C_PUNCH, C_DONE, C_FAILED };
static bool         s_ownSocket;          // opened here, not yet ZombieNet's
static int          s_cState = C_IDLE;
static unsigned int s_cCode, s_cNonce, s_cBeganMs, s_cLastJoinMs, s_cLastPunchMs, s_cPunchBeganMs;
static bool         s_cRejoin;
static PlatNetAddr  s_cLocal;
static PlatNetAddr  s_peerSeen, s_peerLocal;
static ZmRdvGame    s_list[ZM_RDV_LIST_MAX];
static int          s_listCount;
static bool         s_listHeard;
static unsigned int s_lastListMs;

// ---------------------------------------------------------------------------
// The server's address
// ---------------------------------------------------------------------------
bool zm_rdv_available(void)
{
    const char* s = g_zmRdvServer;
    while (*s == ' ') s++;
    if (*s == '\0') return false;
    return !(strncmp(s, "off", 3) == 0 || strncmp(s, "OFF", 3) == 0 || strcmp(s, "0") == 0);
}

static bool rdv_resolve(void)
{
    s_serverOk = false;
    if (!zm_rdv_available()) return false;
    char host[96];
    snprintf(host, sizeof(host), "%s", g_zmRdvServer);
    unsigned int port = ZM_RDV_DEFAULT_PORT;
    char* colon = strrchr(host, ':');
    if (colon != NULL) {
        *colon = '\0';
        unsigned int p = (unsigned int)atoi(colon + 1);
        if (p > 0 && p <= 65535) port = p;
    }
    if (!plat_net_resolve(host, (unsigned short)port, &s_server)) {
        dbg_printf("[rdv] could not resolve the match server %s\n", g_zmRdvServer);
        return false;
    }
    s_serverOk = true;
    return true;
}

// ---------------------------------------------------------------------------
// Packets
// ---------------------------------------------------------------------------
static void rdv_send(const PlatNetAddr* to, unsigned char type, const void* body, int bytes)
{
    unsigned char buf[sizeof(RdvHeader) + 64];
    if (bytes > 64) return;
    RdvHeader h = {};
    h.magic = ZM_RDV_MAGIC; h.protocol = ZM_RDV_PROTOCOL; h.type = type;
    memcpy(buf, &h, sizeof(h));
    if (bytes) memcpy(buf + sizeof(h), body, bytes);
    plat_net_send(to, buf, (int)sizeof(h) + bytes);
}

static RdvAddr rdv_addr(const PlatNetAddr& a)
{
    RdvAddr r = {};
    r.ip = a.ip; r.port = a.port;
    return r;
}

static PlatNetAddr rdv_plat(const RdvAddr& a)
{
    PlatNetAddr r;
    r.ip = a.ip; r.port = a.port;
    return r;
}

static bool rdv_same(const PlatNetAddr& a, const PlatNetAddr& b)
{
    return a.ip == b.ip && a.port == b.port;
}

static unsigned int rdv_random(void)
{
    unsigned int r = plat_time_ms() * 2654435761u ^ ((unsigned int)rand() << 16) ^ (unsigned int)rand();
    return r ? r : 1;
}

static PlatNetAddr rdv_local(void)
{
    PlatNetAddr local = {};
    if (!plat_net_local(&s_server, &local)) local.ip = local.port = 0;
    return local;
}

bool zm_rdv_is_packet(const unsigned char* buf, int len)
{
    if (len < (int)sizeof(RdvHeader)) return false;
    unsigned int magic;
    memcpy(&magic, buf, sizeof(magic));
    return magic == ZM_RDV_MAGIC;
}

static void rdv_fail(const char* text, const char* hint);
static void rdv_connected(const PlatNetAddr* host);

void zm_rdv_take(const unsigned char* buf, int len, const PlatNetAddr* from)
{
    const RdvHeader* h = (const RdvHeader*)buf;
    if (len < (int)sizeof(RdvHeader) || h->magic != ZM_RDV_MAGIC || h->protocol != ZM_RDV_PROTOCOL) return;
    const unsigned char* body = buf + sizeof(RdvHeader);
    int bytes = len - (int)sizeof(RdvHeader);
    bool fromServer = s_serverOk && rdv_same(*from, s_server);

    switch (h->type) {
    case ZM_RDV_REGISTERED: {
        if (!s_hostOn || !fromServer || bytes < (int)sizeof(RdvRegistered)) return;
        const RdvRegistered& r = *(const RdvRegistered*)body;
        if (r.code != s_code)
            dbg_printf("[rdv] game code %06u (public address %u.%u.%u.%u:%u)\n", r.code,
                       r.seen.ip >> 24, (r.seen.ip >> 16) & 255, (r.seen.ip >> 8) & 255, r.seen.ip & 255,
                       (unsigned)r.seen.port);
        s_code = r.code;
        s_hostState = ZM_RDV_HOST_REGISTERED;
        return;
    }
    case ZM_RDV_INTRO: {
        if (!s_hostOn || !fromServer || bytes < (int)sizeof(RdvIntro)) return;
        const RdvIntro& r = *(const RdvIntro*)body;
        if (r.code != s_code) return;
        PlatNetAddr seen = rdv_plat(r.seen);
        unsigned int now = plat_time_ms();
        RdvPunchTarget* t = NULL;
        for (int i = 0; i < RDV_PUNCH_SLOTS && t == NULL; i++)
            if (s_targets[i].used && rdv_same(s_targets[i].seen, seen)) t = &s_targets[i];
        for (int i = 0; i < RDV_PUNCH_SLOTS && t == NULL; i++)
            if (!s_targets[i].used || now - s_targets[i].untilMs < 0x80000000u) t = &s_targets[i];
        if (t == NULL) t = &s_targets[0];
        if (!t->used || !rdv_same(t->seen, seen)) {
            dbg_printf("[rdv] survivor at %u.%u.%u.%u:%u wants in: punching\n", seen.ip >> 24,
                       (seen.ip >> 16) & 255, (seen.ip >> 8) & 255, seen.ip & 255, (unsigned)seen.port);
            t->nonce = rdv_random();
        }
        t->used = true;
        t->seen = seen;
        t->local = rdv_plat(r.local);
        t->untilMs = now + RDV_HOST_PUNCH_MS;
        t->lastMs = 0;
        return;
    }
    case ZM_RDV_PUNCH: {
        // Answer from whatever address it came from: behind a NAT that gives
        // each destination its own port, that is the one that works.
        if (bytes < (int)sizeof(RdvPunch)) return;
        const RdvPunch& p = *(const RdvPunch*)body;
        bool ours = (s_hostOn && s_code && p.code == s_code) ||
                    (s_cState != C_IDLE && s_cState != C_FAILED && p.code == s_cCode);
        if (!ours) return;
        rdv_send(from, ZM_RDV_PUNCH_ACK, &p, sizeof(p));
        // The host's own punch got through: its NAT already lets us in.
        if (s_cState == C_PUNCH && p.code == s_cCode) rdv_connected(from);
        return;
    }
    case ZM_RDV_PUNCH_ACK: {
        if (bytes < (int)sizeof(RdvPunch) || s_cState != C_PUNCH) return;
        const RdvPunch& p = *(const RdvPunch*)body;
        if (p.code == s_cCode && p.nonce == s_cNonce) rdv_connected(from);
        return;
    }
    case ZM_RDV_PEER: {
        if (!fromServer || s_cState != C_ASK || bytes < (int)sizeof(RdvIntro)) return;
        const RdvIntro& r = *(const RdvIntro*)body;
        if (r.code != s_cCode) return;
        s_peerSeen = rdv_plat(r.seen);
        s_peerLocal = rdv_plat(r.local);
        s_cState = C_PUNCH;
        s_cPunchBeganMs = plat_time_ms();
        s_cLastPunchMs = 0;
        s_cLastJoinMs = s_cPunchBeganMs;
        dbg_printf("[rdv] game %06u is at %u.%u.%u.%u:%u: punching\n", s_cCode, s_peerSeen.ip >> 24,
                   (s_peerSeen.ip >> 16) & 255, (s_peerSeen.ip >> 8) & 255, s_peerSeen.ip & 255,
                   (unsigned)s_peerSeen.port);
        return;
    }
    case ZM_RDV_JOIN_FAIL: {
        if (!fromServer || s_cState != C_ASK || bytes < (int)sizeof(RdvJoinFail)) return;
        const RdvJoinFail& f = *(const RdvJoinFail*)body;
        if (f.code != s_cCode) return;
        switch (f.reason) {
        case ZM_RDV_FAIL_VERSION: rdv_fail("GAME VERSION MISMATCH", "UPDATE EVERY PLAYER TO THE SAME BUILD"); break;
        case ZM_RDV_FAIL_FULL:    rdv_fail("THAT GAME IS FULL", ""); break;
        case ZM_RDV_FAIL_BUSY:    rdv_fail("TOO MANY TRIES - WAIT A MINUTE", ""); break;
        default:                  rdv_fail("NO GAME WITH THAT CODE", "CHECK THE CODE WITH THE HOST"); break;
        }
        return;
    }
    case ZM_RDV_LIST: {
        if (!fromServer || bytes < (int)sizeof(RdvListHead)) return;
        const RdvListHead& head = *(const RdvListHead*)body;
        int n = head.count;
        if (n > ZM_RDV_LIST_MAX) n = ZM_RDV_LIST_MAX;
        if ((int)sizeof(RdvListHead) + n * (int)sizeof(RdvListEntry) > bytes)
            n = (bytes - (int)sizeof(RdvListHead)) / (int)sizeof(RdvListEntry);
        const RdvListEntry* e = (const RdvListEntry*)(body + sizeof(RdvListHead));
        s_listCount = 0;
        for (int i = 0; i < n; i++) {
            ZmRdvGame& g = s_list[s_listCount++];
            g.code = e[i].code; g.survivors = e[i].survivors; g.capacity = e[i].capacity; g.flags = e[i].flags;
        }
        s_listHeard = true;
        return;
    }
    default:
        return;
    }
}

// ---------------------------------------------------------------------------
// The host
// ---------------------------------------------------------------------------
void zm_rdv_host_begin(void)
{
    s_hostOn = false;
    s_code = 0;
    memset(s_targets, 0, sizeof(s_targets));
    memset(&s_lastRegister, 0, sizeof(s_lastRegister));
    if (!rdv_resolve()) {
        s_hostState = ZM_RDV_HOST_OFF;
        return;
    }
    s_hostOn = true;
    s_hostLocal = rdv_local();
    s_token = rdv_random();
    s_hostBeganMs = plat_time_ms();
    s_lastRegisterMs = 0;
    s_hostState = ZM_RDV_HOST_CONTACTING;
}

void zm_rdv_host_end(void)
{
    if (!s_hostOn) return;
    unsigned int token = s_token;
    for (int i = 0; i < 2; i++) rdv_send(&s_server, ZM_RDV_UNREGISTER, &token, sizeof(token));
    s_hostOn = false;
    s_code = 0;
    s_hostState = ZM_RDV_HOST_OFF;
}

static RdvRegister rdv_listing(void)
{
    RdvRegister r = {};
    r.token = s_token;
    r.version = (unsigned char)zm_net_protocol_version();
    int ai = zm_net_ai_level();
    int survivors = 0;
    for (int i = ai > 0 ? 0 : 1; i < zm_net_seat_limit(); i++)
        if (zm_net_char(i) >= 0) survivors++;
    r.survivors = (unsigned char)survivors;
    r.capacity = (unsigned char)(ai > 0 ? zm_net_seat_limit() : zm_net_seat_limit() - 1);
    r.flags = (unsigned char)((g_zmRdvPublic ? ZM_RDV_PUBLIC : 0) | (ai > 0 ? ZM_RDV_AI : 0) |
                              (zm_net_lobby_started() || zm_net_lobby_phase() != ZM_LOBBY_JOIN ? ZM_RDV_STARTED : 0));
    r.local = rdv_addr(s_hostLocal);
    return r;
}

static void rdv_host_frame(unsigned int now)
{
    RdvRegister cur = rdv_listing();
    bool changed = memcmp(&cur, &s_lastRegister, sizeof(cur)) != 0;
    unsigned int interval = s_code ? RDV_REGISTER_MS : RDV_REGISTER_FAST_MS;
    unsigned int since = now - s_lastRegisterMs;
    if (!s_lastRegisterMs || since >= interval || (changed && since >= RDV_REGISTER_MIN_MS)) {
        s_lastRegisterMs = now ? now : 1;
        s_lastRegister = cur;
        rdv_send(&s_server, ZM_RDV_REGISTER, &cur, sizeof(cur));
    }
    if (!s_code && now - s_hostBeganMs > RDV_NO_ANSWER_MS) s_hostState = ZM_RDV_HOST_NO_ANSWER;

    for (int i = 0; i < RDV_PUNCH_SLOTS; i++) {
        RdvPunchTarget& t = s_targets[i];
        if (!t.used) continue;
        if (now - t.untilMs < 0x80000000u) { t.used = false; continue; }
        if (t.lastMs && now - t.lastMs < RDV_PUNCH_MS) continue;
        t.lastMs = now ? now : 1;
        RdvPunch p = { s_code, t.nonce };
        rdv_send(&t.seen, ZM_RDV_PUNCH, &p, sizeof(p));
        if (t.local.ip && !rdv_same(t.local, t.seen)) rdv_send(&t.local, ZM_RDV_PUNCH, &p, sizeof(p));
    }
}

int zm_rdv_host_state(void) { return s_hostOn ? s_hostState : ZM_RDV_HOST_OFF; }
unsigned int zm_rdv_code(void) { return s_hostOn ? s_code : 0; }

// ---------------------------------------------------------------------------
// A survivor
// ---------------------------------------------------------------------------
static bool rdv_open_socket(void)
{
    if (s_ownSocket) return true;
    if (!rdv_resolve()) return false;
    if (!plat_net_open(0)) return false;
    s_ownSocket = true;
    return true;
}

static void rdv_pump(void)
{
    if (!s_ownSocket) return;
    static unsigned char buf[2048];
    PlatNetAddr from;
    for (int i = 0; i < 64 && s_ownSocket; i++) {
        int n = plat_net_recv(buf, sizeof(buf), &from);
        if (n <= 0) break;
        if (zm_rdv_is_packet(buf, n)) zm_rdv_take(buf, n, &from);
    }
}

void zm_rdv_close(void)
{
    if (s_ownSocket) plat_net_close();
    s_ownSocket = false;
    s_cState = C_IDLE;
    s_listCount = 0;
    s_listHeard = false;
}

bool zm_rdv_browse_begin(void)
{
    zm_rdv_close();
    if (!rdv_open_socket()) return false;
    s_lastListMs = 0;
    return true;
}

void zm_rdv_browse_frame(void)
{
    if (!s_ownSocket) return;
    rdv_pump();
    unsigned int now = plat_time_ms();
    if (!s_lastListMs || now - s_lastListMs >= RDV_LIST_MS) {
        s_lastListMs = now ? now : 1;
        RdvListReq q = {};
        q.version = (unsigned char)zm_net_protocol_version();
        rdv_send(&s_server, ZM_RDV_LIST_REQ, &q, sizeof(q));
    }
}

bool zm_rdv_list_heard(void) { return s_listHeard; }
int zm_rdv_list_count(void) { return s_listCount; }
const ZmRdvGame* zm_rdv_list(int i) { return i >= 0 && i < s_listCount ? &s_list[i] : NULL; }

static void rdv_fail(const char* text, const char* hint)
{
    dbg_printf("[rdv] game %06u: %s\n", s_cCode, text);
    zm_rdv_close();
    s_cState = C_FAILED;
    zm_net_fail(text, hint);
}

static void rdv_connected(const PlatNetAddr* host)
{
    dbg_printf("[rdv] game %06u: through to %u.%u.%u.%u:%u\n", s_cCode, host->ip >> 24,
               (host->ip >> 16) & 255, (host->ip >> 8) & 255, host->ip & 255, (unsigned)host->port);
    PlatNetAddr to = *host;
    // The socket is ZombieNet's from here.
    s_ownSocket = false;
    s_cState = C_DONE;
    s_listCount = 0;
    s_listHeard = false;
    if (s_cRejoin) zm_net_rejoin_peer(&to, s_cCode);
    else zm_net_join_peer(&to, -1, s_cCode);
    if (zm_net_status() != ZM_NET_JOINING) {
        s_cState = C_FAILED;
        plat_net_close();
    }
}

void zm_rdv_connect_begin(unsigned int code, bool rejoin)
{
    s_cState = C_IDLE;
    if (!rdv_open_socket()) {
        zm_rdv_close();
        s_cState = C_FAILED;
        zm_net_fail(zm_rdv_available() ? "COULD NOT REACH THE MATCH SERVER" : "NO MATCH SERVER SET",
                    zm_rdv_available() ? "CHECK YOUR INTERNET CONNECTION" : "SET RENDEZVOUS IN CONFIG.INI");
        return;
    }
    s_cCode = code;
    s_cRejoin = rejoin;
    s_cNonce = rdv_random();
    s_cBeganMs = plat_time_ms();
    s_cLastJoinMs = 0;
    s_cLocal = rdv_local();
    s_cState = C_ASK;
    dbg_printf("[rdv] joining game %06u%s\n", code, rejoin ? " (rejoin)" : "");
}

static void rdv_send_join(void)
{
    RdvJoin j = {};
    j.code = s_cCode;
    j.version = (unsigned char)zm_net_protocol_version();
    j.local = rdv_addr(s_cLocal);
    rdv_send(&s_server, ZM_RDV_JOIN, &j, sizeof(j));
}

int zm_rdv_connect_frame(void)
{
    rdv_pump();
    unsigned int now = plat_time_ms();
    switch (s_cState) {
    case C_ASK:
        if (now - s_cBeganMs >= RDV_JOIN_TIMEOUT_MS) {
            rdv_fail("THE MATCH SERVER DID NOT ANSWER", "CHECK YOUR INTERNET CONNECTION");
            break;
        }
        if (!s_cLastJoinMs || now - s_cLastJoinMs >= RDV_JOIN_MS) {
            s_cLastJoinMs = now ? now : 1;
            rdv_send_join();
        }
        break;
    case C_PUNCH:
        if (now - s_cPunchBeganMs >= RDV_PUNCH_TIMEOUT_MS) {
            rdv_fail("COULD NOT REACH THE HOST", "A NETWORK BLOCKS DIRECT LINKS - TRY A VPN SUCH AS TAILSCALE");
            break;
        }
        if (now - s_cLastJoinMs >= RDV_REJOIN_PUNCH_MS) {
            s_cLastJoinMs = now ? now : 1;
            rdv_send_join();
        }
        if (!s_cLastPunchMs || now - s_cLastPunchMs >= RDV_PUNCH_MS) {
            s_cLastPunchMs = now ? now : 1;
            RdvPunch p = { s_cCode, s_cNonce };
            rdv_send(&s_peerSeen, ZM_RDV_PUNCH, &p, sizeof(p));
            if (s_peerLocal.ip && !rdv_same(s_peerLocal, s_peerSeen))
                rdv_send(&s_peerLocal, ZM_RDV_PUNCH, &p, sizeof(p));
        }
        break;
    default:
        break;
    }
    if (s_cState == C_DONE) return ZM_RDV_CONNECT_DONE;
    if (s_cState == C_FAILED || s_cState == C_IDLE) return ZM_RDV_CONNECT_FAILED;
    return ZM_RDV_CONNECT_WORKING;
}

const char* zm_rdv_connect_text(void)
{
    switch (s_cState) {
    case C_ASK:   return "ASKING THE MATCH SERVER";
    case C_PUNCH: return "OPENING A DIRECT LINK TO THE HOST";
    case C_DONE:  return "CONNECTED";
    default:      return "";
    }
}

void zm_rdv_connect_cancel(void)
{
    if (s_cState == C_DONE) return;
    zm_rdv_close();
}

void zm_rdv_frame(void)
{
    if (s_hostOn) rdv_host_frame(plat_time_ms());
}
