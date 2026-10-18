"""Rendezvous end to end, CPU only. Never launches the game.

The production client (src/game/mods/ZombieRendezvous.cpp) is built twice
as shared libraries - a host copy and a survivor copy, each with its own
statics - and driven against the production server (server/rendezvous/
rendezvous.c, built as a library around its rdv_handle) over a simulated
internet with NAT routers in front of the players. A case passes when the
survivor's ZombieNet would join the host at an address that game packets
then cross, both ways.

Also: the server's own rules (listing, codes, join replies, rate limit,
expiry, the relay and its caps), a fuzz pass of random and truncated packets through rdv_handle
under AddressSanitizer / UBSan, and the real server binary answering
tools/rendezvous_probe.py on a localhost socket.

    python3 tests/test_zombie_rendezvous.py
"""
import ctypes
import os
import re
import socket
import struct
import subprocess
import sys
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SERVER_C = ROOT / "server/rendezvous/rendezvous.c"


class rdv:
    """The wire constants (ZombieRendezvousWire.h) and the C server, wrapped."""
    MAGIC, PROTOCOL = 0x52314552, 1
    REGISTER, REGISTERED, LIST_REQ, LIST, JOIN, PEER, INTRO, JOIN_FAIL = 1, 2, 3, 4, 5, 6, 7, 8
    PUNCH, PUNCH_ACK, PROBE, PROBE_REPLY, UNREGISTER = 9, 10, 11, 12, 13
    RELAY_REQ, RELAY_OK, RELAY_OPEN, RELAY_FAIL, RELAY_DATA, RELAY_CLOSE = 14, 15, 16, 17, 18, 19
    FAIL_NOT_FOUND, FAIL_VERSION, FAIL_FULL, FAIL_BUSY, FAIL_NO_RELAY = 1, 2, 3, 4, 5
    HEADER = struct.Struct("<IBBH")
    REGISTER_BODY = struct.Struct("<IBBBB")
    JOIN_BODY = struct.Struct("<IB3x")
    lib = None

    @staticmethod
    def ip_to_int(text):
        a, b, c, d = (int(x) for x in text.split("."))
        return (a << 24) | (b << 16) | (c << 8) | d

    @staticmethod
    def int_to_ip(v):
        return "%d.%d.%d.%d" % ((v >> 24) & 255, (v >> 16) & 255, (v >> 8) & 255, v & 255)

    @staticmethod
    def pack_addr(ip_int, port):
        return struct.pack("<IHH", ip_int & 0xFFFFFFFF, port & 0xFFFF, 0)


class Endpoint(ctypes.Structure):
    _fields_ = [("ip", ctypes.c_uint32), ("port", ctypes.c_uint16)]


class Out(ctypes.Structure):
    _fields_ = [("to", Endpoint), ("len", ctypes.c_int), ("data", ctypes.c_ubyte * 1212)]


class Rendezvous:
    """The C server behind the Python server's old interface: handle(data,
    (ip text, port)) -> [(bytes, (ip text, port))], on the test's clock."""

    def __init__(self, clock, relays=64, session_bps=384 * 1024, total_bps=8 * 1024 * 1024, month=150 << 30):
        self.clock = clock
        self.out = (Out * 2)()
        rdv.lib.rdv_reset(ctypes.c_uint64(0x1234567))
        rdv.lib.rdv_set_relay_limits(relays, ctypes.c_double(session_bps), ctypes.c_double(total_bps),
                                     ctypes.c_uint64(month))

    def now(self):
        return ctypes.c_uint64(int(self.clock() * 1000) + 1000000)

    def handle(self, data, src):
        n = rdv.lib.rdv_handle(data, len(data), rdv.ip_to_int(src[0]), src[1], self.now(), self.out)
        return [(bytes(self.out[i].data[:self.out[i].len]),
                 (rdv.int_to_ip(self.out[i].to.ip), self.out[i].to.port)) for i in range(n)]

    def expire(self):
        rdv.lib.rdv_expire(self.now())

    def games(self):
        return rdv.lib.rdv_game_count()

    def relays(self):
        return rdv.lib.rdv_relay_count()


