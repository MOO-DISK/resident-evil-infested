#include "ZombieModeInternal.h"
#include "ZombieNet.h"
#include "ZombieBumperBarrier.h"
#include "ZombieReconnect.h"
#include "ZombieRendezvous.h"
#include "../../system/AssetPath.h"
#include "../../platform/platform.h"
#include "../../DebugPrint.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>

// ============================================================================
// ZombieNet.cpp - the zombie mod's multiplayer link. See ZombieNet.h.
//
// Every packet starts with the same header: magic, version, type, how many
// events ride along, the player the body is about (the sender, or the survivor
// a relayed packet came from), and the newest event id received on this link
// (the ack). Events are kept per link until acked, so a lost datagram only
// delays them; each carries its source and destination player, and the host
// passes on what is not for it.
// ============================================================================

char           g_zmNetAddress[64] = "127.0.0.1";
unsigned short g_zmNetPort = 27960;

#define ZM_NET_MAGIC      0x5A314552u   // "RE1Z"
// Version 49 moves the QUICK_DEBUG puzzle tools to the trap passage; copies
// must agree on their rooms, pickup flags and action slots. Version 54: the
// randomizer's route honours script-gated doors and furniture rooms, so a
// seed builds a different scenario. Version 55: puzzles hand out the heavy
// weapons and key items prefer leaf rooms. Version 56: the piano (ZM_EV_PIANO).
// Version 57: the bar's spawn spots leave the alcove out. Version 58: PIANO 5
// (finished: the tune plays on). Version 59: the back area's ways in (the
// battery, the keypad's note, a crest behind them) change every seed's scenario.
// Version 60: a room's pushed objects and puzzle flags (ZM_EV_ROOMSYNC).
// Version 61: the survivors' timeout (ZM_EV_TIMEOUT, the lobby's option bits).
// Version 62: the bathtub's small key is a supply (every seed's scenario).
// Version 63: ink ribbons are supplies; the main hall's Beretta and the vacant
// room's broken shotgun are taken out. Version 64: no ink ribbons among the
// supplies.
// Version 70: the survivors' voice lines (ZM_EV_VOICE).
// Version 72: a survivor's own match id (its HELLO token) seats it and
// reclaims that seat on a rejoin; the lobby's menu-driven flow.
// Version 73: monster kill time rewards and leaf-room time spheres.
// Version 74: the clock carries the latest time bonus for its fading HUD popup.
// Version 75: independent time spheres in leaf rooms without keys or crests.
// Version 76: survivor timeouts removed; poison pauses during boss regrouping.
// Version 77: no datagram over ZM_NET_DATAGRAM_MAX - STATE without the
// reconnect checkpoint (its own CHECKPOINT packet), variable-length ENEMIES
// entries, and FRAG pieces for any body still too large.
#define ZM_NET_VERSION    77
// The largest datagram sent. Under every virtual LAN's MTU (Hamachi 1404,
// Tailscale 1280, ZeroTier 2800) less the IP and UDP headers, so the system
// never splits one into IP fragments - Hamachi loses those.
#define ZM_NET_DATAGRAM_MAX 1200
#define ZM_NET_MESSAGE_MAX  16384       // a whole body, before FRAG pieces
#define ZM_NET_CHECKPOINT_MS      1000  // the reconnect checkpoint's refresh
#define ZM_NET_CHECKPOINT_MIN_MS  100   // ... and its fastest, when it changes
#define ZM_NET_GAME_TIMEOUT_MS 5000
#define ZM_NET_RECONNECT_MS 30000
#define ZM_NET_TIMEOUT_MS 30000         // generous: room loads and FMVs do not
                                        // poll the socket
#define ZM_NET_HELLO_MS   500

enum {
    PKT_HELLO = 1,        // survivor -> host: { character } (also the lobby keepalive)
    PKT_WELCOME = 2,      // host -> survivor: { your index, chars[4] }
    PKT_GO = 3,           // host -> survivors: leave the lobby, start the game
    PKT_STATE = 4,        // anyone: NetStateBody (relayed by the host)
    PKT_PING = 5,         // keepalive / event carrier
    PKT_BYE = 6,
    PKT_FLAGS = 7,        // survivor -> host, host -> all: the shared world flag banks
    PKT_ENEMIES = 8,      // a room owner's monsters (relayed by the host)
    PKT_READY = 9,        // survivor -> host: in the game, waiting at the barrier
    PKT_START = 10,       // host -> survivors: the barrier is down
    PKT_PAUSE = 11,
    PKT_RECOVERY_GET = 12,
    PKT_RECOVERY_CHUNK = 13,
    PKT_RECOVERY_READY = 14,
    PKT_REJECT = 15,
    PKT_BUMPER = 17,           // intro readiness / host phase + frozen roster
    PKT_VERSION_REJECT = 16,    // diagnostic only; decoded before the full header
    PKT_FRAG = 18,              // one piece of a body over ZM_NET_DATAGRAM_MAX: NetFragHeader + data
    PKT_CHECKPOINT = 19,        // survivor -> host: ZmReconnectPlayer (not relayed)
};

#pragma pack(push, 1)
struct NetHeader {
    unsigned int   magic;
    unsigned char  version;
    unsigned char  type;
    unsigned char  eventCount;
    unsigned char  origin;       // the player the body describes
    unsigned short ack;          // newest event id received on this link
    unsigned int epoch;          // rejects a previous process's packets
};

struct NetEvent {
    unsigned short id;           // per link
    unsigned char  kind;
    unsigned char  src, dst;     // players; dst ZM_NET_ALL = everyone else
    unsigned char  pad[3];
    short          args[8];
};

struct NetPoseBody {
    int            x, y, z;
    short          angle;
    unsigned char  jointCount, pad;
    short          root[3];
    short          rot[ZM_NET_JOINTS][3];
};

struct NetStateBody {
    unsigned char  stage, room, flags, jointCount;   // flags: 1 in game, 2 dead, 4 zombie override,
                                                     // 8 reacting to a hit, 0x10 spectating (the
                                                     // rest is the corpse, frozen where it fell); 0x40 owner playing a pair
    int            x, y, z;
    short          angle, health, maxHealth;
    unsigned char  zombieSlot, weapon;               // which monster the override poses;
                                                     // the item id in the survivor's hand
    short          root[3];
    short          rot[ZM_NET_JOINTS][3];
    NetPoseBody    zombie;
    unsigned char  viewStage, viewRoom;              // the room this copy has loaded (a
                                                     // spectator's is not its corpse's)
    unsigned char  camera, entranceDoor;            // camera and survivor's entry door index
    unsigned int boxRevision;
    ItemSlot box[48];              // host-only authoritative snapshot
};

// One ENEMIES entry; variable length. After it: rot[jointCount][3], then
// ZmDeathPlayback (flags 4), then the appearance (flags 8): hiddenMask,
// tintMask and one colour per set tintMask bit.
struct NetEnemyEntry {
    unsigned char  slot, id, state, flags;           // flags: 1 dead, 2 death settled,
                                                     // 4 death block, 8 appearance block
    short          health;
    unsigned short uid;                              // ZombieWorld uid (ZM_WORLD_UID_*)
    int            x, y, z;
    short          angle;
    unsigned char  jointCount, pad;
    short          root[3];
};
// The most one entry can take: every joint, the death block and every tint.
#define ZM_NET_ENEMY_MAX_BYTES ((int)(sizeof(NetEnemyEntry) + sizeof(short) * 3 * ZM_NET_JOINTS + \
    sizeof(ZmDeathPlayback) + sizeof(unsigned int) * (2 + ZM_NET_JOINTS)))
// A FRAG piece's own header; `type` and `bytes` are the whole body's.
struct NetFragHeader {
    unsigned short serial;
    unsigned char  index, count, type, pad;
    unsigned short bytes;
};

struct NetEnemiesHeader {
    unsigned char  count, flags, stage, room;        // flags: 1 adopt, 2 a ZmYawnBody follows the entries
};

struct NetLobbyBody {
    unsigned char  index;                            // WELCOME: the receiver's seat
    unsigned char  chars[ZM_NET_MAX_PLAYERS];        // 0xFF free, 0xFE director
    unsigned char  phase;                            // ZM_LOBBY_* (the host's)
    unsigned char  vetoUsed;                         // bit 0 the director's, bit 1 the survivors'
    unsigned char  votes;                            // 2 bits a survivor (seat 1 in bits 0-1): ZM_VOTE_*
    unsigned int   revision;                         // orders lobby snapshots across vetoes and GO
    unsigned int   seed;                             // the game's scenario seed (ZombieRandom.cpp)
    unsigned int token[2];
    unsigned int recoveryBytes;
    unsigned char options;                           // ZM_LOBBY_OPT_* (the host's)
};
#define ZM_LOBBY_OPT_AI_SHIFT   1                    // bits 1-3: the AI director's level (0 a human)
#define ZM_LOBBY_OPT_AI_MASK    0x0E
struct NetHelloBody { unsigned char character, vote; unsigned int revision, seed, token[2]; };
struct NetPauseBody { unsigned int serial, remainingMs; unsigned char missing, present; };
struct NetRecoveryChunk { unsigned int offset, total; unsigned short bytes; unsigned char data[900]; };
#pragma pack(pop)
#define ZM_NET_FRAG_DATA ((int)(ZM_NET_DATAGRAM_MAX - sizeof(NetHeader) - sizeof(NetFragHeader)))
// The bodies sent every frame or every second go whole, never as FRAG pieces.
static_assert(sizeof(NetHeader) + sizeof(NetStateBody) <= ZM_NET_DATAGRAM_MAX, "STATE over a datagram");
static_assert(sizeof(NetHeader) + sizeof(ZmReconnectPlayer) <= ZM_NET_DATAGRAM_MAX, "CHECKPOINT over a datagram");
static_assert(sizeof(NetHeader) + sizeof(NetRecoveryChunk) <= ZM_NET_DATAGRAM_MAX, "RECOVERY_CHUNK over a datagram");
static_assert(sizeof(NetHeader) + sizeof(NetLobbyBody) <= ZM_NET_DATAGRAM_MAX, "WELCOME over a datagram");

#define ZM_NET_MAX_PENDING 96          // a room exit sends one roster event per monster
#define ZM_NET_MAX_ENEMIES 30
#define ZM_NET_FLAGS_MS    1000
#define ZM_SEAT_FREE       0xFF
#define ZM_SEAT_DIRECTOR   0xFE

// One link: the host keeps one per survivor seat (1..3); a survivor keeps
// link 0, to the host.
struct NetLink {
    bool           used;
    PlatNetAddr    addr;
    unsigned int   lastHeardMs;
    bool           lost;
    NetEvent       pending[ZM_NET_MAX_PENDING];      // sent, not yet acked
    int            pendingCount;
    unsigned short nextEventId;
    unsigned short lastReceived;                     // newest taken from the other end
    unsigned int epoch, token[2], graceAt;
    bool recovering, expired, checkpointHave;
    ZmReconnectPlayer checkpoint;
    unsigned char recovery[ZM_RECONNECT_BLOB_MAX];
    int recoveryBytes;
};

// A body coming in as FRAG pieces, one per link: the pieces of one body go
// out back to back, so a piece of a newer body drops an unfinished older one.
struct NetFragIn {
    unsigned int   epoch;
    unsigned short serial;
    unsigned char  count, type;
    unsigned int   have;                             // bit per piece received
    int            bytes;
    unsigned char  data[ZM_NET_MESSAGE_MAX];
};
static NetFragIn      s_fragIn[ZM_NET_MAX_PLAYERS];
static unsigned short s_fragSerial = 0;
// The reconnect checkpoint last sent to the host (a survivor's copy).
static ZmReconnectPlayer s_checkpointSent;
static bool           s_checkpointSentHave = false;
static unsigned int   s_checkpointSentMs = 0;

static int            s_role = ZM_NET_OFF;
static int            s_status = ZM_NET_IDLE;
static int            s_self = -1;
static NetLink        s_links[ZM_NET_MAX_PLAYERS];
// The host's own survivor (an AI director's game): its live inventory.
static bool net_own_inventory(int player, unsigned char out[16])
{
    if (player != s_self || s_role != ZM_NET_ZOMBIE || zm_net_char(player) < 0 || g_ItemSlotsPointer == NULL)
        return false;
    memcpy(out, g_ItemSlotsPointer, 16);
    return true;
}

