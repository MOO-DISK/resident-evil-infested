"""Rendezvous end to end, CPU only. Never launches the game.

The production client (src/game/mods/ZombieRendezvous.cpp) is built twice
as shared libraries - a host copy and a survivor copy, each with its own
statics - and driven against the production server logic
(server/rendezvous/rendezvous.py) over a simulated internet with NAT routers
in front of the players. A case passes when the survivor's ZombieNet would
join the host at an address that game packets then cross, both ways.

    python3 tests/test_zombie_rendezvous.py
"""
import ctypes
import importlib.util
import re
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

spec = importlib.util.spec_from_file_location("rendezvous", ROOT / "server/rendezvous/rendezvous.py")
rdv = importlib.util.module_from_spec(spec)
spec.loader.exec_module(rdv)

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

extern "C" {
void t_init(SendFn send, RecvFn recv, unsigned int localIp)
{ s_send = send; s_recv = recv; s_localIp = localIp; s_rejoin = 0; s_status = ZM_NET_IDLE; s_fail[0] = 0; }
void t_tick(unsigned int ms) { s_now += ms; }
int  t_status(void) { return s_status; }
unsigned int t_join_ip(void) { return s_joinIp; }
unsigned short t_join_port(void) { return s_joinPort; }
int  t_join_code(void) { return s_joinedCode; }
int  t_rejoin(void) { return s_rejoin; }
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
void t_host_poll(void)
{
    static unsigned char buf[2048]; PlatNetAddr from;
    for (;;) { int n = plat_net_recv(buf, sizeof(buf), &from); if (n <= 0) break;
               if (zm_rdv_is_packet(buf, n)) zm_rdv_take(buf, n, &from); }
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
void t_send_game(unsigned int ip, unsigned short port) { PlatNetAddr a = { ip, port }; plat_net_send(&a, "RE1Zgame", 8); }
}
'''


def build(directory):
    header = (ROOT / "src/game/mods/ZombieRendezvous.h").read_text()
    header = header.replace('#include "../../platform/platform.h"', "").replace("#pragma once", "")
    source = (ROOT / "src/game/mods/ZombieRendezvous.cpp").read_text()
    source = re.sub(r'#include "[^"]*"\n', "", source)
    unit = SHIM + header + source + EXPORTS
    cpp = Path(directory) / "rdv.cpp"
    cpp.write_text(unit)
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
             drop_intro=0, code_offset=0, expect_ok=True, rejoin=False, browse=False):
    host, surv = libs
    clock = [0.0]
    server = rdv.Rendezvous(clock=lambda: clock[0])
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
    # The survivor's HELLOs go to that address: they must cross, and the
    # host's answers to their source must cross back.
    to = (surv.t_join_ip(), surv.t_join_port())
    net.game_seen.clear()
    surv.t_send_game(*to)
    assert net.game_seen and net.game_seen[-1][0] == (ip(host_ip), 27960), (name, "survivor -> host blocked")
    back = net.game_seen[-1][1]
    host.t_send_game(*back)
    assert net.game_seen[-1][0] == (ip(surv_ip), surv.t_port()), (name, "host -> survivor blocked")
    # The host's punches keep arriving after the hand-over: answered, harmless.
    for _ in range(20):
        surv.t_net_poll()
        tick()
    host.t_host_end()
    assert not server.by_code, (name, "UNREGISTER left the code")
    return "%s:%d" % (rdv.int_to_ip(to[0]), to[1])


def server_checks():
    clock = [0.0]
    s = rdv.Rendezvous(clock=lambda: clock[0])
    hdr = lambda t: rdv.HEADER.pack(rdv.MAGIC, rdv.PROTOCOL, t, 0)
    reg = lambda token, flags=1, surv=0, cap=3, ver=77: hdr(rdv.REGISTER) + rdv.REGISTER_BODY.pack(
        token, ver, flags, surv, cap) + rdv.pack_addr(ip("192.168.0.2"), 27960)
    a = ("5.5.5.5", 1000)
    out = s.handle(reg(1), a)
    code = struct.unpack_from("<I", out[0][0], 8)[0]
    assert 100000 <= code <= 999999
    assert struct.unpack_from("<I", s.handle(reg(1), a)[0][0], 8)[0] == code, "re-register keeps the code"
    s.handle(reg(2, flags=0), ("6.6.6.6", 1))            # private
    s.handle(reg(3, flags=1 | 2), ("7.7.7.7", 1))        # started
    s.handle(reg(4, surv=3), ("8.8.8.8", 1))             # full
    s.handle(reg(5, ver=76), ("9.9.9.9", 1))             # other version
    lst = s.handle(hdr(rdv.LIST_REQ) + struct.pack("<B3x", 77), ("1.1.1.1", 9))[0][0]
    assert lst[8] == 1 and struct.unpack_from("<I", lst, 12)[0] == code, "only the open public game is listed"
    join = lambda c, ver=77, src=("1.1.1.1", 9): s.handle(
        hdr(rdv.JOIN) + rdv.JOIN_BODY.pack(c, ver) + rdv.pack_addr(ip("10.0.0.2"), 4000), src)
    out = join(code)
    assert [o[0][5] for o in out] == [rdv.PEER, rdv.INTRO] and out[1][1] == a
    full = s.by_token[4].code
    assert join(full)[0][0][12] == rdv.FAIL_FULL
    assert join(s.by_token[3].code)[0][0][5] == rdv.PEER, "a started game takes a rejoin"
    assert join(s.by_token[5].code)[0][0][12] == rdv.FAIL_VERSION
    assert join(123)[0][0][12] == rdv.FAIL_NOT_FOUND
    for _ in range(rdv.JOIN_LIMIT):
        join(code, src=("2.2.2.2", 1))
    assert join(code, src=("2.2.2.2", 1))[0][0][12] == rdv.FAIL_BUSY
    # Someone else cannot close the game.
    s.handle(hdr(rdv.UNREGISTER) + struct.pack("<I", 1), ("6.6.6.6", 1))
    assert code in s.by_code
    clock[0] += rdv.HOST_EXPIRE_S + 1
    s.expire()
    assert not s.by_code, "silent hosts expire"
    assert s.handle(b"junk", a) == [] and s.handle(hdr(99), a) == []
    assert s.handle(hdr(rdv.JOIN) + b"\x01", a) == [], "a short body is ignored"


def main():
    server_checks()
    print("Server: listing, codes, join replies, rate limit and expiry checks passed.")
    with tempfile.TemporaryDirectory(prefix="re1-rdv-") as d:
        libs = build(d)
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
        why = run_case(libs, "symmetric", Nat(ip(H), symmetric=True), Nat(ip(S), symmetric=True), expect_ok=False)
        assert why == "COULD NOT REACH THE HOST", why
        print("  %-34s fails as expected: %s" % ("both symmetric", why))
        why = run_case(libs, "bad code", Nat(ip(H)), Nat(ip(S)), code_offset=1, expect_ok=False)
        assert why == "NO GAME WITH THAT CODE", why
        print("  %-34s fails as expected: %s" % ("a wrong code", why))
    print("Rendezvous punch checks passed.")


if __name__ == "__main__":
    sys.exit(main())