SHIM = r'''
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
typedef int BOOL;
#define TRUE 1
#define FALSE 0
struct PlatNetAddr { unsigned int ip; unsigned short port; };
enum { ZM_NET_IDLE, ZM_NET_HOSTING, ZM_NET_JOINING, ZM_NET_CONNECTED, ZM_NET_LOST, ZM_NET_FAILED, ZM_NET_EXPIRED };
enum { ZM_LOBBY_JOIN = 0, ZM_LOBBY_MAP = 1 };

typedef int  (*SendFn)(unsigned int ip, unsigned short port, const void* data, int len);
typedef int  (*RecvFn)(void* buf, int cap, unsigned int* ip, unsigned short* port);
static SendFn s_send; static RecvFn s_recv;
static unsigned int s_now = 1000, s_localIp; static unsigned short s_localPort, s_nextPort = 40000;
static bool s_open;
static int s_status = ZM_NET_IDLE, s_joinedCode, s_rejoin, s_ai, s_phase, s_chars[4] = {-2, -1, -1, -1};
static unsigned int s_joinIp; static unsigned short s_joinPort;
static char s_fail[96];
static int s_gameCount, s_gameLen; static unsigned int s_gameFromIp; static unsigned short s_gameFromPort;

extern "C" {
void t_init(SendFn send, RecvFn recv, unsigned int localIp)
{ s_send = send; s_recv = recv; s_localIp = localIp; s_rejoin = 0; s_status = ZM_NET_IDLE; s_fail[0] = 0; }
void t_tick(unsigned int ms) { s_now += ms; }
int  t_status(void) { return s_status; }
unsigned int t_join_ip(void) { return s_joinIp; }
unsigned short t_join_port(void) { return s_joinPort; }
int  t_join_code(void) { return s_joinedCode; }
int  t_rejoin(void) { return s_rejoin; }
int  t_game_count(void) { return s_gameCount; }
int  t_game_len(void) { return s_gameLen; }
unsigned int t_game_from_ip(void) { return s_gameFromIp; }
unsigned short t_game_from_port(void) { return s_gameFromPort; }
const char* t_fail(void) { return s_fail; }
unsigned short t_port(void) { return s_localPort; }
void t_seat(int i, int c) { s_chars[i] = c; }
void t_phase(int p) { s_phase = p; }
}

unsigned int plat_time_ms(void) { return s_now; }
BOOL plat_net_open(unsigned short port) { s_open = true; s_localPort = port ? port : s_nextPort++; return TRUE; }
void plat_net_close(void) { s_open = false; }
BOOL plat_net_resolve(const char* host, unsigned short port, PlatNetAddr* out)
{
    unsigned a, b, c, d;
    if (sscanf(host, "%u.%u.%u.%u", &a, &b, &c, &d) != 4) return FALSE;
    out->ip = a << 24 | b << 16 | c << 8 | d; out->port = port; return TRUE;
}
BOOL plat_net_send(const PlatNetAddr* to, const void* data, int length)
{ return s_open && s_send(to->ip, to->port, data, length); }
int plat_net_recv(void* buffer, int capacity, PlatNetAddr* from)
{
    if (!s_open) return -1;
    unsigned int ip; unsigned short port;
    int n = s_recv(buffer, capacity, &ip, &port);
    if (n > 0) { from->ip = ip; from->port = port; }
    return n;
}
BOOL plat_net_local(const PlatNetAddr*, PlatNetAddr* out)
{ if (!s_open) return FALSE; out->ip = s_localIp; out->port = s_localPort; return TRUE; }
unsigned int plat_random_u32(void)
{ static unsigned int s = 0x9E3779B9u; s = s * 1664525u + 1013904223u; return s ^ (s >> 16); }
void dbg_printf(const char* fmt, ...)
{ va_list a; va_start(a, fmt); vfprintf(stderr, fmt, a); va_end(a); }

int  zm_net_protocol_version(void) { return 77; }
int  zm_net_ai_level(void) { return s_ai; }
int  zm_net_seat_limit(void) { return 4; }
int  zm_net_char(int i) { return s_chars[i]; }
bool zm_net_lobby_started(void) { return false; }
int  zm_net_lobby_phase(void) { return s_phase; }
int  zm_net_status(void) { return s_status; }
void zm_net_fail(const char* text, const char* hint) { s_status = ZM_NET_FAILED; snprintf(s_fail, sizeof(s_fail), "%s", text); }
void zm_net_join_peer(const PlatNetAddr* host, int, unsigned int code)
{ s_status = ZM_NET_JOINING; s_joinIp = host->ip; s_joinPort = host->port; s_joinedCode = (int)code; }
bool zm_net_rejoin_peer(const PlatNetAddr* host, unsigned int code)
{ zm_net_join_peer(host, -1, code); s_rejoin = 1; return true; }
'''

EXPORTS = r'''
extern "C" {
void t_host_begin(void) { plat_net_open(27960); s_status = ZM_NET_HOSTING; zm_rdv_host_begin(); }
void t_host_end(void) { zm_rdv_host_end(); }
unsigned int t_code(void) { return zm_rdv_code(); }
int  t_host_state(void) { return zm_rdv_host_state(); }
// What zm_net_poll does on the host: the socket, then the heartbeat.
// A game datagram (direct, or unwrapped from the relay) is counted with
// the address net_handle would see it from.
static void t_game(const unsigned char* p, int n, const PlatNetAddr* from)
{
    if (n < 8 || memcmp(p, "RE1Zgame", 8) != 0) return;
    s_gameCount++; s_gameLen = n; s_gameFromIp = from->ip; s_gameFromPort = from->port;
}
void t_host_poll(void)
{
    static unsigned char buf[2048]; PlatNetAddr from;
    for (;;) {
        int n = plat_net_recv(buf, sizeof(buf), &from); if (n <= 0) break;
        const unsigned char* inner; int innerLen; PlatNetAddr innerFrom;
        if (!zm_rdv_is_packet(buf, n)) t_game(buf, n, &from);
        else if (zm_rdv_take(buf, n, &from, &inner, &innerLen, &innerFrom) && !zm_rdv_is_packet(inner, innerLen))
            t_game(inner, innerLen, &innerFrom);
    }
    zm_rdv_frame();
}
// The survivor after the hand-over: ZombieNet reads, punches still answered.
void t_net_poll(void) { t_host_poll(); }
int  t_browse_begin(void) { return zm_rdv_browse_begin(); }
void t_browse_frame(void) { zm_rdv_browse_frame(); }
int  t_list_count(void) { return zm_rdv_list_count(); }
unsigned int t_list_code(int i) { return zm_rdv_list(i)->code; }
void t_connect_begin(unsigned int code, int rejoin) { zm_rdv_connect_begin(code, rejoin != 0); }
int  t_connect_frame(void) { return zm_rdv_connect_frame(); }
void t_set_server(const char* s) { snprintf(g_zmRdvServer, sizeof(g_zmRdvServer), "%s", s); }
void t_set_public(int on) { g_zmRdvPublic = on != 0; }
int  t_relayed(void) { return zm_rdv_relayed(); }
// What ZombieNet sends: through zm_rdv_net_send, which relays if it must.
int  t_send_game(unsigned int ip, unsigned short port, int len)
{
    static unsigned char d[1300]; PlatNetAddr a = { ip, port };
    memset(d, 0x5A, sizeof(d)); memcpy(d, "RE1Zgame", 8);
    return zm_rdv_net_send(&a, d, len);
}
}
'''