bool zm_net_has_item(int player, unsigned char item)
{
    unsigned char own[16];
    if (net_own_inventory(player, own)) {
        for (int i = 0; i < 8; i++)
            if (own[i * 2] == item && (own[i * 2 + 1] || item == ITEM_SHOTGUN)) return true;
        return false;
    }
    if (player < 1 || player >= ZM_NET_MAX_PLAYERS || !s_links[player].checkpointHave) return false;
    const ZmReconnectPlayer& p = s_links[player].checkpoint;
    for (int i = 0; i < 8; i++)
        if (p.inventory[i * 2] == item && (p.inventory[i * 2 + 1] || item == ITEM_SHOTGUN)) return true;
    return false;
}
static unsigned char  s_chars[ZM_NET_MAX_PLAYERS];
bool zm_net_inventory(int player, unsigned char out[16])
{
    if (net_own_inventory(player, out)) return true;
    if (player < 1 || player >= ZM_NET_MAX_PLAYERS || !s_links[player].checkpointHave) return false;
    ZmReconnectPlayer p = s_links[player].checkpoint;
    // Include committed transactions which have not yet appeared in STATE.
    zm_pickups_reconnect_reconcile(player, &p);
    zm_box_reconnect_reconcile(player, &p);
    zm_drops_reconnect_reconcile(player, &p);
    zm_shotgun_reconcile(player, &p);
    memcpy(out, p.inventory, 16);
    return true;
}
static int            s_myChar = ZM_CHAR_CHRIS;
static unsigned int   s_lastHelloMs = 0;
static bool           s_lobbyGo = false;
// The scenario seed: the host draws it when it starts hosting and hands it
// out with every WELCOME / GO, so every copy builds the same scenario.
static unsigned int   s_seed = 0;
static unsigned int   s_lobbyRevision = 0;
// The lobby's map review (ZombieLobby.cpp): the host's phase and veto flags,
// and each survivor's vote, which its HELLO carries.
static unsigned char  s_phase = ZM_LOBBY_JOIN;
static unsigned char  s_vetoUsed = 0;
static unsigned char  s_votes[ZM_NET_MAX_PLAYERS];
static unsigned char  s_myVote = ZM_VOTE_NONE;
static unsigned char  s_lobbyOptions = 0;            // ZM_LOBBY_OPT_*

// Inbox: events for this copy, in arrival order, with their source.
struct NetInboxEntry { NetEvent ev; };
static NetInboxEntry  s_inbox[ZM_NET_MAX_PENDING * 2];
static int            s_inboxCount = 0;
// The host's own survivor (an AI director's game) addresses its requests to
// the host - itself. They come back through zm_net_poll as if received, after
// the sender's frame, the way a remote survivor's would.
static NetEvent       s_loopback[ZM_NET_MAX_PENDING];
static int            s_loopbackCount = 0;
static NetEvent       s_pendingWin;
static bool           s_pendingWinHave = false; // confirmation received before game initialization

static ZmNetPeerState s_states[ZM_NET_MAX_PLAYERS];

// The game-start barrier.
static bool           s_ready[ZM_NET_MAX_PLAYERS];
static bool           s_started = false;
static signed char s_bumperReady[ZM_NET_MAX_PLAYERS] = { -1, -1, -1, -1 };
static unsigned char s_bumperPhase = 0;
static unsigned int s_bumperSent = 0;
static bool s_rejoining = false, s_downloaded = false;
static bool s_rejoinLoadReady = false;
static unsigned int s_downloadOffset, s_downloadTotal, s_lastDownloadMs;
static unsigned char s_download[ZM_RECONNECT_BLOB_MAX];
static unsigned int s_localLostAt, s_pauseRemaining, s_pauseReceived, s_pauseSerial;
static unsigned char s_pauseMissing;
static unsigned int s_pauseTotal, s_pauseBegan;
static bool s_clockPaused;
// Before the game starts, a survivor whose host left (BYE) or went silent is
// sent back to the NEW GAME menu: the session closes at the end of the poll.
static const char* s_kickText = NULL;
#define ZM_NET_LOBBY_TIMEOUT_MS 10000   // the host polls every frame in its menus
static void net_bumper_advance(void);
static const char* s_failureText = "COULD NOT OPEN THE CONNECTION";
static const char* s_failureHint = "";

static bool net_has_link_pause(unsigned int now)
{
    if (zombie_mode_match_over()) return false;
    if (s_status == ZM_NET_EXPIRED || s_status == ZM_NET_FAILED) return false;
    if (s_role == ZM_NET_ZOMBIE) {
        for (int i = 1; i < 4; i++) if (s_links[i].graceAt && !s_links[i].expired &&
            now - s_links[i].graceAt < ZM_NET_RECONNECT_MS) return true;
    } else if (s_role == ZM_NET_SURVIVOR) {
        if (s_localLostAt && now - s_localLostAt < ZM_NET_RECONNECT_MS) return true;
        if (s_pauseRemaining && now - s_pauseReceived < s_pauseRemaining) return true;
        if (s_rejoining && !s_downloaded) return true;
    }
    return false;
}
// A lost link holds the match on every copy.
static bool net_has_pause(unsigned int now)
{
    return net_has_link_pause(now);
}
bool zm_net_pause_active(void) { return net_has_pause(plat_time_ms()); }
bool zm_net_link_pause_active(void) { return net_has_link_pause(plat_time_ms()); }
unsigned int zm_game_time_ms(void)
{
    unsigned int now = plat_time_ms();
    bool pause = net_has_pause(now);
    if (pause && !s_clockPaused) { s_pauseBegan = now; s_clockPaused = true; }
    if (!pause && s_clockPaused) { s_pauseTotal += now - s_pauseBegan; s_clockPaused = false; }
    return (s_clockPaused ? s_pauseBegan : now) - s_pauseTotal;
}
int zm_net_pause_seconds(void)
{
    unsigned int now = plat_time_ms(), left = 0;
    if (s_role == ZM_NET_ZOMBIE) {
        for (int i = 1; i < 4; i++) if (s_links[i].graceAt && !s_links[i].expired) {
            unsigned int elapsed = now - s_links[i].graceAt;
            if (elapsed < ZM_NET_RECONNECT_MS && left < ZM_NET_RECONNECT_MS - elapsed)
                left = ZM_NET_RECONNECT_MS - elapsed;
        }
    } else if (s_localLostAt) {
        unsigned int elapsed = now - s_localLostAt;
        if (elapsed < ZM_NET_RECONNECT_MS) left = ZM_NET_RECONNECT_MS - elapsed;
    } else if (now - s_pauseReceived < s_pauseRemaining) left = s_pauseRemaining - (now - s_pauseReceived);
    return (int)((left + 999) / 1000);
}
bool zm_net_rejoining(void) { return s_rejoining; }
bool zm_net_rejoin_downloaded(void) { return s_downloaded; }

static unsigned int s_rdvCode;   // the rendezvous game code this survivor joined by (0: direct)

// The last match this copy joined: the host, the seed and this survivor's
// match id. One file - the id, not a seat number, finds the seat again.
// code: the rendezvous game code joined through (0: a direct address) - a
// rejoin through a NAT must be introduced again from its new socket.
struct NetRejoinCredential { unsigned int magic, seed, token[2], ip; unsigned short port; unsigned char seat, character; unsigned int code; };
static void net_credential_path(char* out, int len)
{
    snprintf(out, len, "%sinfestation-rejoin.dat", GetSaveRoot());
}
static void net_save_credential(void)
{
    if (s_self < 1 || s_self > 3) return;
    NetLink& L = s_links[0];
    NetRejoinCredential c = {};
    c.magic = 0x52454A43; c.seed = s_seed;
    c.token[0] = L.token[0]; c.token[1] = L.token[1];
    c.ip = L.addr.ip; c.port = L.addr.port; c.seat = (unsigned char)s_self; c.character = s_chars[s_self];
    c.code = s_rdvCode;
    char path[512]; net_credential_path(path, sizeof(path));
    plat_mkdir(GetSaveRoot());
    if (!plat_file_write_atomic(path, &c, sizeof(c))) dbg_printf("[rejoin] could not save seat %d credential\n", s_self);
}

const char* zm_char_name(int ch)
{
    switch (ch) {
    case ZM_CHAR_CHRIS: return "CHRIS";
    case ZM_CHAR_JILL:  return "JILL";
    case ZM_CHAR_BARRY: return "BARRY";
    case ZM_CHAR_REBECCA: return "REBECCA";
    case ZM_CHAR_RICHARD: return "RICHARD";
    case ZM_CHAR_ENRICO: return "ENRICO";
    default:            return "?";
    }
}

int zm_net_role(void) { return s_role; }
int zm_net_status(void) { return s_status; }
int zm_net_self(void) { return s_self; }

int zm_net_char(int player)
{
    if (player < 0 || player >= ZM_NET_MAX_PLAYERS) return -1;
    unsigned char c = s_chars[player];
    return (c < ZM_CHAR_COUNT) ? c : -1;
}

bool zm_net_char_taken(int character)
{
    for (int i = 0; i < ZM_NET_MAX_PLAYERS; i++) {
        if (i != s_self && s_chars[i] == character) return true;
    }
    return false;
}

// The host's ruling on a seat's pick: the wish if nobody else wears it, else
// what the seat already has, else the first free character.
static unsigned char net_grant_char(int seat, int wish)
{
    bool wishFree = wish >= 0 && wish < ZM_CHAR_COUNT;
    for (int i = 0; i < ZM_NET_MAX_PLAYERS && wishFree; i++) {
        if (i != seat && s_chars[i] == wish) wishFree = false;
    }
    if (wishFree) return (unsigned char)wish;
    if (s_chars[seat] < ZM_CHAR_COUNT) return s_chars[seat];
    for (int c = 0; c < ZM_CHAR_COUNT; c++) {
        bool free = true;
        for (int i = 0; i < ZM_NET_MAX_PLAYERS && free; i++) {
            if (i != seat && s_chars[i] == c) free = false;
        }
        if (free) return (unsigned char)c;
    }
    return ZM_CHAR_CHRIS;
}

int zm_net_survivor_count(void)
{
    int n = 0;
    for (int i = 0; i < ZM_NET_MAX_PLAYERS; i++) {
        if (zm_net_char(i) >= 0) n++;
    }
    return n;
}

const char* zm_net_status_text(void)
{
    switch (s_status) {
    case ZM_NET_HOSTING:   return "HOSTING - WAITING FOR SURVIVORS";
    case ZM_NET_JOINING:   return "CONNECTING";
    case ZM_NET_CONNECTED: return "CONNECTED";
    case ZM_NET_LOST:      return "CONNECTION LOST";
    case ZM_NET_FAILED:    return s_failureText;
    case ZM_NET_EXPIRED:   return "RECONNECT TIME EXPIRED";
    default:               return "";
    }
}

const char* zm_net_status_hint(void) { return s_status == ZM_NET_FAILED ? s_failureHint : ""; }

const ZmNetPeerState* zm_net_player(int player)
{
    if (player < 0 || player >= ZM_NET_MAX_PLAYERS || player == s_self) return NULL;
    return s_states[player].valid ? &s_states[player] : NULL;
}

bool zm_net_active(void)
{
    if (s_role == ZM_NET_OFF) return false;
    if (s_role == ZM_NET_ZOMBIE) return s_status == ZM_NET_HOSTING;
    return s_status == ZM_NET_CONNECTED || s_status == ZM_NET_LOST;
}

// ---------------------------------------------------------------------------
// Sending
// ---------------------------------------------------------------------------
static void net_send_link(int link, unsigned char type, unsigned char origin,
                          const void* body, int bodyLen)
{
    NetLink& L = s_links[link];
    if (!L.used) return;
    if (body == NULL || bodyLen < 0) bodyLen = 0;
    static unsigned char buf[ZM_NET_DATAGRAM_MAX];
    NetHeader* h = (NetHeader*)buf;
    h->magic = ZM_NET_MAGIC;
    h->version = ZM_NET_VERSION;
    h->origin = origin;
    h->ack = L.lastReceived;
    h->epoch = L.epoch;
    if ((int)sizeof(NetHeader) + bodyLen > ZM_NET_DATAGRAM_MAX) {
        // Too large for one datagram: FRAG pieces, reassembled by the other
        // end. They carry no events; the next packet does.
        if (bodyLen > ZM_NET_MESSAGE_MAX) {
            dbg_printf("[net] body of type %d too large (%d bytes), dropped\n", (int)type, bodyLen);
            return;
        }
        int count = (bodyLen + ZM_NET_FRAG_DATA - 1) / ZM_NET_FRAG_DATA;
        unsigned short serial = ++s_fragSerial;
        h->type = PKT_FRAG;
        h->eventCount = 0;
        NetFragHeader* f = (NetFragHeader*)(buf + sizeof(NetHeader));
        unsigned char* data = buf + sizeof(NetHeader) + sizeof(NetFragHeader);
        for (int i = 0; i < count; i++) {
            int off = i * ZM_NET_FRAG_DATA;
            int n = bodyLen - off < ZM_NET_FRAG_DATA ? bodyLen - off : ZM_NET_FRAG_DATA;
            f->serial = serial;
            f->index = (unsigned char)i;
            f->count = (unsigned char)count;
            f->type = type;
            f->pad = 0;
            f->bytes = (unsigned short)bodyLen;
            memcpy(data, (const unsigned char*)body + off, n);
            zm_rdv_net_send(&L.addr, buf, (int)(sizeof(NetHeader) + sizeof(NetFragHeader)) + n);
        }
        return;
    }
    h->type = type;
    // Events first, as many as leave room for the body (the rest go next time).
    int room = ZM_NET_DATAGRAM_MAX - (int)sizeof(NetHeader) - bodyLen;
    int n = L.pendingCount;
    if (n * (int)sizeof(NetEvent) > room) n = room / (int)sizeof(NetEvent);
    if (n < 0) n = 0;
    h->eventCount = (unsigned char)n;
    int len = sizeof(NetHeader);
    memcpy(buf + len, L.pending, sizeof(NetEvent) * n);
    len += (int)sizeof(NetEvent) * n;
    if (bodyLen > 0) {
        memcpy(buf + len, body, bodyLen);
        len += bodyLen;
    }
    zm_rdv_net_send(&L.addr, buf, len);
    // Events that did not fit: a PING of their own, so a burst (a room exit's
    // roster) is not held back behind a few frames of large bodies.
    if (n < L.pendingCount && bodyLen > 0) {
        int more = L.pendingCount - n;
        int fit = (ZM_NET_DATAGRAM_MAX - (int)sizeof(NetHeader)) / (int)sizeof(NetEvent);
        if (more > fit) more = fit;
        h->type = PKT_PING;
        h->eventCount = (unsigned char)more;
        memcpy(buf + sizeof(NetHeader), L.pending + n, sizeof(NetEvent) * more);
        zm_rdv_net_send(&L.addr, buf, (int)sizeof(NetHeader) + (int)sizeof(NetEvent) * more);
    }
}

