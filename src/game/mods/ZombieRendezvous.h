#pragma once
#include "../../platform/platform.h"

// ============================================================================
// ZombieRendezvous - finding a match without port forwarding (port-added).
//
// A small UDP server (server/rendezvous/rendezvous.py) introduces players and
// carries nothing else. The host registers from its game socket and gets a
// six-digit game code; a survivor lists the public games or types a code.
// The server hands each side the other's public address (as it saw it) and
// LAN address, both send PUNCH packets at the other at once, and each NAT
// lets the other's packets in. The first PUNCH or PUNCH_ACK to arrive names
// the address the survivor's normal HELLO then goes to - the match itself is
// the plain ZombieNet link, peer to peer.
//
// Everything here runs on ZombieNet's one socket: the port the server sees
// must be the port the game plays on.
//
// Wire format, little-endian, mirrored in rendezvous.py:
//   RdvHeader, then the body for its type. Never over 1200 bytes.
// ============================================================================

#define ZM_RDV_MAGIC         0x52314552u    // "RE1R" (ZombieNet's is "RE1Z")
#define ZM_RDV_PROTOCOL      1
#define ZM_RDV_DEFAULT_PORT  27961
// The server players use when config.ini sets none ([Net] Rendezvous=host:port;
// Rendezvous=off turns it off). Empty: no default server yet.
#define ZM_RDV_DEFAULT_SERVER ""
#define ZM_RDV_LIST_MAX      64

enum {
    ZM_RDV_REGISTER = 1,     // host -> server: RdvRegister (every 5 s; the heartbeat)
    ZM_RDV_REGISTERED = 2,   // server -> host: RdvRegistered
    ZM_RDV_LIST_REQ = 3,     // survivor -> server: RdvListReq
    ZM_RDV_LIST = 4,         // server -> survivor: RdvListHead + RdvListEntry[count]
    ZM_RDV_JOIN = 5,         // survivor -> server: RdvJoin
    ZM_RDV_PEER = 6,         // server -> survivor: RdvIntro (the host's addresses)
    ZM_RDV_INTRO = 7,        // server -> host: RdvIntro (the survivor's addresses)
    ZM_RDV_JOIN_FAIL = 8,    // server -> survivor: RdvJoinFail
    ZM_RDV_PUNCH = 9,        // peer <-> peer: RdvPunch
    ZM_RDV_PUNCH_ACK = 10,   // peer <-> peer: RdvPunch, answering one
    ZM_RDV_PROBE = 11,       // tools/rendezvous_probe.py
    ZM_RDV_PROBE_REPLY = 12,
    ZM_RDV_UNREGISTER = 13,  // host -> server: { token } - the game closed
};

enum { ZM_RDV_PUBLIC = 1, ZM_RDV_STARTED = 2, ZM_RDV_AI = 4 };
enum { ZM_RDV_FAIL_NOT_FOUND = 1, ZM_RDV_FAIL_VERSION = 2, ZM_RDV_FAIL_FULL = 3, ZM_RDV_FAIL_BUSY = 4 };

#pragma pack(push, 1)
struct RdvHeader   { unsigned int magic; unsigned char protocol, type; unsigned short reserved; };
struct RdvAddr     { unsigned int ip; unsigned short port, reserved; };
struct RdvRegister { unsigned int token; unsigned char version, flags, survivors, capacity; RdvAddr local; };
struct RdvRegistered { unsigned int code; RdvAddr seen; };
struct RdvListReq  { unsigned char version, pad[3]; };
struct RdvListHead { unsigned char count, pad[3]; };
struct RdvListEntry { unsigned int code; unsigned char survivors, capacity, flags, version; };
struct RdvJoin     { unsigned int code; unsigned char version, pad[3]; RdvAddr local; };
struct RdvIntro    { unsigned int code; RdvAddr seen, local; };
struct RdvJoinFail { unsigned int code; unsigned char reason, pad[3]; };
struct RdvPunch    { unsigned int code, nonce; };
#pragma pack(pop)

// ---- configuration (config.ini [Net] Rendezvous / Public) ----
extern char g_zmRdvServer[96];       // "host" or "host:port"; "" or "off" = none
extern bool g_zmRdvPublic;           // the host lists its game
bool zm_rdv_available(void);

// ---- ZombieNet's side ----
bool zm_rdv_is_packet(const unsigned char* buf, int len);
void zm_rdv_take(const unsigned char* buf, int len, const PlatNetAddr* from);
void zm_rdv_host_begin(void);        // the host's socket is open
void zm_rdv_host_end(void);          // ... and about to close
void zm_rdv_frame(void);             // from zm_net_poll

// ---- the host's lobby ----
enum { ZM_RDV_HOST_OFF = 0, ZM_RDV_HOST_CONTACTING, ZM_RDV_HOST_REGISTERED, ZM_RDV_HOST_NO_ANSWER };
int  zm_rdv_host_state(void);
unsigned int zm_rdv_code(void);      // 0 until the server gave one

// ---- a survivor finding a game (before ZombieNet has a session) ----
struct ZmRdvGame { unsigned int code; unsigned char survivors, capacity, flags; };
bool zm_rdv_browse_begin(void);      // opens the socket; FALSE: no server / no socket
void zm_rdv_browse_frame(void);      // reads the socket, asks for the list now and then
bool zm_rdv_list_heard(void);        // the server has answered at least once
int  zm_rdv_list_count(void);
const ZmRdvGame* zm_rdv_list(int i);
void zm_rdv_close(void);             // closes the socket unless ZombieNet now has it

enum { ZM_RDV_CONNECT_WORKING = 0, ZM_RDV_CONNECT_DONE, ZM_RDV_CONNECT_FAILED };
// Joins game `code` (opens the socket if browsing has not). DONE: ZombieNet
// is JOINING on the punched address. FAILED: ZombieNet shows why (FAILED).
void zm_rdv_connect_begin(unsigned int code, bool rejoin);
int  zm_rdv_connect_frame(void);
const char* zm_rdv_connect_text(void);
void zm_rdv_connect_cancel(void);