def build(directory):
    header = (ROOT / "src/game/mods/ZombieRendezvous.h").read_text()
    wire = (ROOT / "src/game/mods/ZombieRendezvousWire.h").read_text().replace("#pragma once", "")
    header = header.replace('#include "../../platform/platform.h"', "").replace("#pragma once", "")
    header = header.replace('#include "ZombieRendezvousWire.h"', wire)
    source = (ROOT / "src/game/mods/ZombieRendezvous.cpp").read_text()
    source = re.sub(r'#include "[^"]*"\n', "", source)
    unit = SHIM + header + source + EXPORTS
    cpp = Path(directory) / "rdv.cpp"
    cpp.write_text(unit)
    server = Path(directory) / "rdv_server.so"
    subprocess.run(["gcc", "-std=c11", "-O2", "-Wall", "-Wextra", "-shared", "-fPIC", "-DRDV_NO_MAIN",
                    str(SERVER_C), "-o", str(server)], check=True)
    rdv.lib = ctypes.CDLL(str(server))
    rdv.lib.rdv_handle.argtypes = [ctypes.c_char_p, ctypes.c_int, ctypes.c_uint32, ctypes.c_uint16,
                                   ctypes.c_uint64, ctypes.POINTER(Out)]
    rdv.lib.rdv_expire.argtypes = [ctypes.c_uint64]
    rdv.lib.rdv_reset.argtypes = [ctypes.c_uint64]
    rdv.lib.rdv_set_verbose(0)
    rdv.lib.rdv_set_relay_limits.argtypes = [ctypes.c_int, ctypes.c_double, ctypes.c_double, ctypes.c_uint64]
    rdv.lib.rdv_relay_month_bytes.restype = ctypes.c_uint64
    rdv.lib.rdv_relay_dropped.restype = ctypes.c_uint64
    assert rdv.lib.rdv_out_bytes() == ctypes.sizeof(Out), "rdv_out layout"
    libs = []
    for name in ("host", "survivor"):
        out = Path(directory) / ("rdv_%s.so" % name)
        subprocess.run(["g++", "-std=c++17", "-shared", "-fPIC", "-Wall", "-Wno-unused-function",
                        "-Wl,-Bsymbolic", str(cpp), "-o", str(out)], check=True)
        libs.append(ctypes.CDLL(str(out)))
    return libs


def ip(s):
    return rdv.ip_to_int(s)


SERVER = (ip("20.1.2.3"), 27961)


class Nat:
    """A home router. cone: one public port per private socket, for every
    destination; symmetric: a public port per destination. Inbound is let in
    only from where that mapping has sent to - that address and port, or
    with by_address that address on any port. The router does not loop its
    public address back."""

    def __init__(self, public, symmetric=False, by_address=False):
        self.public, self.symmetric, self.by_address = public, symmetric, by_address
        self.out = {}          # (private, dest or None) -> public port
        self.back = {}         # public port -> private
        self.allowed = set()   # (public port, remote)
        self.next = 50000

    def outbound(self, private, dest):
        key = (private, dest if self.symmetric else None)
        if key not in self.out:
            self.next += 7
            self.out[key] = self.next
            self.back[self.next] = private
        port = self.out[key]
        self.allowed.add((port, dest))
        self.allowed.add((port, dest[0]))
        return (self.public, port)

    def inbound(self, port, remote):
        if (port, remote[0] if self.by_address else remote) not in self.allowed:
            return None
        return self.back.get(port)