// To every link (the host: each survivor; a survivor: the host), except one.
static void net_send_all(unsigned char type, unsigned char origin, const void* body, int bodyLen,
                         int exceptLink)
{
    for (int i = 0; i < ZM_NET_MAX_PLAYERS; i++) {
        if (i != exceptLink && s_links[i].used && !s_links[i].lost) {
            net_send_link(i, type, origin, body, bodyLen);
        }
    }
}

static void net_queue(int link, const NetEvent& ev)
{
    NetLink& L = s_links[link];
    if (!L.used) return;
    // Keep space for gameplay transactions while a burst of visual/roster
    // traffic is awaiting acknowledgement. Never evict an assigned sequence.
    bool critical = ev.kind == ZM_EV_PICKUP || ev.kind == ZM_EV_BOX || ev.kind == ZM_EV_DROP ||
                    ev.kind == ZM_EV_WIN || ev.kind == ZM_EV_REVIVE || ev.kind == ZM_EV_SHOTGUN ||
                    ev.kind == ZM_EV_PIANO || ev.kind == ZM_EV_BOSS ||
                    ev.kind == ZM_EV_TIME_PICKUP;
    int limit = critical ? ZM_NET_MAX_PENDING : ZM_NET_MAX_PENDING - 24;
    if (L.pendingCount >= limit) {
        dbg_printf("[net] link %d event queue full, dropping kind %d\n", link, (int)ev.kind);
        return;
    }
    NetEvent& e = L.pending[L.pendingCount++];
    e = ev;
    e.id = L.nextEventId++;
}

// Where an event for `dst` goes from this copy: the host sends straight to the
// survivor's link (or every link); a survivor always sends to the host.
static void net_route(const NetEvent& ev, int fromLink)
{
    if (s_role == ZM_NET_SURVIVOR) {
        net_queue(0, ev);
        return;
    }
    for (int i = 1; i < ZM_NET_MAX_PLAYERS; i++) {
        // A committed revive is echoed to its sender too: everybody observes
        // the host's accepted order, including simultaneous revivers.
        if ((i == fromLink && ev.kind != ZM_EV_REVIVE) || !s_links[i].used) continue;
        if (ev.dst == ZM_NET_ALL || ev.dst == i) net_queue(i, ev);
    }
}

void zm_net_send_event_to(int dst, int kind, short a0, short a1, short a2, short a3,
                          short a4, short a5, short a6, short a7)
{
    if (!zm_net_active()) return;
    NetEvent ev;
    memset(&ev, 0, sizeof(ev));
    ev.kind = (unsigned char)kind;
    ev.src = (unsigned char)s_self;
    ev.dst = (unsigned char)dst;
    ev.args[0] = a0; ev.args[1] = a1; ev.args[2] = a2; ev.args[3] = a3;
    ev.args[4] = a4; ev.args[5] = a5; ev.args[6] = a6; ev.args[7] = a7;
    if (dst == s_self) {
        if (s_role != ZM_NET_ZOMBIE || zm_net_char(s_self) < 0) return;
        if (s_loopbackCount < ZM_NET_MAX_PENDING) s_loopback[s_loopbackCount++] = ev;
        else dbg_printf("[net] loopback full, dropping kind %d\n", (int)kind);
        return;
    }
    net_route(ev, -1);
    net_send_all(PKT_PING, (unsigned char)s_self, NULL, 0, -1);   // out now
}

void zm_net_send_event8(int kind, short a0, short a1, short a2, short a3,
                        short a4, short a5, short a6, short a7)
{
    zm_net_send_event_to(ZM_NET_ALL, kind, a0, a1, a2, a3, a4, a5, a6, a7);
}

void zm_net_send_event(int kind, short a, short b, short c, short d)
{
    zm_net_send_event8(kind, a, b, c, d, 0, 0, 0, 0);
}

bool zm_net_take_event(int* kind, short args[8], int* src)
{
    if (s_inboxCount == 0) return false;
    const NetEvent& e = s_inbox[0].ev;
    *kind = e.kind;
    *src = e.src;
    for (int i = 0; i < 8; i++) args[i] = e.args[i];
    memmove(s_inbox, s_inbox + 1, sizeof(NetInboxEntry) * (s_inboxCount - 1));
    s_inboxCount--;
    return true;
}

static void net_fill_pose(NetPoseBody* out, const Entity* e)
{
    out->x = e->scaMatrixData.localMatrix.t[0];
    out->y = e->scaMatrixData.localMatrix.t[1];
    out->z = e->scaMatrixData.localMatrix.t[2];
    out->angle = e->angle;
    const JointStruct* j = e->jointsStructs;
    int n = e->jointCount < ZM_NET_JOINTS ? e->jointCount : ZM_NET_JOINTS;
    out->jointCount = (unsigned char)n;
    if (j != NULL && n > 0) {
        out->root[0] = (short)j[0].transform.t[0];
        out->root[1] = (short)j[0].transform.t[1];
        out->root[2] = (short)j[0].transform.t[2];
        for (int i = 0; i < n; i++) {
            out->rot[i][0] = j[i].rotation.x;
            out->rot[i][1] = j[i].rotation.y;
            out->rot[i][2] = j[i].rotation.z;
        }
    }
}

// A dead survivor watching the others (ZombieSpectate.cpp): its STATE stays
// the corpse as it lay when the watching began - stage, room, spot and pose -
// so the other copies keep showing the body there, whatever room this copy
// has loaded since. Flag 0x10 tells them this player is in no room at all.
static bool         s_freezeOn = false;
static bool         s_frozenHave = false;
static NetStateBody s_frozen;
static bool s_departing = false;

void zm_net_freeze_state(bool on)
{
    s_freezeOn = on;
    s_frozenHave = false;
}

// A survivor's reconnect checkpoint (the host keeps the newest per seat for a
// rejoin, the seat's loot and its inventory checks), in a packet of its own
// rather than every STATE: at once when anything but the position changed,
// else once a second.
static void net_send_checkpoint(const ZmReconnectPlayer& cp)
{
    unsigned int now = plat_time_ms();
    bool changed = !s_checkpointSentHave;
    if (!changed) {
        ZmReconnectPlayer a = cp, was = s_checkpointSent;
        a.sequence = was.sequence = 0;
        a.x = was.x = 0; a.y = was.y = 0; a.z = was.z = 0;
        a.angle = was.angle = 0;
        changed = memcmp(&a, &was, sizeof(a)) != 0;
    }
    unsigned int since = now - s_checkpointSentMs;
    unsigned int wait = changed ? ZM_NET_CHECKPOINT_MIN_MS : ZM_NET_CHECKPOINT_MS;
    if (s_checkpointSentHave && since < wait) return;
    s_checkpointSent = cp;
    s_checkpointSentHave = true;
    s_checkpointSentMs = now;
    net_send_link(0, PKT_CHECKPOINT, (unsigned char)s_self, &cp, sizeof(cp));
}

void zm_net_send_state(const Entity* ch, bool inGame, const Entity* zombieOverride, int overrideSlot, bool departing)
{
    if (!zm_net_active()) return;
    if (zm_net_pause_active()) return;
    if (departing) s_departing = true;
    else if ((g_main_state_flags & MSF_GAMEPLAY_ACTIVE) == 0) s_departing = false;
    NetStateBody b;
    memset(&b, 0, sizeof(b));
    b.stage = g_stageId;
    b.room = g_roomId;
    b.flags = inGame ? 1 : 0;
    if (ch != NULL) {
        b.x = ch->scaMatrixData.localMatrix.t[0];
        b.y = ch->scaMatrixData.localMatrix.t[1];
        b.z = ch->scaMatrixData.localMatrix.t[2];
        b.angle = ch->angle;
        b.health = ch->health;
        if (ch == (const Entity*)&g_playerEntity) {
            b.weapon = g_playerEntity.equippedWeaponId;
            if (g_playerEntity.isBeingAttackedFlag != 0) b.flags |= 8;
        }
        if (ch->health < 0) b.flags |= 2;
        const JointStruct* j = ch->jointsStructs;
        int n = ch->jointCount < ZM_NET_JOINTS ? ch->jointCount : ZM_NET_JOINTS;
        b.jointCount = (unsigned char)n;
        if (j != NULL && n > 0) {
            b.root[0] = (short)j[0].transform.t[0];
            b.root[1] = (short)j[0].transform.t[1];
            b.root[2] = (short)j[0].transform.t[2];
            for (int i = 0; i < n; i++) {
                b.rot[i][0] = j[i].rotation.x;
                b.rot[i][1] = j[i].rotation.y;
                b.rot[i][2] = j[i].rotation.z;
            }
        }
    }
    if (zombieOverride != NULL) {
        b.flags |= 4;
        b.zombieSlot = (unsigned char)overrideSlot;
        net_fill_pose(&b.zombie, zombieOverride);
    }
    if (s_freezeOn) {
        if (!s_frozenHave) {
            // This frame's state is still the corpse's room: keep it.
            s_frozen = b;
            s_frozen.flags &= (unsigned char)~(4 | 8);
            s_frozenHave = true;
        }
        b = s_frozen;
        b.flags = (unsigned char)((inGame ? 1 : 0) | 2 | 0x10);
    }
    if (inGame && !s_freezeOn && !s_departing && zombie_mode_room_pair_busy()) b.flags |= 0x40;
    b.viewStage = g_stageId;
    if (s_departing && !s_freezeOn) b.flags |= 0x20;
    b.viewRoom = g_roomId;
    b.camera = g_roomCameraId;
    b.entranceDoor = zm_survivor_entrance_door();
    if (s_role == ZM_NET_SURVIVOR && inGame && !s_departing && zombie_mode_armed()) {
        ZmReconnectPlayer cp;
        zm_reconnect_capture(&cp);
        if (s_freezeOn) {
            cp.card.stageId = b.stage; cp.card.roomId = b.room;
            cp.card.roomCameraId = 0;
            cp.x = b.x; cp.y = b.y; cp.z = b.z;
            cp.angle = b.angle; cp.health = b.health;
        }
        net_send_checkpoint(cp);
    }
    if (s_role == ZM_NET_ZOMBIE) b.boxRevision = zm_box_snapshot(b.box);
    net_send_all(PKT_STATE, (unsigned char)s_self, &b, sizeof(b), -1);
}

