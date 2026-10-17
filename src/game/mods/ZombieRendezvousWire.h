#pragma once

/* ===========================================================================
 * ZombieRendezvousWire.h - the match server's wire format (port-added).
 *
 * Plain C, no includes: the game (ZombieRendezvous.h) and the server
 * (server/rendezvous/rendezvous.c) both build from this one copy.
 *
 * Every datagram is an RdvHeader and then the body for its type, packed and
 * little-endian. Never over 1200 bytes (ZombieNet's ZM_NET_DATAGRAM_MAX),
 * except RELAY_DATA: a game datagram of up to 1200 bytes plus 12.
 * Addresses are IPv4 values in host order (192.168.1.10 = 0xC0A8010A).
 * =========================================================================== */

#define ZM_RDV_MAGIC         0x52314552u    /* "RE1R" (ZombieNet's is "RE1Z") */
#define ZM_RDV_PROTOCOL      1
#define ZM_RDV_DEFAULT_PORT  27961
#define ZM_RDV_LIST_MAX      64             /* games in one LIST reply */

enum {
    ZM_RDV_REGISTER = 1,     /* host -> server: RdvRegister (every 5 s; the heartbeat) */
    ZM_RDV_REGISTERED = 2,   /* server -> host: RdvRegistered */
    ZM_RDV_LIST_REQ = 3,     /* survivor -> server: RdvListReq */
    ZM_RDV_LIST = 4,         /* server -> survivor: RdvListHead + RdvListEntry[count] */
    ZM_RDV_JOIN = 5,         /* survivor -> server: RdvJoin */
    ZM_RDV_PEER = 6,         /* server -> survivor: RdvIntro (the host's addresses) */
    ZM_RDV_INTRO = 7,        /* server -> host: RdvIntro (the survivor's addresses) */
    ZM_RDV_JOIN_FAIL = 8,    /* server -> survivor: RdvJoinFail */
    ZM_RDV_PUNCH = 9,        /* peer <-> peer: RdvPunch */
    ZM_RDV_PUNCH_ACK = 10,   /* peer <-> peer: RdvPunch, answering one */
    ZM_RDV_PROBE = 11,       /* tools/rendezvous_probe.py: RdvProbe */
    ZM_RDV_PROBE_REPLY = 12, /* RdvProbeReply */
    ZM_RDV_UNREGISTER = 13,  /* host -> server: RdvUnregister - the game closed */
    /* The relay: a survivor whose punch failed asks the server to carry its
     * link to the host. Each side then wraps every game datagram in
     * RELAY_DATA (RdvRelayData + the datagram) to the server, which passes
     * it to the other side the same way. */
    ZM_RDV_RELAY_REQ = 14,   /* survivor -> server: RdvRelayReq */
    ZM_RDV_RELAY_OK = 15,    /* server -> survivor: RdvRelayOk */
    ZM_RDV_RELAY_OPEN = 16,  /* server -> host: RdvRelayOpen (a survivor is relayed to it) */
    ZM_RDV_RELAY_FAIL = 17,  /* server -> survivor: RdvRelayFail */
    ZM_RDV_RELAY_DATA = 18,  /* either way: RdvRelayData + one game datagram */
    ZM_RDV_RELAY_CLOSE = 19  /* client -> server: RdvRelayClose */
};

enum { ZM_RDV_PUBLIC = 1, ZM_RDV_STARTED = 2, ZM_RDV_AI = 4 };
enum { ZM_RDV_FAIL_NOT_FOUND = 1, ZM_RDV_FAIL_VERSION = 2, ZM_RDV_FAIL_FULL = 3, ZM_RDV_FAIL_BUSY = 4,
       ZM_RDV_FAIL_NO_RELAY = 5 /* relay off, all sessions taken or the month's data used up */ };

/* A relayed datagram is the game's (at most 1200 bytes) plus the relay's
 * wrapping; it crosses the internet, not a virtual LAN, so it may be bigger. */
#define ZM_RDV_GAME_DATAGRAM_MAX  1200
#define ZM_RDV_RELAY_OVERHEAD     12      /* RdvHeader + RdvRelayData */

#pragma pack(push, 1)
typedef struct RdvHeader     { unsigned int magic; unsigned char protocol, type; unsigned short reserved; } RdvHeader;
typedef struct RdvAddr       { unsigned int ip; unsigned short port, reserved; } RdvAddr;
typedef struct RdvRegister   { unsigned int token; unsigned char version, flags, survivors, capacity; RdvAddr local; } RdvRegister;
typedef struct RdvRegistered { unsigned int code; RdvAddr seen; } RdvRegistered;
typedef struct RdvUnregister { unsigned int token; } RdvUnregister;
typedef struct RdvListReq    { unsigned char version, pad[3]; } RdvListReq;
typedef struct RdvListHead   { unsigned char count, pad[3]; } RdvListHead;
typedef struct RdvListEntry  { unsigned int code; unsigned char survivors, capacity, flags, version; } RdvListEntry;
typedef struct RdvJoin       { unsigned int code; unsigned char version, pad[3]; RdvAddr local; } RdvJoin;
typedef struct RdvIntro      { unsigned int code; RdvAddr seen, local; } RdvIntro;
typedef struct RdvJoinFail   { unsigned int code; unsigned char reason, pad[3]; } RdvJoinFail;
typedef struct RdvPunch      { unsigned int code, nonce; } RdvPunch;
typedef struct RdvProbe      { unsigned int nonce; } RdvProbe;
typedef struct RdvProbeReply { unsigned int nonce; RdvAddr seen; } RdvProbeReply;
typedef struct RdvRelayReq   { unsigned int code; unsigned char version, pad[3]; } RdvRelayReq;
typedef struct RdvRelayOk    { unsigned int code, session; } RdvRelayOk;
typedef struct RdvRelayOpen  { unsigned int code, session; RdvAddr seen; } RdvRelayOpen;
typedef struct RdvRelayFail  { unsigned int code; unsigned char reason, pad[3]; } RdvRelayFail;
typedef struct RdvRelayData  { unsigned int session; } RdvRelayData;
typedef struct RdvRelayClose { unsigned int session; } RdvRelayClose;
#pragma pack(pop)

#ifdef __cplusplus
#define ZM_RDV_STATIC_ASSERT(c, m) static_assert(c, m)
#else
#define ZM_RDV_STATIC_ASSERT(c, m) _Static_assert(c, m)
#endif
ZM_RDV_STATIC_ASSERT(sizeof(RdvHeader) == 8, "RdvHeader size");
ZM_RDV_STATIC_ASSERT(sizeof(RdvAddr) == 8, "RdvAddr size");
ZM_RDV_STATIC_ASSERT(sizeof(RdvRegister) == 16, "RdvRegister size");
ZM_RDV_STATIC_ASSERT(sizeof(RdvJoin) == 16, "RdvJoin size");
ZM_RDV_STATIC_ASSERT(sizeof(RdvIntro) == 20, "RdvIntro size");
ZM_RDV_STATIC_ASSERT(sizeof(RdvListEntry) == 8, "RdvListEntry size");
ZM_RDV_STATIC_ASSERT(sizeof(RdvHeader) + sizeof(RdvListHead) + ZM_RDV_LIST_MAX * sizeof(RdvListEntry) <= 1200,
                     "a full LIST over a datagram");
ZM_RDV_STATIC_ASSERT(sizeof(RdvHeader) + sizeof(RdvRelayData) == ZM_RDV_RELAY_OVERHEAD, "relay overhead");