class Net:
    def __init__(self, server):
        self.server = server
        self.queues = {}       # private endpoint -> [(from, bytes)]
        self.hosts = {}        # endpoint name -> (lib, private ip, nat)
        self.dropped = 0
        self.drop_intro = 0
        self.game_seen = []

    def attach(self, lib, private_ip, nat):
        net = self
        send_t = ctypes.CFUNCTYPE(ctypes.c_int, ctypes.c_uint, ctypes.c_ushort, ctypes.c_void_p, ctypes.c_int)
        recv_t = ctypes.CFUNCTYPE(ctypes.c_int, ctypes.c_void_p, ctypes.c_int,
                                  ctypes.POINTER(ctypes.c_uint), ctypes.POINTER(ctypes.c_ushort))

        def send(dip, dport, data, length):
            payload = ctypes.string_at(data, length)
            net.route((private_ip, lib.t_port()), nat, (dip, dport), payload)
            return 1

        def recv(buf, cap, pip, pport):
            q = net.queues.get((private_ip, lib.t_port()))
            if not q:
                return 0
            src, payload = q.pop(0)
            ctypes.memmove(buf, payload, len(payload))
            pip[0], pport[0] = src
            return len(payload)

        lib._cbs = (send_t(send), recv_t(recv))
        lib.t_init(lib._cbs[0], lib._cbs[1], private_ip)
        lib.t_fail.restype = ctypes.c_char_p
        lib.t_join_port.restype = ctypes.c_ushort
        lib.t_port.restype = ctypes.c_ushort
        lib.t_code.restype = ctypes.c_uint
        lib.t_list_code.restype = ctypes.c_uint
        lib.t_join_ip.restype = ctypes.c_uint
        lib.t_game_from_ip.restype = ctypes.c_uint
        lib.t_game_from_port.restype = ctypes.c_ushort
        self.hosts[lib] = (private_ip, nat)

    def nat_of_public(self, public_ip):
        for private_ip, nat in self.hosts.values():
            if nat is not None and nat.public == public_ip:
                return nat
        return None

    def route(self, src_private, nat, dest, payload):
        # Same LAN: straight across, no router involved.
        for private_ip, other in self.hosts.values():
            if dest[0] == private_ip and other is nat:
                self.deliver(dest, src_private, payload)
                return
        src_public = nat.outbound(src_private, dest) if nat else src_private
        if dest == SERVER:
            for out, (dip_s, dport) in self.server.handle(payload, (rdv.int_to_ip(src_public[0]), src_public[1])):
                kind = out[5]
                if kind == rdv.INTRO and self.drop_intro:
                    self.drop_intro -= 1
                    continue
                self.route_from_server((ip(dip_s), dport), out)
            return
        dnat = self.nat_of_public(dest[0])
        if dnat is None:
            # Another LAN's private address does not route over the internet.
            if any(dest[0] == pip and other is not None for pip, other in self.hosts.values()):
                self.dropped += 1
                return
            self.deliver(dest, src_public, payload)
            return
        if dnat is nat:            # no hairpin
            self.dropped += 1
            return
        private = dnat.inbound(dest[1], src_public)
        if private is None:
            self.dropped += 1
            return
        self.deliver(private, src_public, payload)

    def route_from_server(self, dest, payload):
        dnat = self.nat_of_public(dest[0])
        private = dnat.inbound(dest[1], SERVER) if dnat else dest
        if private is None:
            self.dropped += 1
            return
        self.deliver(private, SERVER, payload)

    def deliver(self, private, src, payload):
        if payload.startswith(b"RE1Zgame"):
            self.game_seen.append((private, src))
        self.queues.setdefault(private, []).append((src, payload))


def run_case(libs, name, host_nat, surv_nat, host_ip="192.168.1.10", surv_ip="10.0.0.5",
             drop_intro=0, code_offset=0, expect_ok=True, rejoin=False, browse=False,
             relayed=False, relays=64):
    host, surv = libs
    clock = [0.0]
    server = Rendezvous(clock=lambda: clock[0], relays=relays)
    net = Net(server)
    net.drop_intro = drop_intro
    net.attach(host, ip(host_ip), host_nat)
    net.attach(surv, ip(surv_ip), surv_nat)
    for lib in libs:
        lib.t_set_server(b"20.1.2.3")
        lib.t_set_public(1)

    def tick(ms=16):
        clock[0] += ms / 1000.0
        for lib in libs:
            lib.t_tick(ms)
        host.t_host_poll()

    host.t_host_begin()
    for _ in range(10):
        tick()
    code = host.t_code()
    assert code and host.t_host_state() == 2, (name, "host has no code")

    if browse:
        assert surv.t_browse_begin()
        for _ in range(10):
            surv.t_browse_frame()
            tick()
        assert surv.t_list_count() == 1 and surv.t_list_code(0) == code, (name, "list")
        host.t_set_public(0)
        for _ in range(30):
            tick()
        surv.t_browse_begin()
        for _ in range(10):
            surv.t_browse_frame()
            tick()
        assert surv.t_list_count() == 0, (name, "a private game is listed")

    surv.t_connect_begin(code + code_offset, 1 if rejoin else 0)
    result = 0
    for _ in range(1500):
        result = surv.t_connect_frame()
        tick()
        if result:
            break
    if not expect_ok:
        assert result == 2, (name, "should have failed", result)
        return surv.t_fail().decode()
    assert result == 1, (name, "did not connect", surv.t_fail())
    assert surv.t_join_code() == code
    assert bool(surv.t_rejoin()) == rejoin
    assert bool(surv.t_relayed()) == relayed, (name, "relayed" if relayed else "not relayed")
    assert (server.relays() == 1) == relayed, (name, "relay sessions", server.relays())
    # The survivor's HELLOs go to that address: they must cross, and the
    # host's answers to their source must cross back - the largest datagram
    # ZombieNet sends too.
    to = (surv.t_join_ip(), surv.t_join_port())
    for size in (8, 1200):
        before = host.t_game_count()
        assert surv.t_send_game(to[0], to[1], size), (name, "send")
        tick()
        assert host.t_game_count() == before + 1 and host.t_game_len() == size, (name, "survivor -> host blocked")
        back = (host.t_game_from_ip(), host.t_game_from_port())
        before = surv.t_game_count()
        host.t_send_game(back[0], back[1], size)
        surv.t_net_poll()
        assert surv.t_game_count() == before + 1 and surv.t_game_len() == size, (name, "host -> survivor blocked")
        assert (surv.t_game_from_ip(), surv.t_game_from_port()) == to, (name, "answer from another address")
    # The host's punches keep arriving after the hand-over: answered, harmless.
    for _ in range(20):
        surv.t_net_poll()
        tick()
    host.t_host_end()
    surv.t_host_end()                      # zm_net_stop: the relay session closes
    assert server.games() == 0, (name, "UNREGISTER left the code")
    assert server.relays() == 0, (name, "RELAY_CLOSE left the session")
    where = "%s:%d" % (rdv.int_to_ip(to[0]), to[1])
    return "relayed (as %s)" % where if relayed else where