void zm_net_send_enemies(const Entity* const* list, const unsigned short* uids, int count, bool adopt)
{
    if (!zm_net_active()) return;
    static unsigned char body[sizeof(NetEnemiesHeader) + ZM_NET_ENEMY_MAX_BYTES * ZM_NET_MAX_ENEMIES +
                              sizeof(ZmYawnBody)];
    static_assert(sizeof(body) <= ZM_NET_MESSAGE_MAX, "a full room's ENEMIES over FRAG's limit");
    NetEnemiesHeader* h = (NetEnemiesHeader*)body;
    if (count > ZM_NET_MAX_ENEMIES) count = ZM_NET_MAX_ENEMIES;
    h->count = (unsigned char)count;
    h->flags = adopt ? 1 : 0;
    h->stage = g_stageId;
    h->room = g_roomId;
    int len = (int)sizeof(NetEnemiesHeader);
    for (int i = 0; i < count; i++) {
        const Entity* e = list[i];
        NetEnemyEntry* out = (NetEnemyEntry*)(body + len);
        memset(out, 0, sizeof(*out));
        out->slot = (unsigned char)(e - g_EnemiesList);
        out->id = e->id;
        out->state = e->state;
        out->flags = zm_monster_dead(e) ? 1 : 0;
        if (zm_monster_death_settled(e)) out->flags |= 2;
        out->health = e->health;
        out->uid = uids[i];
        NetPoseBody pose;
        memset(&pose, 0, sizeof(pose));
        net_fill_pose(&pose, e);
        out->x = pose.x; out->y = pose.y; out->z = pose.z;
        out->angle = pose.angle;
        out->jointCount = pose.jointCount;
        memcpy(out->root, pose.root, sizeof(out->root));
        len += (int)sizeof(NetEnemyEntry);
        // Only the joints it has.
        memcpy(body + len, pose.rot, sizeof(short) * 3 * pose.jointCount);
        len += (int)sizeof(short) * 3 * pose.jointCount;
        if (e->health < 0) {
            out->flags |= 4;
            ZmDeathPlayback d;
            memset(&d, 0, sizeof(d));
            const unsigned char* raw = (const unsigned char*)e;
            d.status = e->status_flags;
            d.behavior = e->behavior_flags;
            memcpy(d.movement, raw + 0x6C, sizeof(d.movement));
            memcpy(d.state, raw + 0x84, sizeof(d.state));
            memcpy(d.animation, raw + 0xBC, sizeof(d.animation));
            memcpy(d.phase, raw + 0x16C, sizeof(d.phase));
            memcpy(d.extra, raw + 0x170, zm_death_extra_bytes(e->id));
            memcpy(d.tail, raw + 0x178, sizeof(d.tail));
            memcpy(body + len, &d, sizeof(d));
            len += (int)sizeof(d);
        }
        ZmMonsterAppearance app;
        zm_monster_appearance_capture(e, &app);
        if (app.hiddenMask != 0 || app.tintMask != 0) {
            // The masks, then a colour for each tinted joint only.
            out->flags |= 8;
            memcpy(body + len, &app.hiddenMask, sizeof(app.hiddenMask));
            memcpy(body + len + 4, &app.tintMask, sizeof(app.tintMask));
            len += 8;
            for (int j = 0; j < ZM_NET_JOINTS; j++) {
                if ((app.tintMask & (1u << j)) == 0) continue;
                memcpy(body + len, &app.color[j], sizeof(app.color[j]));
                len += (int)sizeof(app.color[j]);
            }
        }
    }
    // Yawn's body, which its head's pose cannot carry (ZombieYawn.cpp).
    bool yawn = false;
    for (int i = 0; i < count && !yawn; i++) yawn = zm_yawn_head(list[i]);
    ZmYawnBody yb;
    if (yawn && zm_yawn_body_capture(&yb)) {
        h->flags |= 2;
        memcpy(body + len, &yb, sizeof(yb));
        len += (int)sizeof(yb);
    }
    net_send_all(PKT_ENEMIES, (unsigned char)s_self, body, len, -1);
}

static void net_send_lobby(int link, unsigned char type)
{
    NetLobbyBody b;
    memset(&b, 0, sizeof(b));
    b.index = (unsigned char)link;
    memcpy(b.chars, s_chars, sizeof(b.chars));
    b.revision = s_lobbyRevision;
    b.seed = s_seed;
    b.phase = s_phase;
    b.token[0] = s_links[link].token[0]; b.token[1] = s_links[link].token[1];
    b.recoveryBytes = s_links[link].recovering ? s_links[link].recoveryBytes : 0;
    b.vetoUsed = s_vetoUsed;
    b.options = s_lobbyOptions;
    for (int i = 1; i < ZM_NET_MAX_PLAYERS; i++) b.votes |= (unsigned char)((s_votes[i] & 3) << ((i - 1) * 2));
    net_send_link(link, type, (unsigned char)s_self, &b, sizeof(b));
}

// ---------------------------------------------------------------------------
// Receiving
// ---------------------------------------------------------------------------
static void net_take_state(int origin, const unsigned char* body, int bodyLen)
{
    if (origin < 0 || origin >= ZM_NET_MAX_PLAYERS || origin == s_self) return;
    if (bodyLen < (int)sizeof(NetStateBody)) return;
    const NetStateBody* b = (const NetStateBody*)body;
    if (s_role == ZM_NET_SURVIVOR && origin == ZM_NET_DIRECTOR)
        zm_box_snapshot_take(b->boxRevision, b->box);
    ZmNetPeerState& p = s_states[origin];
    // Only a peer crossing into our loaded room counts as an arrival. Repeated
    // STATEs, first sightings and spectators do not produce a door cue.
    if (zombie_mode_live_menu() && p.valid && (b->flags & 1) != 0 &&
        (b->flags & 0x10) == 0 && b->stage == g_stageId && b->room == g_roomId &&
        (p.stage != b->stage || p.room != b->room)) {
        zombie_mode_room_entry_sound();
    }
    // In-game STATE getting through (the lobby's small packets can, alone).
    if (!p.valid && (b->flags & 1) != 0)
        dbg_printf("[net] STATE from player %d: in game, stage %d room %02X\n", origin, (int)b->stage, (int)b->room);
    p.valid = (b->flags & 1) != 0;
    p.dead = (b->flags & 2) != 0;
    p.spectating = (b->flags & 0x10) != 0;
    p.transitioning = (b->flags & 0x20) != 0;
    p.roomPairBusy = (b->flags & 0x40) != 0;
    p.viewStage = b->viewStage;
    p.viewRoom = b->viewRoom;
    p.camera = b->camera;
    p.entranceDoor = b->entranceDoor;
    p.stage = b->stage;
    p.room = b->room;
    p.x = b->x; p.y = b->y; p.z = b->z;
    p.angle = b->angle;
    p.health = b->health;
    p.jointCount = b->jointCount < ZM_NET_JOINTS ? b->jointCount : ZM_NET_JOINTS;
    memcpy(p.root, b->root, sizeof(p.root));
    memcpy(p.rot, b->rot, sizeof(p.rot));
    p.receivedMs = zm_game_time_ms();
    p.weapon = b->weapon;
    p.attacked = (b->flags & 8) != 0;
    p.hasZombie = (b->flags & 4) != 0;
    p.zombieSlot = b->zombieSlot;
    if (p.hasZombie) {
        p.zombie.x = b->zombie.x;
        p.zombie.y = b->zombie.y;
        p.zombie.z = b->zombie.z;
        p.zombie.angle = b->zombie.angle;
        p.zombie.jointCount = b->zombie.jointCount < ZM_NET_JOINTS ? b->zombie.jointCount : ZM_NET_JOINTS;
        memcpy(p.zombie.root, b->zombie.root, sizeof(p.zombie.root));
        memcpy(p.zombie.rot, b->zombie.rot, sizeof(p.zombie.rot));
    }
}

// The host: a survivor's reconnect checkpoint (PKT_CHECKPOINT).
static void net_take_checkpoint(int link, const unsigned char* body, int bodyLen)
{
    if (s_role != ZM_NET_ZOMBIE || link <= 0 || link >= ZM_NET_MAX_PLAYERS) return;
    if (bodyLen != (int)sizeof(ZmReconnectPlayer)) return;
    ZmReconnectPlayer cp;
    memcpy(&cp, body, sizeof(cp));
    if (!cp.sequence) return;
    NetLink& L = s_links[link];
    if (L.checkpointHave && (int)(cp.sequence - L.checkpoint.sequence) <= 0) return;
    // (A departing survivor sends none: its card is mid-door.)
    if (!L.recovering && !L.expired &&
        cp.card.totalHeldItems <= 8 && cp.card.stageId <= 6 && cp.card.roomId < 58) {
        L.checkpoint = cp;
        L.checkpointHave = true;
        zm_box_checkpoint(link, cp.boxToken);
    }
}

static void net_take_enemies(int origin, const unsigned char* body, int bodyLen)
{
    if (bodyLen < (int)sizeof(NetEnemiesHeader)) return;
    const NetEnemiesHeader* eh = (const NetEnemiesHeader*)body;
    int count = eh->count;
    if (count > ZM_NET_MAX_ENEMIES) return;
    static ZmNetEnemy got[ZM_NET_MAX_ENEMIES];
    int off = (int)sizeof(NetEnemiesHeader);
    for (int i = 0; i < count; i++) {
        if (off + (int)sizeof(NetEnemyEntry) > bodyLen) return;
        NetEnemyEntry in;
        memcpy(&in, body + off, sizeof(in));
        off += (int)sizeof(NetEnemyEntry);
        if (in.jointCount > ZM_NET_JOINTS) return;
        ZmNetEnemy& g = got[i];
        memset(&g, 0, sizeof(g));
        int rotBytes = (int)sizeof(short) * 3 * in.jointCount;
        if (off + rotBytes > bodyLen) return;
        memcpy(g.pose.rot, body + off, rotBytes);
        off += rotBytes;
        if ((in.flags & 4) != 0) {
            if (off + (int)sizeof(ZmDeathPlayback) > bodyLen) return;
            memcpy(&g.death, body + off, sizeof(g.death));
            off += (int)sizeof(ZmDeathPlayback);
        }
        if ((in.flags & 8) != 0) {
            if (off + 8 > bodyLen) return;
            memcpy(&g.appearance.hiddenMask, body + off, 4);
            memcpy(&g.appearance.tintMask, body + off + 4, 4);
            off += 8;
            for (int j = 0; j < ZM_NET_JOINTS; j++) {
                if ((g.appearance.tintMask & (1u << j)) == 0) continue;
                if (off + 4 > bodyLen) return;
                memcpy(&g.appearance.color[j], body + off, 4);
                off += 4;
            }
        }
        g.uid = in.uid;
        g.slot = in.slot;
        g.id = in.id;
        g.state = in.state;
        g.dead = (in.flags & 1) != 0;
        g.settled = (in.flags & 2) != 0;
        g.health = in.health;
        g.pose.x = in.x;
        g.pose.y = in.y;
        g.pose.z = in.z;
        if (s_role == ZM_NET_ZOMBIE) zm_world_reconnect_enemy(eh->stage, eh->room, g.uid,
            g.id, g.health, !g.dead, g.pose.x, g.pose.y, g.pose.z, in.angle);
        g.pose.angle = in.angle;
        g.pose.jointCount = in.jointCount;
        memcpy(g.pose.root, in.root, sizeof(g.pose.root));
    }
    if ((eh->flags & 2) != 0 && bodyLen >= off + (int)sizeof(ZmYawnBody)) {
        ZmYawnBody yb;
        memcpy(&yb, body + off, sizeof(yb));
        zm_yawn_body_take(yb, eh->stage, eh->room, origin);
    }
    zm_shared_receive_enemies(got, count, (eh->flags & 1) != 0, eh->stage, eh->room, origin);
}

