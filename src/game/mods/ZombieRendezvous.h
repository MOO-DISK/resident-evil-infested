#pragma once
#include "../../platform/platform.h"
#include "ZombieRendezvousWire.h"

// ============================================================================
// ZombieRendezvous - finding a match without port forwarding (port-added).
//
// A small UDP server (server/rendezvous/rendezvous.c) introduces players and
// carries nothing else. The host registers from its game socket and gets a
// six-digit game code; a survivor lists the public games or types a code.
// The server hands each side the other's public address (as it saw it) and
// LAN address, both send PUNCH packets at the other at once, and each NAT
// lets the other's packets in. The first PUNCH or PUNCH_ACK to arrive names
// the address the survivor's normal HELLO then goes to - the match itself is
// the plain ZombieNet link, peer to peer. When no punch gets through, the
// server relays that one link (the others stay direct).
//
// Everything here runs on ZombieNet's one socket: the port the server sees
// must be the port the game plays on.
//
// The wire format is ZombieRendezvousWire.h, shared with the server
// (server/rendezvous/rendezvous.c).
// ============================================================================

// The server players use when config.ini sets none ([Net] Rendezvous=host:port;
// Rendezvous=off turns it off). Empty: no default server yet.
#define ZM_RDV_DEFAULT_SERVER ""

// ---- configuration (config.ini [Net] Rendezvous / Public) ----
extern char g_zmRdvServer[96];       // "host" or "host:port"; "" or "off" = none
extern bool g_zmRdvPublic;           // the host lists its game
bool zm_rdv_available(void);

// ---- ZombieNet's side ----
bool zm_rdv_is_packet(const unsigned char* buf, int len);
// A rendezvous packet from the socket. TRUE: it was a relayed game datagram -
// *inner / *innerLen is that datagram and *innerFrom the relayed peer's
// made-up address, for net_handle.
bool zm_rdv_take(const unsigned char* buf, int len, const PlatNetAddr* from,
                 const unsigned char** inner, int* innerLen, PlatNetAddr* innerFrom);
// ZombieNet's every send: a relayed peer's address goes through the server.
BOOL zm_rdv_net_send(const PlatNetAddr* to, const void* data, int length);
bool zm_rdv_relayed(void);           // this survivor joined through the relay
void zm_rdv_host_begin(void);        // the host's socket is open
void zm_rdv_host_end(void);          // ... and about to close (also closes any relay session)
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