def server_checks():
    clock = [0.0]
    s = Rendezvous(clock=lambda: clock[0])
    hdr = lambda t: rdv.HEADER.pack(rdv.MAGIC, rdv.PROTOCOL, t, 0)
    reg = lambda token, flags=1, surv=0, cap=3, ver=77: hdr(rdv.REGISTER) + rdv.REGISTER_BODY.pack(
        token, ver, flags, surv, cap) + rdv.pack_addr(ip("192.168.0.2"), 27960)
    code_of = lambda out: struct.unpack_from("<I", out[0][0], 8)[0]
    a = ("5.5.5.5", 1000)
    out = s.handle(reg(1), a)
    code = code_of(out)
    assert 100000 <= code <= 999999
    seen_ip, seen_port = struct.unpack_from("<IH", out[0][0], 12)
    assert (seen_ip, seen_port) == (ip(a[0]), a[1]) and out[0][1] == a, "REGISTERED names the public address"
    assert code_of(s.handle(reg(1), a)) == code, "re-register keeps the code"
    private = code_of(s.handle(reg(2, flags=0), ("6.6.6.6", 1)))
    started = code_of(s.handle(reg(3, flags=1 | 2), ("7.7.7.7", 1)))
    full = code_of(s.handle(reg(4, surv=3), ("8.8.8.8", 1)))
    other = code_of(s.handle(reg(5, ver=76), ("9.9.9.9", 1)))
    assert len({code, private, started, full, other}) == 5 and s.games() == 5
    lst = s.handle(hdr(rdv.LIST_REQ) + struct.pack("<B3x", 77), ("1.1.1.1", 9))[0][0]
    assert lst[8] == 1 and struct.unpack_from("<I", lst, 12)[0] == code, "only the open public game is listed"
    join = lambda c, ver=77, src=("1.1.1.1", 9): s.handle(
        hdr(rdv.JOIN) + rdv.JOIN_BODY.pack(c, ver) + rdv.pack_addr(ip("10.0.0.2"), 4000), src)
    out = join(code)
    assert [o[0][5] for o in out] == [rdv.PEER, rdv.INTRO] and out[1][1] == a and out[0][1] == ("1.1.1.1", 9)
    peer_code, host_ip, host_port = struct.unpack_from("<IIH", out[0][0], 8)
    assert (peer_code, host_ip, host_port) == (code, ip(a[0]), a[1]), "PEER names the host"
    intro = struct.unpack_from("<IIHHIH", out[1][0], 8)
    assert intro[1:3] == (ip("1.1.1.1"), 9) and intro[4:6] == (ip("10.0.0.2"), 4000), "INTRO names the survivor"
    assert join(full)[0][0][12] == rdv.FAIL_FULL
    assert join(started)[0][0][5] == rdv.PEER, "a started game takes a rejoin"
    assert join(private)[0][0][5] == rdv.PEER, "a private game takes its code"
    assert join(other)[0][0][12] == rdv.FAIL_VERSION
    assert join(123)[0][0][12] == rdv.FAIL_NOT_FOUND
    for _ in range(rdv.lib.rdv_join_limit()):
        join(code, src=("2.2.2.2", 1))
    assert join(code, src=("2.2.2.2", 1))[0][0][12] == rdv.FAIL_BUSY
    clock[0] += 2.0                                     # the bucket refills a JOIN a second
    assert join(code, src=("2.2.2.2", 1))[0][0][5] == rdv.PEER, "the limit refills"
    # Someone else cannot close the game; its host can.
    s.handle(hdr(rdv.UNREGISTER) + struct.pack("<I", 1), ("6.6.6.6", 1))
    assert s.games() == 5
    s.handle(hdr(rdv.UNREGISTER) + struct.pack("<I", 2), ("6.6.6.6", 1))
    assert s.games() == 4
    # A host that stops heartbeating is gone from the list and from JOIN at
    # once, and from the table at the next sweep.
    clock[0] += rdv.lib.rdv_expire_ms() / 1000.0 + 1
    s.handle(reg(4, surv=3), ("8.8.8.8", 1))           # this one is still alive
    assert join(code)[0][0][12] == rdv.FAIL_NOT_FOUND
    s.expire()
    assert s.games() == 1, "silent hosts expire"
    assert s.handle(b"junk", a) == [] and s.handle(hdr(99), a) == []
    assert s.handle(hdr(rdv.JOIN) + b"\x01", a) == [], "a short body is ignored"
    assert s.handle(reg(0), a) == [], "token 0 is refused"
    # Many games: the tables hold, codes stay unique, the list stays capped
    # and oldest first.
    s = Rendezvous(clock=lambda: clock[0])
    codes = set()
    for i in range(3000):
        codes.add(code_of(s.handle(reg(1000 + i), ("%d.%d.0.1" % (10 + i // 250, i % 250), 5000))))
    assert len(codes) == 3000 and s.games() == 3000
    lst = s.handle(hdr(rdv.LIST_REQ) + struct.pack("<B3x", 77), ("1.1.1.1", 9))[0][0]
    assert lst[8] == 64 and len(lst) == 8 + 4 + 64 * 8
    for i in range(0, 3000, 2):
        s.handle(hdr(rdv.UNREGISTER) + struct.pack("<I", 1000 + i), ("%d.%d.0.1" % (10 + i // 250, i % 250), 5000))
    assert s.games() == 1500
    for i in range(3000, 6000):                          # tombstones reused / rebuilt
        s.handle(reg(1000 + i), ("%d.%d.0.2" % (10 + i // 250, i % 250), 5000))
    assert s.games() == 4500


def relay_checks():
    clock = [0.0]
    hdr = lambda t: rdv.HEADER.pack(rdv.MAGIC, rdv.PROTOCOL, t, 0)
    host = ("5.5.5.5", 1000)

    def setup(**limits):
        srv = Rendezvous(clock=lambda: clock[0], **limits)
        out = srv.handle(hdr(rdv.REGISTER) + rdv.REGISTER_BODY.pack(1, 77, 1, 0, 3) +
                         rdv.pack_addr(ip("192.168.0.2"), 27960), host)
        return srv, struct.unpack_from("<I", out[0][0], 8)[0]

    def req(srv, src, code, ver=77):
        return srv.handle(hdr(rdv.RELAY_REQ) + struct.pack("<IB3x", code, ver), src)

    def data(srv, session, payload, src):
        return srv.handle(hdr(rdv.RELAY_DATA) + struct.pack("<I", session) + payload, src)

    s, code = setup(relays=3, session_bps=10000)
    a = ("1.1.1.1", 9)
    out = req(s, a, code)
    assert [o[0][5] for o in out] == [rdv.RELAY_OK, rdv.RELAY_OPEN] and out[0][1] == a and out[1][1] == host
    session = struct.unpack_from("<I", out[0][0], 12)[0]
    assert session and struct.unpack_from("<II", out[1][0], 8) == (code, session)
    assert struct.unpack_from("<IH", out[1][0], 16) == (ip(a[0]), a[1]), "OPEN names the survivor"
    assert struct.unpack_from("<I", req(s, a, code)[0][0], 12)[0] == session and s.relays() == 1, "a retry"
    pkt = hdr(rdv.RELAY_DATA) + struct.pack("<I", session) + b"x" * 100
    out = s.handle(pkt, a)
    assert out == [(pkt, host)], "survivor -> host, passed on as it came"
    assert data(s, session, b"y", host)[0][1] == a, "host -> survivor"
    assert data(s, session, b"z", ("6.6.6.6", 1)) == [], "a stranger's data is dropped"
    assert data(s, session, b"z", ("1.1.1.1", 10)) == [], "another port of the survivor's IP is a stranger"
    assert data(s, session ^ 1, b"z", a) == [], "an unknown session"
    assert data(s, session, b"o" * 1201, a) == [], "over a game datagram"
    assert len(data(s, session, b"o" * 1200, a)[0][0]) == 1212, "the largest passes"
    assert req(s, a, 123)[0][0][12] == rdv.FAIL_NOT_FOUND
    assert req(s, a, code, ver=76)[0][0][12] == rdv.FAIL_VERSION
    # The session cap.
    assert req(s, ("1.1.1.1", 10), code)[0][0][5] == rdv.RELAY_OK
    assert req(s, ("2.2.2.2", 1), code)[0][0][5] == rdv.RELAY_OK
    assert req(s, ("3.3.3.3", 1), code)[0][0][12] == rdv.FAIL_NO_RELAY and s.relays() == 3
    # The per-session rate: a second's worth of burst, then drops, then refilled.
    clock[0] += 1.0
    big = b"b" * 1200
    passed = sum(1 for _ in range(12) if data(s, session, big, a))
    assert passed == 10000 // 1212, passed
    assert rdv.lib.rdv_relay_dropped() >= 1
    clock[0] += 1.0
    assert data(s, session, big, a), "refilled"
    # Only a member closes a session.
    s.handle(hdr(rdv.RELAY_CLOSE) + struct.pack("<I", session), ("6.6.6.6", 1))
    assert s.relays() == 3
    s.handle(hdr(rdv.RELAY_CLOSE) + struct.pack("<I", session), host)
    assert s.relays() == 2 and data(s, session, b"q", a) == []
    # Silent sessions close.
    clock[0] += 61
    s.handle(hdr(rdv.REGISTER) + rdv.REGISTER_BODY.pack(1, 77, 1, 0, 3) + rdv.pack_addr(0, 0), host)
    s.expire()
    assert s.relays() == 0, "idle sessions close"

    # The host's router moves its port: the session follows it.
    s, code = setup()
    out = req(s, a, code)
    session = struct.unpack_from("<I", out[0][0], 12)[0]
    moved = ("5.5.5.5", 2000)
    s.handle(hdr(rdv.REGISTER) + rdv.REGISTER_BODY.pack(1, 77, 1, 0, 3) + rdv.pack_addr(0, 0), moved)
    assert data(s, session, b"m", a)[0][1] == moved
    assert data(s, session, b"m", moved)[0][1] == a
    assert data(s, session, b"m", host) == [], "the old port is a stranger now"

    # Per survivor IP and per game.
    s, code = setup()
    oks = [req(s, ("9.9.9.9", p), code)[0][0][5] for p in range(1, 6)]
    assert oks == [rdv.RELAY_OK] * 4 + [rdv.RELAY_FAIL], oks
    oks = [req(s, ("8.8.8.%d" % i, 1), code)[0][0][5] for i in range(1, 4)]
    assert oks == [rdv.RELAY_OK] * 2 + [rdv.RELAY_FAIL], oks

    # The month's budget: no new sessions once used up; open ones carry on.
    s, code = setup(month=5000)
    out = req(s, a, code)
    session = struct.unpack_from("<I", out[0][0], 12)[0]
    for _ in range(5):
        assert data(s, session, big, a)
    assert rdv.lib.rdv_relay_month_bytes() == 5 * 1212
    assert req(s, ("2.2.2.2", 1), code)[0][0][12] == rdv.FAIL_NO_RELAY
    assert data(s, session, big, a), "an open session is not cut off"
    rdv.lib.rdv_set_month(1)
    assert rdv.lib.rdv_relay_month_bytes() == 0, "a new month"
    assert req(s, ("2.2.2.2", 1), code)[0][0][5] == rdv.RELAY_OK

    # Relay off.
    s, code = setup(relays=0)
    assert req(s, a, code)[0][0][12] == rdv.FAIL_NO_RELAY


FUZZ = r'''
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef struct rdv_endpoint { unsigned int ip; unsigned short port; } rdv_endpoint;
typedef struct rdv_out { rdv_endpoint to; int len; unsigned char data[1212]; } rdv_out;
void rdv_reset(unsigned long long);
void rdv_set_verbose(int);
void rdv_expire(unsigned long long);
unsigned long long rdv_relay_month_bytes(void);
int rdv_handle(const unsigned char*, int, unsigned int, unsigned short, unsigned long long, rdv_out*);
int main(void)
{
    static rdv_out out[2];
    unsigned char pkt[1300];
    unsigned long long now = 1000000;
    unsigned int x = 12345;
    rdv_reset(7); rdv_set_verbose(0);
    /* A game and a relay session at fixed addresses, so random RELAY_DATA
     * from them reaches the forwarding path. */
    unsigned char reg[24] = {0}, req[16] = {0};
    unsigned int magic = 0x52314552u, token = 99, code = 0, session = 0;
    memcpy(reg, &magic, 4); reg[4] = 1; reg[5] = 1; memcpy(reg + 8, &token, 4); reg[12] = 77; reg[13] = 1; reg[15] = 3;
    if (rdv_handle(reg, 24, 0x05050505, 1000, now, out) == 1) memcpy(&code, out[0].data + 8, 4);
    memcpy(req, &magic, 4); req[4] = 1; req[5] = 14; memcpy(req + 8, &code, 4); req[12] = 77;
    if (rdv_handle(req, 16, 0x01010101, 9, now, out) == 2) memcpy(&session, out[0].data + 12, 4);
    if (!session) { puts("no relay session"); return 1; }
    for (int i = 0; i < 400000; i++) {
        x ^= x << 13; x ^= x >> 17; x ^= x << 5;
        int len = (int)(x % 40);
        if (i % 97 == 0) len = (int)(x % 1300);
        for (int k = 0; k < len; k++) { x ^= x << 13; x ^= x >> 17; x ^= x << 5; pkt[k] = (unsigned char)x; }
        unsigned int srcIp = x & 0xFFFF; unsigned short srcPort = (unsigned short)(x >> 3);
        if (len >= 6 && (i & 3)) {                /* a valid header, a random body */
            memcpy(pkt, &magic, 4);
            pkt[4] = 1; pkt[5] = (unsigned char)(1 + (x >> 8) % 20);
        }
        if (i % 7 == 0) {                         /* relay data on the live session */
            len = 12 + (int)(x % 1220);
            memcpy(pkt, &magic, 4); pkt[4] = 1; pkt[5] = 18; memcpy(pkt + 8, &session, 4);
            if (x & 1) { srcIp = 0x01010101; srcPort = 9; } else { srcIp = 0x05050505; srcPort = 1000; }
        }
        /* Exactly len bytes on the heap: a read past the end hits ASan's redzone. */
        unsigned char* exact = (unsigned char*)malloc(len ? (size_t)len : 1);
        memcpy(exact, pkt, (size_t)len);
        int n = rdv_handle(exact, len, srcIp, srcPort, now, out);
        free(exact);
        if (n < 0 || n > 2) { printf("bad count %d\n", n); return 1; }
        for (int k = 0; k < n; k++) if (out[k].len < 8 || out[k].len > 1212) { printf("bad len\n"); return 1; }
        now += x % 50;
        if (i % 5000 == 0) rdv_expire(now);
    }
    if (!rdv_relay_month_bytes()) { puts("nothing relayed"); return 1; }
    puts("fuzz ok");
    return 0;
}
'''


def fuzz(directory):
    exe = Path(directory) / "fuzz"
    src = Path(directory) / "fuzz.c"
    src.write_text(FUZZ)
    subprocess.run(["gcc", "-std=c11", "-O1", "-g", "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                    "-DRDV_NO_MAIN", str(SERVER_C), str(src), "-o", str(exe)], check=True)
    r = subprocess.run([str(exe)], capture_output=True, text=True, timeout=600)
    assert r.returncode == 0 and "fuzz ok" in r.stdout, r.stdout + r.stderr


def smoke(directory):
    exe = Path(directory) / "rendezvous"
    subprocess.run(["gcc", "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror", str(SERVER_C), "-o", str(exe)],
                   check=True)
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as probe_port:
        probe_port.bind(("127.0.0.1", 0))
        port = probe_port.getsockname()[1]
    proc = subprocess.Popen([str(exe), "--bind", "127.0.0.1", "--port", str(port), "-q"])
    try:
        time.sleep(0.3)
        r = subprocess.run([sys.executable, "-I", str(ROOT / "tools/rendezvous_probe.py"), "127.0.0.1", str(port)],
                           capture_output=True, text=True, timeout=30)
        assert r.returncode == 0 and "OK" in r.stdout, r.stdout + r.stderr
    finally:
        proc.terminate()
        proc.wait(timeout=5)


def main():
    with tempfile.TemporaryDirectory(prefix="re1-rdv-") as d:
        libs = build(d)
        server_checks()
        print("Server: listing, codes, join replies, rate limit, expiry and table checks passed.")
        relay_checks()
        print("Server: relay sessions, forwarding, caps, month budget and expiry checks passed.")
        fuzz(d)
        print("Server: 400000 fuzzed packets under ASan / UBSan passed.")
        smoke(d)
        print("Server: the binary answers the probe over a real socket.")
        H, S = "1.1.1.1", "2.2.2.2"
        cases = [
            ("both behind cone NATs", Nat(ip(H)), Nat(ip(S)), {}),
            ("the INTRO lost twice", Nat(ip(H)), Nat(ip(S)), {"drop_intro": 2}),
            ("host with a public address", None, Nat(ip(S)), {"host_ip": H}),
            ("symmetric survivor, address host", Nat(ip(H), by_address=True), Nat(ip(S), symmetric=True), {}),
            ("symmetric host, address survivor", Nat(ip(H), symmetric=True), Nat(ip(S), by_address=True), {}),
            ("rejoin by code", Nat(ip(H)), Nat(ip(S)), {"rejoin": True}),
            ("found in the public list", Nat(ip(H)), Nat(ip(S)), {"browse": True}),
        ]
        for name, hn, sn, kw in cases:
            print("  %-34s ok, joined %s" % (name, run_case(libs, name, hn, sn, **kw)))
        same = Nat(ip(H))
        print("  %-34s ok, joined %s" % ("same LAN, no hairpin", run_case(
            libs, "lan", same, same, surv_ip="192.168.1.20")))
        print("  %-34s ok, %s" % ("both symmetric", run_case(
            libs, "both symmetric", Nat(ip(H), symmetric=True), Nat(ip(S), symmetric=True), relayed=True)))
        print("  %-34s ok, %s" % ("both symmetric, rejoin", run_case(
            libs, "both symmetric, rejoin", Nat(ip(H), symmetric=True), Nat(ip(S), symmetric=True),
            relayed=True, rejoin=True)))
        why = run_case(libs, "relay off", Nat(ip(H), symmetric=True), Nat(ip(S), symmetric=True),
                       expect_ok=False, relays=0)
        assert why == "COULD NOT REACH THE HOST", why
        print("  %-34s fails as expected: %s" % ("both symmetric, relay off", why))
        why = run_case(libs, "bad code", Nat(ip(H)), Nat(ip(S)), code_offset=1, expect_ok=False)
        assert why == "NO GAME WITH THAT CODE", why
        print("  %-34s fails as expected: %s" % ("a wrong code", why))
    print("Rendezvous punch checks passed.")


if __name__ == "__main__":
    sys.exit(main())