// A new event off link `link`: for this copy, for others (the host passes it
// on), or both.
static void net_take_event(const NetEvent& e, int link)
{
    // A survivor's link carries only its own events: the host passes them on
    // to the others, which trust src (the director's word among them).
    if (s_role == ZM_NET_ZOMBIE && e.src != link) return;
    if (e.src >= ZM_NET_MAX_PLAYERS) return;
    if (e.kind == ZM_EV_SHOTGUN) {
        if (s_role == ZM_NET_ZOMBIE) {
            if (e.src != link || e.dst != ZM_NET_DIRECTOR) return;
        } else if (s_role != ZM_NET_SURVIVOR || link != ZM_NET_DIRECTOR ||
                   e.src != ZM_NET_DIRECTOR || (e.dst != ZM_NET_ALL && e.dst != s_self)) return;
        zm_shotgun_take(e.args, e.src);
        return;
    }
    if (e.kind == ZM_EV_TIME_PICKUP) {
        if (s_role != ZM_NET_SURVIVOR || link != ZM_NET_DIRECTOR || e.src != ZM_NET_DIRECTOR) return;
        zm_time_pickups_take(e.args, e.src);
        return;
    }
    if (e.kind == ZM_EV_BOSS) {
        // Only the host decides a boss fight's phases and its entry; a
        // survivor asks it to enter (3) and answers its question (5), for itself.
        if (s_role == ZM_NET_ZOMBIE) {
            if (e.src != link || e.dst != ZM_NET_DIRECTOR || (e.args[0] != 3 && e.args[0] != 5) ||
                e.args[2] != link) return;
        } else if (s_role != ZM_NET_SURVIVOR || link != ZM_NET_DIRECTOR || e.src != ZM_NET_DIRECTOR ||
                   (e.dst != ZM_NET_ALL && e.dst != s_self) || e.args[0] == 3 || e.args[0] == 5) return;
        zm_yawn_take(e.args, e.src);
        return;
    }
    if (e.kind == ZM_EV_PIANO) {
        // A survivor reports only its own playing: start / stop go on to
        // everybody, the finished tune only to the host. Only the host opens.
        if (s_role == ZM_NET_ZOMBIE) {
            if (e.src != link || e.args[1] != link || e.args[0] < 1 || e.args[0] > 5 || e.args[0] == 4) return;
            if (e.args[0] != 3 && e.dst == ZM_NET_ALL) net_route(e, link);
        } else if (s_role != ZM_NET_SURVIVOR || link != ZM_NET_DIRECTOR || e.args[0] == 3 ||
                   (e.args[0] == 4 && e.src != ZM_NET_DIRECTOR) ||
                   // The host plays the piano itself only against the AI director.
                   (e.args[0] != 4 && e.src == ZM_NET_DIRECTOR && zm_net_char(ZM_NET_DIRECTOR) < 0)) return;
        zm_piano_take(e.args, e.src);
        return;
    }
    if (e.kind == ZM_EV_PICKUP || e.kind == ZM_EV_BOX) {
        if (s_role == ZM_NET_ZOMBIE) {
            if (e.src != link || e.dst != ZM_NET_DIRECTOR) return;
        } else if (s_role != ZM_NET_SURVIVOR || link != ZM_NET_DIRECTOR ||
                   e.src != ZM_NET_DIRECTOR || (e.dst != s_self && e.dst != ZM_NET_ALL)) return;
        if (e.kind == ZM_EV_PICKUP) zm_pickups_take(e.args, e.src);
        else zm_box_take(e.args, e.src);
        return;
    }
    if (e.kind == ZM_EV_DROP || e.kind == ZM_EV_DROP_TAKE) {
        // Drop creation must be visible to the arbiter before a following
        // pickup request, even when effects have filled the ordinary inbox.
        if (s_role == ZM_NET_ZOMBIE) {
            if (e.kind != ZM_EV_DROP || e.src != link || e.dst != ZM_NET_ALL ||
                ((unsigned short)e.args[0] >> 12) != link) return;
            net_route(e, link);
        } else if (s_role != ZM_NET_SURVIVOR || link != ZM_NET_DIRECTOR || e.dst != ZM_NET_ALL ||
                   (e.kind == ZM_EV_DROP_TAKE && e.src != ZM_NET_DIRECTOR)) return;
        zm_drops_take_event(e.kind, e.args, e.src);
        return;
    }
    if (e.kind == ZM_EV_WIN) {
        // A survivor can request only its own escape, privately to the host.
        // Never relay that request as a result or allow it to forge host src.
        if (s_role == ZM_NET_ZOMBIE) {
            if (e.src != link || e.dst != ZM_NET_DIRECTOR || e.args[0] != link || e.args[1] != ZM_END_ESCAPE) return;
        } else if (s_role != ZM_NET_SURVIVOR || link != ZM_NET_DIRECTOR ||
                   e.src != ZM_NET_DIRECTOR || e.dst != ZM_NET_ALL) return;
        if (!zm_match_take_win(e.args, e.src)) {
            s_pendingWin = e;
            s_pendingWinHave = true;
        }
        return;
    }
    if (s_role == ZM_NET_ZOMBIE && e.kind == ZM_EV_REVIVE) {
        if (e.src != link) return;
        if (e.args[1] <= 0) {
            // Negative health requests a reservation; zero releases one.
            // Only the final positive {target, health} goes to everybody.
            bool granted = zm_revive_host_claim(e.args[0], e.args[1], link);
            if (e.args[1] < 0) {
                NetEvent reply = e;
                reply.src = ZM_NET_DIRECTOR;
                reply.dst = (unsigned char)link;
                reply.args[1] = granted ? (short)-e.args[1] : 0;
                reply.args[2] = 1;              // a grant, not another reviver's commit
                net_queue(link, reply);
            }
            return;
        }
        if (!zm_revive_host_claim(e.args[0], e.args[1], link)) return;
    }
    if (e.kind == ZM_EV_ROSTER && s_role == ZM_NET_ZOMBIE && e.src != link) return;
    if (e.kind == ZM_EV_VOICE && s_role == ZM_NET_ZOMBIE && e.src != link) return;   // speaks only as itself
    bool mine = (e.dst == ZM_NET_ALL) || (e.dst == s_self);
    if (s_role == ZM_NET_ZOMBIE && (e.dst == ZM_NET_ALL || e.dst != s_self)) {
        net_route(e, link);
    }
    if (!mine) return;
    if (e.kind == ZM_EV_ROSTER) {
        // World bookkeeping, not for any one entity: applied now.
        zm_world_apply_remote(e.args, e.src);
        return;
    }
    if (s_inboxCount < (int)(sizeof(s_inbox) / sizeof(s_inbox[0]))) {
        s_inbox[s_inboxCount++].ev = e;
    }
}

static int net_link_of(const PlatNetAddr* from)
{
    for (int i = 0; i < ZM_NET_MAX_PLAYERS; i++) {
        if (s_links[i].used && s_links[i].addr.ip == from->ip && s_links[i].addr.port == from->port) {
            return i;
        }
    }
    return -1;
}

// A dropped packet's reason, to the debug log - at most one line a second.
static void net_log_drop(const PlatNetAddr* from, const char* why, int a, int b)
{
    static unsigned int s_lastMs = 0;
    unsigned int now = plat_time_ms();
    if (s_lastMs != 0 && now - s_lastMs < 1000) return;
    s_lastMs = now;
    dbg_printf("[net] dropped a packet from %u.%u.%u.%u:%u: %s (%d, %d)\n",
               (from->ip >> 24) & 0xFF, (from->ip >> 16) & 0xFF,
               (from->ip >> 8) & 0xFF, from->ip & 0xFF, (unsigned)from->port, why, a, b);
}

static void net_pause_broadcast(void)
{
    NetPauseBody b = {};
    b.serial = ++s_pauseSerial;
    unsigned int now = plat_time_ms();
    for (int i = 1; i < 4; i++) {
        const NetLink& L = s_links[i];
        if (s_states[i].valid && !L.expired) b.present |= (unsigned char)(1 << i);
        if (!L.graceAt || L.expired || now - L.graceAt >= ZM_NET_RECONNECT_MS) continue;
        b.missing |= (unsigned char)(1 << i);
        unsigned int left = ZM_NET_RECONNECT_MS - (now - L.graceAt);
        if (left > b.remainingMs) b.remainingMs = left;
    }
    for (int i = 1; i < 4; i++) if (s_links[i].used)
        net_send_link(i, PKT_PAUSE, 0, &b, sizeof(b));
}

static void net_begin_grace(int link)
{
    NetLink& L = s_links[link];
    if (L.expired || L.graceAt) return;
    L.graceAt = plat_time_ms();
    if (!L.graceAt) L.graceAt = 1;
    L.lost = true;
    if (s_role == ZM_NET_ZOMBIE) net_pause_broadcast();
    else { s_status = ZM_NET_LOST; s_localLostAt = L.graceAt; }
    (void)zm_game_time_ms();
    dbg_printf("[rejoin] link %d lost: holding match for 30 seconds\n", link);
}

static void net_reject(const PlatNetAddr* addr, unsigned int epoch)
{
    NetHeader h = {};
    h.magic = ZM_NET_MAGIC; h.version = ZM_NET_VERSION; h.type = PKT_REJECT; h.epoch = epoch;
    zm_rdv_net_send(addr, &h, sizeof(h));
}

static void net_expire_link(int link)
{
    NetLink& L = s_links[link];
    if (L.expired) return;
    L.expired = L.lost = true; L.recovering = false;
    if (s_role == ZM_NET_ZOMBIE) {
        if (L.checkpointHave) {
            ZmReconnectPlayer p = L.checkpoint;
            zm_pickups_reconnect_reconcile(link, &p);
            zm_box_reconnect_reconcile(link, &p);
            zm_drops_reconnect_reconcile(link, &p);
            zm_spec_reconnect_reconcile(link, &p);
            zm_shotgun_reconcile(link, &p);
            zm_drops_reconnect_abandon(&p);
        }
        s_states[link].valid = false;
        net_reject(&L.addr, L.epoch);
        net_pause_broadcast();
    } else {
        s_status = ZM_NET_EXPIRED; s_localLostAt = s_pauseRemaining = 0;
    }
    dbg_printf("[rejoin] link %d grace expired\n", link);
}

static int net_accept_rejoin(const NetHeader* h, const unsigned char* body, int bytes, const PlatNetAddr* from)
{
    if (bytes != sizeof(NetHelloBody) || h->eventCount != 0 || zombie_mode_match_over()) return -1;
    const NetHelloBody& hello = *(const NetHelloBody*)body;
    unsigned int now = plat_time_ms();
    if (hello.seed != s_seed || (!hello.token[0] && !hello.token[1])) return -1;
    for (int i = 1; i < 4; i++) {
        NetLink& L = s_links[i];
        if (!L.used || L.expired || !L.checkpointHave || hello.token[0] != L.token[0] || hello.token[1] != L.token[1]) continue;
        if (L.graceAt && now - L.graceAt >= ZM_NET_RECONNECT_MS) return -1;
        // A matching credential cannot replace an actively reporting client.
        if (!L.graceAt && now - L.lastHeardMs < 1000u) return -1;
        if (!L.graceAt) net_begin_grace(i);
        L.addr = *from; L.epoch = h->epoch;
        L.pendingCount = 0; L.nextEventId = 1; L.lastReceived = 0;
        L.lastHeardMs = now; L.lost = false; L.recovering = true;
        zm_reconnect_route_events();
        ZmReconnectPlayer player = L.checkpoint;
        zm_pickups_reconnect_reconcile(i, &player);
        zm_box_reconnect_reconcile(i, &player);
        zm_drops_reconnect_reconcile(i, &player);
        zm_spec_reconnect_reconcile(i, &player);
        zm_shotgun_reconcile(i, &player);
        L.recoveryBytes = zm_reconnect_build(L.recovery, sizeof(L.recovery), &player);
        if (!L.recoveryBytes) return -1;
        net_send_lobby(i, PKT_GO);
        net_pause_broadcast();
        dbg_printf("[rejoin] survivor %d reclaimed its seat; sending %d checkpoint bytes\n", i, L.recoveryBytes);
        return i;
    }
    return -1;
}

static void net_dispatch(int link, unsigned char type, int origin, const unsigned char* body, int bodyLen);
static void net_take_fragment(int link, unsigned int epoch, int origin, const unsigned char* body, int bodyLen);

static void net_handle(const unsigned char* buf, int len, const PlatNetAddr* from)
{
    // The rendezvous server's packets, punches still arriving after the link
    // is up, and relayed datagrams (ZombieRendezvous.cpp).
    if (zm_rdv_is_packet(buf, len)) {
        const unsigned char* inner; int innerLen; PlatNetAddr innerFrom;
        if (zm_rdv_take(buf, len, from, &inner, &innerLen, &innerFrom) && !zm_rdv_is_packet(inner, innerLen))
            net_handle(inner, innerLen, &innerFrom);    // a relayed peer's datagram
        return;
    }
    // Older protocols share magic/version/type, but may have shorter headers.
    // Diagnose incompatibility before decoding their version-specific layout.
    if (len >= 6) {
        unsigned int magic; memcpy(&magic, buf, sizeof(magic));
        if (magic == ZM_NET_MAGIC) {
            bool incompatible = buf[4] != ZM_NET_VERSION;
            if (s_role == ZM_NET_SURVIVOR && s_status == ZM_NET_JOINING && net_link_of(from) == 0 &&
                (incompatible || buf[5] == PKT_VERSION_REJECT)) {
                s_status = ZM_NET_FAILED;
                s_failureText = "GAME VERSION MISMATCH";
                s_failureHint = "UPDATE EVERY PLAYER TO THE SAME BUILD";
                return;
            }
            if (s_role == ZM_NET_ZOMBIE && incompatible && buf[5] == PKT_HELLO) {
                NetHeader reply = {};
                reply.magic = ZM_NET_MAGIC; reply.version = ZM_NET_VERSION; reply.type = PKT_VERSION_REJECT;
                zm_rdv_net_send(from, &reply, sizeof(reply));
            }
        }
    }
    if (len < (int)sizeof(NetHeader)) {
        net_log_drop(from, "too short", len, (int)sizeof(NetHeader));
        return;
    }
    const NetHeader* h = (const NetHeader*)buf;
    if (h->magic != ZM_NET_MAGIC || h->version != ZM_NET_VERSION) {
        net_log_drop(from, "magic or version mismatch (theirs, ours)", h->version, ZM_NET_VERSION);
        return;
    }

    int link = net_link_of(from);
    if (s_role == ZM_NET_ZOMBIE && h->type == PKT_HELLO && s_lobbyGo &&
        (link < 0 || h->epoch != s_links[link].epoch)) {
        link = net_accept_rejoin(h, buf + sizeof(NetHeader), len - sizeof(NetHeader), from);
        if (link < 0) { net_reject(from, h->epoch); return; }
    }

    // The host seats a new survivor on its first hello (lobby only).
    // Three survivors at most: against the AI director the host plays seat 0
    // as one of them, so only seats 1-2 take joiners.
    if (link < 0 && s_role == ZM_NET_ZOMBIE && h->type == PKT_HELLO && !s_lobbyGo) {
        for (int i = 1; i < zm_net_seat_limit(); i++) {
            if (s_links[i].used) continue;
            memset(&s_links[i], 0, sizeof(s_links[i]));
            s_links[i].used = true;
            s_links[i].addr = *from;
            s_links[i].nextEventId = 1;
            s_links[i].epoch = h->epoch;
            // The joiner's own match id (its HELLO token) names the seat
            // from now on; a rejoin presents it again.
            const NetHelloBody* hello = (len - (int)sizeof(NetHeader) >= (int)sizeof(NetHelloBody) && h->eventCount == 0)
                ? (const NetHelloBody*)(buf + sizeof(NetHeader)) : NULL;
            if (hello != NULL && (hello->token[0] || hello->token[1])) {
                s_links[i].token[0] = hello->token[0];
                s_links[i].token[1] = hello->token[1];
            } else {
                s_links[i].token[0] = plat_random_u32();
                s_links[i].token[1] = plat_random_u32() | 1;
            }
            s_links[i].lastHeardMs = plat_time_ms();
            s_chars[i] = ZM_SEAT_FREE;
            s_chars[i] = net_grant_char(i, -1);
            link = i;
            dbg_printf("[net] survivor %d joined from %u.%u.%u.%u:%u\n", i,
                       (from->ip >> 24) & 0xFF, (from->ip >> 16) & 0xFF,
                       (from->ip >> 8) & 0xFF, from->ip & 0xFF, (unsigned)from->port);
            break;
        }
    }
    if (link < 0) {                        // a stranger, or the table is full
        // A joiner's hello with every seat taken: tell it the lobby is full.
        if (s_role == ZM_NET_ZOMBIE && h->type == PKT_HELLO && !s_lobbyGo) net_reject(from, h->epoch);
        net_log_drop(from, s_lobbyGo ? "unknown sender after the game was started"
                                     : "unknown sender (not a hello, or every seat taken)",
                     h->type, (int)s_role);
        return;
    }
    NetLink& L = s_links[link];
    if (h->epoch != L.epoch) return;
    if (L.graceAt && plat_time_ms() - L.graceAt >= ZM_NET_RECONNECT_MS) {
        net_expire_link(link);
    }
    if (L.expired) { if (s_role == ZM_NET_ZOMBIE) net_reject(from, h->epoch); return; }
    L.lastHeardMs = plat_time_ms();
    L.lost = false;
    if (!L.recovering) L.graceAt = 0;
    if (s_role == ZM_NET_SURVIVOR) {
        s_localLostAt = 0;
        if (s_status == ZM_NET_LOST) s_status = ZM_NET_CONNECTED;
    }

    // Their ack drops what they have; their new events are taken in order.
    int keep = 0;
    for (int i = 0; i < L.pendingCount; i++) {
        if ((short)(L.pending[i].id - h->ack) > 0) L.pending[keep++] = L.pending[i];
    }
    L.pendingCount = keep;

    int off = sizeof(NetHeader);
    for (int i = 0; i < h->eventCount; i++) {
        if (off + (int)sizeof(NetEvent) > len) return;
        const NetEvent* e = (const NetEvent*)(buf + off);
        off += sizeof(NetEvent);
        if ((short)(e->id - L.lastReceived) == 1) {
            L.lastReceived = e->id;
            net_take_event(*e, link);
        }
    }
    const unsigned char* body = buf + off;
    int bodyLen = len - off;
    // A survivor's packets are about itself; the host's relays say whose.
    int origin = (s_role == ZM_NET_ZOMBIE) ? link : h->origin;

    if (h->type == PKT_FRAG) {
        net_take_fragment(link, h->epoch, origin, body, bodyLen);
        return;
    }
    net_dispatch(link, h->type, origin, body, bodyLen);
}

// A piece of a body too large for one datagram: once every piece of it is in,
// the whole body goes to net_dispatch as its own type.
static void net_take_fragment(int link, unsigned int epoch, int origin, const unsigned char* body, int bodyLen)
{
    if (bodyLen < (int)sizeof(NetFragHeader)) return;
    NetFragHeader f;
    memcpy(&f, body, sizeof(f));
    const unsigned char* data = body + sizeof(NetFragHeader);
    int n = bodyLen - (int)sizeof(NetFragHeader);
    if (f.count == 0 || f.count > 32 || f.index >= f.count || f.bytes > ZM_NET_MESSAGE_MAX ||
        f.count != ((int)f.bytes + ZM_NET_FRAG_DATA - 1) / ZM_NET_FRAG_DATA || f.type == PKT_FRAG) return;
    int at = f.index * ZM_NET_FRAG_DATA;
    int want = (int)f.bytes - at < ZM_NET_FRAG_DATA ? (int)f.bytes - at : ZM_NET_FRAG_DATA;
    if (want <= 0 || n != want) return;
    NetFragIn& in = s_fragIn[link];
    if (in.epoch != epoch || in.serial != f.serial || in.count != f.count ||
        in.type != f.type || in.bytes != (int)f.bytes) {
        // A newer body (or a new connection): the unfinished one is lost.
        in.epoch = epoch;
        in.serial = f.serial;
        in.count = f.count;
        in.type = f.type;
        in.bytes = f.bytes;
        in.have = 0;
    }
    unsigned int bit = 1u << f.index;
    if (in.have & bit) return;
    memcpy(in.data + at, data, n);
    in.have |= bit;
    unsigned int all = f.count == 32 ? 0xFFFFFFFFu : (1u << f.count) - 1;
    if (in.have != all) return;
    in.have = 0;
    in.serial = 0;
    in.count = 0;
    net_dispatch(link, in.type, origin, in.data, in.bytes);
}

// A whole packet body (or a reassembled one) from link `link`.
static void net_dispatch(int link, unsigned char type, int origin, const unsigned char* body, int bodyLen)
{
    NetLink& L = s_links[link];
    switch (type) {
    case PKT_CHECKPOINT:
        net_take_checkpoint(link, body, bodyLen);
        break;
    case PKT_BUMPER:
        if (s_role == ZM_NET_ZOMBIE && bodyLen == 1 && body[0] <= 2) {
            if (s_bumperReady[link] < (int)body[0]) s_bumperReady[link] = (signed char)body[0];
            net_bumper_advance();   // a human director is not at the barrier to run it
            s_bumperSent = 0; // repeat phase promptly, including after a lost reply
            unsigned char reply[5] = { s_bumperPhase, s_chars[0], s_chars[1], s_chars[2], s_chars[3] };
            net_send_link(link, PKT_BUMPER, (unsigned char)s_self, reply, sizeof(reply));
        } else if (s_role == ZM_NET_SURVIVOR && link == 0 && bodyLen == 5 && body[0] <= 3) {
            if (body[0] >= s_bumperPhase) {
                s_bumperPhase = body[0];
                for (int i = 0; i < ZM_NET_MAX_PLAYERS; ++i) s_chars[i] = body[i+1];
            }
        }
        break;
    case PKT_HELLO:
        if (s_role == ZM_NET_ZOMBIE && bodyLen >= (int)sizeof(NetHelloBody)) {
            const NetHelloBody& hello = *(const NetHelloBody*)body;
            // Picks stay open for a survivor still choosing - after GO too,
            // until it reports in (the character select's two minutes).
            if ((!s_lobbyGo || !s_ready[link]) && hello.character < ZM_CHAR_COUNT) {
                s_chars[link] = net_grant_char(link, hello.character);
            }
            // A vote belongs to the full lobby revision, not a seed byte.
            if (!s_lobbyGo && hello.revision == s_lobbyRevision) {
                s_votes[link] = hello.vote & 3;
            }
            net_send_lobby(link, s_lobbyGo ? PKT_GO : PKT_WELCOME);
        }
        break;
    case PKT_WELCOME:
    case PKT_GO:
        if (s_role == ZM_NET_SURVIVOR && bodyLen >= (int)sizeof(NetLobbyBody)) {
            const NetLobbyBody* w = (const NetLobbyBody*)body;
            if (w->index < 1 || w->index >= ZM_NET_MAX_PLAYERS) break;
            if (w->revision < s_lobbyRevision) break; // delayed pre-veto snapshot
            if (w->revision != s_lobbyRevision) {
                s_myVote = ZM_VOTE_NONE;
                s_lastHelloMs = 0;
                dbg_printf("[net] lobby revision %u seed %08X phase %u veto %u\n",
                           w->revision, w->seed, (unsigned)w->phase, (unsigned)w->vetoUsed);
            }
            s_lobbyRevision = w->revision;
            if (s_self < 0) dbg_printf("[net] seated as survivor %d\n", (int)w->index);
            s_self = w->index;
            bool saveCredential = L.token[0] != w->token[0] || L.token[1] != w->token[1] ||
                                  s_seed != w->seed || s_chars[s_self] != w->chars[s_self];
            L.token[0] = w->token[0]; L.token[1] = w->token[1];
            memcpy(s_chars, w->chars, sizeof(s_chars));
            if (w->seed != s_seed) s_myVote = ZM_VOTE_NONE;     // a new map: vote again
            s_seed = w->seed;
            if (saveCredential) net_save_credential();
            if (s_rejoining && w->recoveryBytes && w->recoveryBytes <= ZM_RECONNECT_BLOB_MAX) {
                s_downloadTotal = w->recoveryBytes;
                s_started = true;
            }
            s_phase = w->phase;
            s_vetoUsed = w->vetoUsed;
            s_lobbyOptions = w->options;
            for (int i = 1; i < ZM_NET_MAX_PLAYERS; i++) s_votes[i] = (unsigned char)((w->votes >> ((i - 1) * 2)) & 3);
            if (s_status == ZM_NET_JOINING) s_status = ZM_NET_CONNECTED;
            if (type == PKT_GO) s_lobbyGo = true;
        }
        break;
    case PKT_READY:
        if (s_role == ZM_NET_ZOMBIE) {
            s_ready[link] = true;
            // Already released: its START went missing, send another.
            if (s_started) net_send_link(link, PKT_START, (unsigned char)s_self, NULL, 0);
        }
        break;
    case PKT_START:
        if (s_role == ZM_NET_SURVIVOR) s_started = true;
        break;
    case PKT_STATE:
        net_take_state(origin, body, bodyLen);
        if (s_role == ZM_NET_ZOMBIE) net_send_all(PKT_STATE, (unsigned char)origin, body, bodyLen, link);
        break;
    case PKT_ENEMIES:
        net_take_enemies(origin, body, bodyLen);
        if (s_role == ZM_NET_ZOMBIE) net_send_all(PKT_ENEMIES, (unsigned char)origin, body, bodyLen, link);
        break;
    case PKT_FLAGS:
        if (bodyLen >= 72) zm_world_merge_flags(body);
        break;
    case PKT_BYE:
        if (s_role == ZM_NET_ZOMBIE) {
            dbg_printf("[net] survivor %d left\n", link);
            if (!s_lobbyGo) {
                s_states[link].valid = false;
                L.used = false;                 // the seat opens again
                s_chars[link] = ZM_SEAT_FREE;
            } else {
                net_begin_grace(link);
            }
        } else if (!s_started) {
            s_kickText = "THE HOST LEFT THE GAME";
        } else {
            net_begin_grace(0);
        }
        break;
    case PKT_PAUSE:
        if (s_role == ZM_NET_SURVIVOR && bodyLen == sizeof(NetPauseBody)) {
            const NetPauseBody& p = *(const NetPauseBody*)body;
            if (s_pauseSerial && (int)(p.serial - s_pauseSerial) <= 0) break;
            s_pauseSerial = p.serial; s_pauseMissing = p.missing;
            s_pauseRemaining = p.remainingMs > ZM_NET_RECONNECT_MS ? ZM_NET_RECONNECT_MS : p.remainingMs;
            s_pauseReceived = plat_time_ms();
            for (int i = 1; i < 4; i++) if (!(p.present & (1 << i))) s_states[i].valid = false;
            if (s_rejoinLoadReady && !(p.missing & (1 << s_self))) s_rejoining = false;
            (void)zm_game_time_ms();
        }
        break;
    case PKT_RECOVERY_GET:
        if (s_role == ZM_NET_ZOMBIE && L.recovering && bodyLen == sizeof(unsigned int)) {
            unsigned int offset; memcpy(&offset, body, sizeof(offset));
            if (offset >= (unsigned int)L.recoveryBytes) break;
            NetRecoveryChunk chunk = {};
            chunk.offset = offset; chunk.total = L.recoveryBytes;
            unsigned int left = L.recoveryBytes - offset;
            chunk.bytes = (unsigned short)(left < sizeof(chunk.data) ? left : sizeof(chunk.data));
            memcpy(chunk.data, L.recovery + offset, chunk.bytes);
            net_send_link(link, PKT_RECOVERY_CHUNK, 0, &chunk, sizeof(chunk));
        }
        break;
    case PKT_RECOVERY_CHUNK:
        if (s_role == ZM_NET_SURVIVOR && s_rejoining && !s_downloaded && bodyLen == sizeof(NetRecoveryChunk)) {
            const NetRecoveryChunk& c = *(const NetRecoveryChunk*)body;
            if (c.total != s_downloadTotal || c.total > sizeof(s_download) || c.offset != s_downloadOffset ||
                !c.bytes || c.bytes > sizeof(c.data) || c.bytes > c.total - c.offset) break;
            memcpy(s_download + c.offset, c.data, c.bytes);
            s_downloadOffset += c.bytes; s_lastDownloadMs = 0;
            if (s_downloadOffset == s_downloadTotal) {
                s_downloaded = zm_reconnect_import(s_download, (int)s_downloadTotal);
                if (!s_downloaded) {
                    s_status = ZM_NET_FAILED; s_failureText = "INVALID REJOIN CHECKPOINT";
                    s_failureHint = "";
                }
            }
        }
        break;
    case PKT_RECOVERY_READY:
        if (s_role == ZM_NET_ZOMBIE && L.recovering) {
            L.recovering = false; L.graceAt = 0; L.lost = false;
            net_pause_broadcast();
        }
        break;
    case PKT_REJECT:
        if (s_role == ZM_NET_SURVIVOR && s_status == ZM_NET_JOINING && !s_rejoining) {
            s_failureText = "THE LOBBY IS FULL";
            s_failureHint = "THE GAME ALREADY HAS ITS SURVIVORS";
            s_status = ZM_NET_FAILED;
            L.expired = true;
        } else if (s_role == ZM_NET_SURVIVOR) {
            s_failureText = "REJOIN NOT AVAILABLE";
            s_failureHint = "THE SAVED MATCH OR REJOIN WINDOW ENDED";
            s_status = zombie_mode_armed() ? ZM_NET_EXPIRED : ZM_NET_FAILED;
            L.expired = true;
            s_localLostAt = s_pauseRemaining = 0;
        }
        break;
    }
}

// One of the host's own events (s_loopback): what net_take_event does for a
// remote one, without its link checks - the sender is this copy.
static void net_take_local(const NetEvent& e)
{
    switch (e.kind) {
    case ZM_EV_SHOTGUN: zm_shotgun_take(e.args, e.src); return;
    case ZM_EV_PIANO:   zm_piano_take(e.args, e.src); return;
    case ZM_EV_PICKUP:  zm_pickups_take(e.args, e.src); return;
    case ZM_EV_BOX:     zm_box_take(e.args, e.src); return;
    case ZM_EV_ROSTER:  zm_world_apply_remote(e.args, e.src); return;
    case ZM_EV_WIN:
        if (!zm_match_take_win(e.args, e.src)) { s_pendingWin = e; s_pendingWinHave = true; }
        return;
    case ZM_EV_REVIVE:
        // A reservation or its release; the grant (args[2] 1) is the reply.
        if (e.args[2] != 1 && e.args[1] <= 0) {
            bool granted = zm_revive_host_claim(e.args[0], e.args[1], e.src);
            if (e.args[1] < 0) {
                NetEvent reply = e;
                reply.args[1] = granted ? (short)-e.args[1] : 0;
                reply.args[2] = 1;
                if (s_loopbackCount < ZM_NET_MAX_PENDING) s_loopback[s_loopbackCount++] = reply;
            }
            return;
        }
        break;
    }
    if (s_inboxCount < (int)(sizeof(s_inbox) / sizeof(s_inbox[0]))) s_inbox[s_inboxCount++].ev = e;
}

static void net_drain_loopback(void)
{
    // Handlers may queue replies to themselves: take a batch at a time.
    for (int pass = 0; pass < 4 && s_loopbackCount > 0; pass++) {
        NetEvent batch[ZM_NET_MAX_PENDING];
        int n = s_loopbackCount;
        memcpy(batch, s_loopback, sizeof(NetEvent) * n);
        s_loopbackCount = 0;
        for (int i = 0; i < n; i++) net_take_local(batch[i]);
    }
}

static void net_reset(void);

// The session closes and the lobby's NEW GAME menu shows why (FAILED).
static void net_kick(void)
{
    const char* why = s_kickText;
    dbg_printf("[net] %s: back to the menu\n", why);
    zm_rdv_host_end();
    plat_net_close();
    g_bRunInBackground = g_bRunInBackgroundConfig;
    net_reset();
    s_role = ZM_NET_OFF;
    s_status = ZM_NET_FAILED;
    s_failureText = why;
    s_failureHint = "";
}

void zm_net_poll(void)
{
    if (s_status == ZM_NET_IDLE || s_status == ZM_NET_FAILED || s_status == ZM_NET_EXPIRED) return;
    if (s_pendingWinHave && zm_match_take_win(s_pendingWin.args, s_pendingWin.src)) s_pendingWinHave = false;
    static unsigned char buf[16384];
    PlatNetAddr from;
    for (int i = 0; i < 128; i++) {
        int n = plat_net_recv(buf, sizeof(buf), &from);
        if (n <= 0) break;
        net_handle(buf, n, &from);
        if (s_kickText != NULL) break;
    }
    if (s_kickText != NULL) { net_kick(); return; }
    net_drain_loopback();
    if (s_role == ZM_NET_ZOMBIE) zm_rdv_frame();     // the game code's heartbeat, punches
    unsigned int now = plat_time_ms();

    // Time out silent links.
    for (int i = 0; i < ZM_NET_MAX_PLAYERS; i++) {
        NetLink& L = s_links[i];
        if (!L.used || L.expired) continue;
        if (L.graceAt && now - L.graceAt >= ZM_NET_RECONNECT_MS) {
            net_expire_link(i);
            continue;
        }
        if (s_status == ZM_NET_JOINING && now - L.lastHeardMs > 10000u) {
            s_status = ZM_NET_FAILED; s_failureText = "HOST DID NOT ANSWER";
            s_failureHint = "CHECK ADDRESS, PORT AND GAME VERSION";
            continue;
        }
        unsigned int timeout = s_started && (s_role == ZM_NET_SURVIVOR || s_ready[i]) ?
            ZM_NET_GAME_TIMEOUT_MS : ZM_NET_TIMEOUT_MS;
        if (!s_lobbyGo) timeout = ZM_NET_LOBBY_TIMEOUT_MS;
        if (L.lost || now - L.lastHeardMs <= timeout) continue;
        if (s_status == ZM_NET_JOINING) continue;
        if (s_started && (s_role == ZM_NET_SURVIVOR || s_ready[i])) { net_begin_grace(i); continue; }
        L.lost = true;
        if (s_role == ZM_NET_ZOMBIE) {
            s_states[i].valid = false;
            if (!s_lobbyGo) { L.used = false; s_chars[i] = ZM_SEAT_FREE; }
            dbg_printf("[net] survivor %d timed out\n", i);
        } else if (!s_started) {
            s_kickText = "LOST CONTACT WITH THE HOST";
        } else {
            s_status = ZM_NET_LOST;
            for (int k = 0; k < ZM_NET_MAX_PLAYERS; k++) s_states[k].valid = false;
            dbg_printf("[net] connection lost\n");
        }
    }

    if (s_kickText != NULL) { net_kick(); return; }

    if (now - s_lastHelloMs > ZM_NET_HELLO_MS) {
        s_lastHelloMs = now;
        if (s_role == ZM_NET_SURVIVOR && !s_started) {
            // Joining, in the lobby, or choosing a character after GO: hello
            // carries the character pick and the map vote.
            NetHelloBody c = {};
            c.character = (unsigned char)s_myChar; c.vote = s_myVote; c.revision = s_lobbyRevision;
            c.token[0] = s_links[0].token[0]; c.token[1] = s_links[0].token[1];
            if (s_rejoining) c.seed = s_seed;
            net_send_link(0, PKT_HELLO, (unsigned char)(s_self < 0 ? 0 : s_self), &c, sizeof(c));
        } else if (s_role == ZM_NET_ZOMBIE && !s_lobbyGo) {
            for (int i = 1; i < ZM_NET_MAX_PLAYERS; i++) {
                if (s_links[i].used) net_send_lobby(i, PKT_WELCOME);
            }
        } else {
            // In the game: a survivor still choosing its character hears the
            // picks (GO again) as well.
            if (s_role == ZM_NET_ZOMBIE) {
                for (int i = 1; i < ZM_NET_MAX_PLAYERS; i++) {
                    if (s_links[i].used && !s_ready[i]) net_send_lobby(i, PKT_GO);
                }
            }
            // Keep the other ends' timers fed (and resend pending events) even
            // where no state is being sent; also when a link went quiet - a
            // long FMV on one side can outlast the timeout.
            for (int i = 0; i < ZM_NET_MAX_PLAYERS; i++) {
                if (s_links[i].used) net_send_link(i, PKT_PING, (unsigned char)s_self, NULL, 0);
            }
            if (s_role == ZM_NET_ZOMBIE && s_started) net_pause_broadcast();
        }
    }

    if (s_role == ZM_NET_SURVIVOR && s_rejoining && s_downloadTotal &&
        (!s_lastDownloadMs || now - s_lastDownloadMs >= 100u)) {
        s_lastDownloadMs = now ? now : 1u;
        if (!s_downloaded) net_send_link(0, PKT_RECOVERY_GET, (unsigned char)s_self, &s_downloadOffset, sizeof(s_downloadOffset));
        else if (s_rejoinLoadReady) net_send_link(0, PKT_RECOVERY_READY, (unsigned char)s_self, NULL, 0);
    }
    (void)zm_game_time_ms();

    // The shared world flags, once a second during a game: survivors to the
    // host, the host's merged banks back to everyone.
    static unsigned int s_lastFlagsMs = 0;
    if (s_lobbyGo && s_started && !zm_net_pause_active() && now - s_lastFlagsMs > ZM_NET_FLAGS_MS) {
        s_lastFlagsMs = now;
        unsigned char flags[72];
        int n = zm_world_pack_flags(flags);
        net_send_all(PKT_FLAGS, (unsigned char)s_self, flags, n, -1);
    }
}

// ---------------------------------------------------------------------------
// Session control
// ---------------------------------------------------------------------------
static void net_reset(void)
{
    s_rdvCode = 0;
    s_loopbackCount = 0;
    memset(s_fragIn, 0, sizeof(s_fragIn));
    s_checkpointSentHave = false;
    s_checkpointSentMs = 0;
    s_kickText = NULL;
    s_rejoining = s_downloaded = s_rejoinLoadReady = false;
    s_downloadOffset = s_downloadTotal = s_lastDownloadMs = 0;
    s_localLostAt = s_pauseRemaining = s_pauseReceived = s_pauseSerial = s_pauseMissing = 0;
    s_pauseTotal = s_pauseBegan = 0; s_clockPaused = false;
    s_failureText = "COULD NOT OPEN THE CONNECTION";
    s_failureHint = "CHECK YOUR NETWORK AND PORT SETTINGS";
    zm_reconnect_forget();
    s_pendingWinHave = false;
    memset(s_links, 0, sizeof(s_links));
    memset(s_states, 0, sizeof(s_states));
    memset(s_ready, 0, sizeof(s_ready));
    memset(s_bumperReady, -1, sizeof(s_bumperReady));
    s_bumperPhase = 0; s_bumperSent = 0;
    for (int i = 0; i < ZM_NET_MAX_PLAYERS; i++) s_chars[i] = ZM_SEAT_FREE;
    s_inboxCount = 0;
    s_self = -1;
    s_lobbyGo = false;
    s_started = false;
    s_lastHelloMs = 0;
    s_lobbyRevision = 0;
    s_phase = ZM_LOBBY_JOIN;
    s_vetoUsed = 0;
    s_lobbyOptions = 0;
    memset(s_votes, 0, sizeof(s_votes));
    s_myVote = ZM_VOTE_NONE;
}

void zm_net_host(void)
{
    zm_net_stop();
    net_reset();
    g_bRunInBackground = true;      // the other copies may be in windows beside this one
    s_role = ZM_NET_ZOMBIE;
    s_self = ZM_NET_DIRECTOR;
    s_chars[0] = ZM_SEAT_DIRECTOR;
    if (!plat_net_open(g_zmNetPort)) {
        s_status = ZM_NET_FAILED;
        dbg_printf("[net] could not bind port %u\n", (unsigned)g_zmNetPort);
        return;
    }
    s_status = ZM_NET_HOSTING;
    zm_rdv_host_begin();
    s_seed = plat_time_ms() * 2654435761u ^ (unsigned int)rand();
    dbg_printf("[net] hosting on port %u as the director (seed %08X)\n", (unsigned)g_zmNetPort, s_seed);
}

// A survivor's session up to the host's address: the role, the pick, the
// roster (the director places monsters while this survivor is still choosing
// its character, ZombieWorld.cpp).
static void net_join_setup(int character)
{
    net_reset();
    g_bRunInBackground = true;
    s_role = ZM_NET_SURVIVOR;
    // No wish (-1) until the survivor picks: the host's free character stands.
    s_myChar = (character >= 0 && character < ZM_CHAR_COUNT) ? character : ZM_SEAT_FREE;
    zm_world_reset(true);
}

static void net_join_link(const PlatNetAddr* host)
{
    NetLink& L = s_links[0];
    L.addr = *host;
    L.used = true;
    L.nextEventId = 1;
    L.epoch = plat_random_u32();
    if (!L.epoch) L.epoch = 1;
    L.lastHeardMs = plat_time_ms();
    // This survivor's match id: drawn here, carried by every HELLO. It alone
    // reclaims the seat on a rejoin, so it comes from the secure generator.
    L.token[0] = plat_random_u32();
    L.token[1] = plat_random_u32();
    if (!L.token[0] && !L.token[1]) L.token[1] = 1;
    s_status = ZM_NET_JOINING;
}

void zm_net_join(const char* address, unsigned short port, int character)
{
    zm_net_stop();
    net_join_setup(character);
    PlatNetAddr host;
    if (!plat_net_resolve(address, port, &host) || !plat_net_open(0)) {
        s_status = ZM_NET_FAILED;
        s_role = ZM_NET_OFF;
        dbg_printf("[net] could not reach %s:%u\n", address, (unsigned)port);
        return;
    }
    net_join_link(&host);
    dbg_printf("[net] joining %s:%u as %s\n", address, (unsigned)port, zm_char_name(s_myChar));
}

void zm_net_join_peer(const PlatNetAddr* host, int character, unsigned int code)
{
    // Not zm_net_stop: the punched socket must stay open.
    net_join_setup(character);
    s_rdvCode = code;
    net_join_link(host);
    dbg_printf("[net] joining game %06u at %u.%u.%u.%u:%u\n", code, host->ip >> 24, (host->ip >> 16) & 255,
               (host->ip >> 8) & 255, host->ip & 255, (unsigned)host->port);
}

void zm_net_fail(const char* text, const char* hint)
{
    g_bRunInBackground = g_bRunInBackgroundConfig;
    net_reset();
    s_role = ZM_NET_OFF;
    s_status = ZM_NET_FAILED;
    s_failureText = text;
    s_failureHint = hint;
}

int zm_net_protocol_version(void) { return ZM_NET_VERSION; }

static void net_broadcast_lobby(void);

void zm_net_set_char(int character)
{
    if (character < 0 || character >= ZM_CHAR_COUNT) return;
    s_myChar = character;
    s_lastHelloMs = 0;              // tell the host now
    // The host of an AI director's game plays seat 0 itself.
    if (s_role == ZM_NET_ZOMBIE && zm_net_ai_level() > 0 && !s_lobbyGo) {
        s_chars[0] = net_grant_char(0, character);
        net_broadcast_lobby();
    }
}

static bool net_read_credential(NetRejoinCredential* c)
{
    char path[512]; net_credential_path(path, sizeof(path));
    size_t size = 0;
    void* file = plat_file_read_all(path, &size);
    memset(c, 0, sizeof(*c));
    if (file && size == sizeof(*c)) memcpy(c, file, sizeof(*c));
    free(file);
    return size == sizeof(*c) && c->magic == 0x52454A43 && c->seat >= 1 && c->seat <= 3 &&
           c->character < ZM_CHAR_COUNT;
}

static void net_rejoin_apply(const NetRejoinCredential& c)
{
    s_rejoining = true; s_seed = c.seed;
    s_links[0].token[0] = c.token[0]; s_links[0].token[1] = c.token[1];
}

unsigned int zm_net_rejoin_code(void)
{
    NetRejoinCredential c;
    return net_read_credential(&c) ? c.code : 0;
}

bool zm_net_rejoin_saved(void)
{
    NetRejoinCredential c;
    if (!net_read_credential(&c)) {
        s_status = ZM_NET_FAILED; s_failureText = "NO SAVED MATCH TO REJOIN";
        s_failureHint = ""; return false;
    }
    unsigned int a = (c.ip >> 24) & 255, b = (c.ip >> 16) & 255, d = c.ip & 255;
    char address[32]; snprintf(address, sizeof(address), "%u.%u.%u.%u", a, b, (c.ip >> 8) & 255, d);
    zm_net_join(address, c.port, c.character);
    if (s_status != ZM_NET_JOINING) return false;
    net_rejoin_apply(c);
    return true;
}

bool zm_net_rejoin_peer(const PlatNetAddr* host, unsigned int code)
{
    NetRejoinCredential c;
    if (!net_read_credential(&c) || c.code != code) {
        zm_net_fail("NO SAVED MATCH TO REJOIN", "");
        return false;
    }
    zm_net_join_peer(host, c.character, code);
    net_rejoin_apply(c);
    return true;
}

void zm_net_rejoin_ready(void)
{
    if (!s_rejoining || !s_downloaded) return;
    s_rejoinLoadReady = true; s_lastDownloadMs = 0;
    net_send_link(0, PKT_RECOVERY_READY, (unsigned char)s_self, NULL, 0);
}

void zm_net_lobby_go(void)
{
    if (s_role != ZM_NET_ZOMBIE) return;
    ++s_lobbyRevision;
    s_lobbyGo = true;
    for (int r = 0; r < 4; r++) {
        for (int i = 1; i < ZM_NET_MAX_PLAYERS; i++) {
            if (s_links[i].used) net_send_lobby(i, PKT_GO);
        }
    }
    dbg_printf("[net] the director starts the game with %d survivors\n", zm_net_survivor_count());
}

bool zm_net_lobby_started(void)
{
    return s_lobbyGo;
}

unsigned int zm_net_seed(void)
{
    return s_seed;
}

int zm_net_lobby_phase(void) { return s_phase; }
int zm_net_veto_used(void) { return s_vetoUsed; }
int zm_net_vote(int player)
{
    if (player < 0 || player >= ZM_NET_MAX_PLAYERS) return ZM_VOTE_NONE;
    return (player == s_self && s_role == ZM_NET_SURVIVOR) ? s_myVote : s_votes[player];
}

static void net_broadcast_lobby(void)
{
    for (int r = 0; r < 2; r++) {
        for (int i = 1; i < ZM_NET_MAX_PLAYERS; i++) {
            if (s_links[i].used) net_send_lobby(i, PKT_WELCOME);
        }
    }
}

void zm_net_set_phase(int phase)
{
    if (s_role != ZM_NET_ZOMBIE) return;
    ++s_lobbyRevision;
    s_phase = (unsigned char)phase;
    memset(s_votes, 0, sizeof(s_votes));
    net_broadcast_lobby();
}

void zm_net_reroll(int vetoBit)
{
    if (s_role != ZM_NET_ZOMBIE) return;
    ++s_lobbyRevision;
    s_vetoUsed |= (unsigned char)vetoBit;
    s_seed = (s_seed * 1664525u + 1013904223u) ^ plat_time_ms();
    if (s_seed == 0) s_seed = 1;
    memset(s_votes, 0, sizeof(s_votes));
    dbg_printf("[net] map vetoed (%s); new seed %08X\n", vetoBit == 1 ? "director" : "survivors", s_seed);
    net_broadcast_lobby();
}


int zm_net_seat_limit(void) { return zm_net_ai_level() > 0 ? 3 : ZM_NET_MAX_PLAYERS; }
int zm_net_ai_level(void) { return (s_lobbyOptions & ZM_LOBBY_OPT_AI_MASK) >> ZM_LOBBY_OPT_AI_SHIFT; }
void zm_net_set_ai_level(int level)
{
    if (s_role != ZM_NET_ZOMBIE || s_lobbyGo) return;
    if (level < 0) level = 0;
    if (level > 5) level = 5;
    s_lobbyOptions = (unsigned char)((s_lobbyOptions & ~ZM_LOBBY_OPT_AI_MASK) | (level << ZM_LOBBY_OPT_AI_SHIFT));
    // Seat 0: the host's survivor against the AI, else the human director.
    if (level == 0) s_chars[0] = ZM_SEAT_DIRECTOR;
    else if (s_chars[0] >= ZM_CHAR_COUNT) s_chars[0] = net_grant_char(0, s_myChar);
    dbg_printf("[net] AI director level %d\n", level);
    net_broadcast_lobby();
}

void zm_net_set_vote(int vote)
{
    if (s_role != ZM_NET_SURVIVOR) return;
    s_myVote = (unsigned char)(vote & 3);
    s_lastHelloMs = 0;                  // tell the host now
}

bool zm_net_survivor_ready(int player)
{
    return player > 0 && player < ZM_NET_MAX_PLAYERS && s_ready[player];
}

void zm_net_stop(void)
{
    if (s_status == ZM_NET_CONNECTED || s_status == ZM_NET_HOSTING) {
        net_send_all(PKT_BYE, (unsigned char)(s_self < 0 ? 0 : s_self), NULL, 0, -1);
    }
    zm_rdv_host_end();
    plat_net_close();
    g_bRunInBackground = g_bRunInBackgroundConfig;
    s_status = ZM_NET_IDLE;
    s_role = ZM_NET_OFF;
    net_reset();
}

bool zm_net_start_game(unsigned int timeoutMs)
{
    if (!zm_net_active()) return false;
    unsigned int begin = plat_time_ms();
    unsigned int lastSend = 0;
    if (s_role == ZM_NET_SURVIVOR) {
        // Report in until the host lets everyone go.
        while (!s_started && plat_time_ms() - begin < timeoutMs) {
            unsigned int now = plat_time_ms();
            if (now - lastSend > 250) {
                lastSend = now;
                net_send_link(0, PKT_READY, (unsigned char)s_self, NULL, 0);
            }
            zm_net_poll();
            Task_sleep(1);
        }
        if (!s_started) {
            dbg_printf("[net] the director never started; playing on alone\n");
            return false;
        }
        dbg_printf("[net] game started as survivor %d (%s)\n", s_self, zm_char_name(zm_net_char(s_self)));
        return true;
    }
    // The host starts at once: the survivors are still choosing their
    // characters (up to two minutes) and come in as they report ready - each
    // READY is answered with START (net_receive). The game is multiplayer as
    // long as anyone is seated.
    (void)begin;
    (void)timeoutMs;
    s_started = true;
    int n = 0;
    for (int i = 1; i < ZM_NET_MAX_PLAYERS; i++) {
        if (s_links[i].used && !s_links[i].lost) {
            if (s_ready[i]) net_send_link(i, PKT_START, (unsigned char)s_self, NULL, 0);
            n++;
        }
    }
    dbg_printf("[net] game started; %d survivors seated\n", n);
    // Against the AI director the host is a survivor too: it may play alone.
    return n > 0 || zm_net_ai_level() > 0;
}

// The host's barrier phase from the seats' reports. Against the AI the host
// is a survivor at the barrier; a human director plays at once
// (ZombieBumpers.cpp) and only the survivors hold it.
static void net_bumper_advance(void)
{
    signed char ready[ZM_NET_MAX_PLAYERS];
    memcpy(ready, s_bumperReady, sizeof(ready));
    unsigned int seats = zm_net_ai_level() > 0 ? 1u : 0u;
    for (int i = 1; i < ZM_NET_MAX_PLAYERS; ++i) {
        if (!s_links[i].used || s_links[i].lost) continue;
        seats |= 1u << i;
        if (s_chars[i] >= ZM_CHAR_COUNT) ready[i] = -1;
    }
    unsigned char phase = zm_bumper_barrier_phase(s_bumperPhase, ready, seats);
    if (phase != s_bumperPhase) { s_bumperPhase = phase; s_bumperSent = 0; }
}

bool zm_net_bumper_released(void) { return !zm_net_active() || s_bumperPhase >= 3; }

// Repeated UDP readiness/phase exchange: no gameplay begins until all copies
// have finished their intro. A reconnect does not replay the original barrier.
int zm_net_bumper_phase(int progress)
{
    if (!zm_net_active()) return 3;
    unsigned int now = plat_time_ms();
    if (s_role == ZM_NET_ZOMBIE) {
        s_bumperReady[0] = (signed char)progress;
        net_bumper_advance();
        if (!s_bumperSent || now - s_bumperSent >= 100) {
            unsigned char body[5] = { s_bumperPhase, s_chars[0], s_chars[1], s_chars[2], s_chars[3] };
            net_send_all(PKT_BUMPER, (unsigned char)s_self, body, sizeof(body), -1);
            s_bumperSent = now ? now : 1;
        }
    } else if (!s_bumperSent || now - s_bumperSent >= 100) {
        unsigned char body = (unsigned char)progress;
        net_send_link(0, PKT_BUMPER, (unsigned char)s_self, &body, 1);
        s_bumperSent = now ? now : 1;
    }
    return s_bumperPhase;
}
