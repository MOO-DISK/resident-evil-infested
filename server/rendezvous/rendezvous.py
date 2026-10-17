#!/usr/bin/env python3
"""Infested rendezvous server - introduces players, never carries a match.

A host registers from its game socket and gets a six-digit game code; a
survivor asks for the list of public games or joins by code. The server
tells each side the other's public (as seen here) and LAN address; the two
then punch through their own NATs and talk directly. Game traffic never
comes through here.

One UDP port, standard library only. The wire format is
src/game/mods/ZombieRendezvous.h (all little-endian):

    header   u32 magic "RE1R", u8 protocol, u8 type, u16 reserved
    address  u32 ip (host order value: 192.168.1.10 = 0xC0A8010A), u16 port, u16 reserved

Run:  python3 rendezvous.py [--port 27961] [--bind 0.0.0.0]
"""
import argparse
import asyncio
import logging
import random
import struct
import time

MAGIC = 0x52314552            # "RE1R"
PROTOCOL = 1

REGISTER, REGISTERED, LIST_REQ, LIST, JOIN, PEER, INTRO, JOIN_FAIL = 1, 2, 3, 4, 5, 6, 7, 8
PUNCH, PUNCH_ACK, PROBE, PROBE_REPLY, UNREGISTER = 9, 10, 11, 12, 13

FLAG_PUBLIC, FLAG_STARTED, FLAG_AI = 1, 2, 4
FAIL_NOT_FOUND, FAIL_VERSION, FAIL_FULL, FAIL_BUSY = 1, 2, 3, 4

HEADER = struct.Struct("<IBBH")
ADDR = struct.Struct("<IHH")
REGISTER_BODY = struct.Struct("<IBBBB")          # token, game version, flags, survivors, capacity (+ addr)
JOIN_BODY = struct.Struct("<IB3x")                # code, game version (+ addr)
ENTRY = struct.Struct("<IBBBB")                   # code, survivors, capacity, flags, game version

HOST_EXPIRE_S = 30.0          # a host heartbeats every 5 s
LIST_MAX = 64                 # fits one 1200-byte datagram
JOIN_LIMIT = 20               # JOINs per source IP per minute
REGISTER_LIMIT = 8            # new codes per source IP per minute

log = logging.getLogger("rendezvous")


def ip_to_int(ip):
    a, b, c, d = (int(x) for x in ip.split("."))
    return (a << 24) | (b << 16) | (c << 8) | d


def int_to_ip(v):
    return "%d.%d.%d.%d" % ((v >> 24) & 255, (v >> 16) & 255, (v >> 8) & 255, v & 255)


def pack_addr(ip_int, port):
    return ADDR.pack(ip_int & 0xFFFFFFFF, port & 0xFFFF, 0)


class Host:
    __slots__ = ("token", "code", "public", "local", "version", "flags", "survivors",
                 "capacity", "seen", "created")


class RateLimit:
    def __init__(self, per_minute):
        self.per_minute = per_minute
        self.hits = {}

    def allow(self, ip, now):
        stamps = [t for t in self.hits.get(ip, ()) if now - t < 60.0]
        if len(stamps) >= self.per_minute:
            self.hits[ip] = stamps
            return False
        stamps.append(now)
        self.hits[ip] = stamps
        return True

    def prune(self, now):
        for ip in [ip for ip, s in self.hits.items() if not s or now - s[-1] >= 60.0]:
            del self.hits[ip]


class Rendezvous:
    """The protocol, transport-free: handle() returns the datagrams to send."""

    def __init__(self, clock=time.monotonic, rng=None):
        self.clock = clock
        self.rng = rng or random.SystemRandom()
        self.by_token = {}
        self.by_code = {}
        self.join_limit = RateLimit(JOIN_LIMIT)
        self.register_limit = RateLimit(REGISTER_LIMIT)

    # -- bookkeeping --------------------------------------------------------
    def expire(self):
        now = self.clock()
        for h in [h for h in self.by_token.values() if now - h.seen > HOST_EXPIRE_S]:
            self.drop(h, "expired")
        self.join_limit.prune(now)
        self.register_limit.prune(now)

    def drop(self, host, why):
        self.by_token.pop(host.token, None)
        self.by_code.pop(host.code, None)
        log.info("game %06d %s", host.code, why)

    def new_code(self):
        for _ in range(1000):
            code = self.rng.randrange(100000, 1000000)
            if code not in self.by_code:
                return code
        raise RuntimeError("no free game codes")

    # -- packets ------------------------------------------------------------
    def handle(self, data, src):
        """data: bytes from src (ip str, port). Returns [(bytes, (ip, port))]."""
        if len(data) < HEADER.size:
            return []
        magic, proto, kind, _ = HEADER.unpack_from(data)
        if magic != MAGIC or proto != PROTOCOL:
            return []
        body = data[HEADER.size:]
        handler = {
            REGISTER: self.on_register, UNREGISTER: self.on_unregister,
            LIST_REQ: self.on_list, JOIN: self.on_join, PROBE: self.on_probe,
        }.get(kind)
        if handler is None:
            return []
        try:
            return handler(body, src)
        except struct.error:
            return []

    def reply(self, kind, body):
        return HEADER.pack(MAGIC, PROTOCOL, kind, 0) + body

    def on_register(self, body, src):
        token, version, flags, survivors, capacity = REGISTER_BODY.unpack_from(body)
        local_ip, local_port, _ = ADDR.unpack_from(body, REGISTER_BODY.size)
        now = self.clock()
        public = (ip_to_int(src[0]), src[1])
        h = self.by_token.get(token)
        if h is None:
            if not token or not self.register_limit.allow(src[0], now):
                return []
            h = Host()
            h.token, h.code, h.created = token, self.new_code(), now
            self.by_token[token] = h
            self.by_code[h.code] = h
            log.info("game %06d registered by %s:%d (version %d)", h.code, src[0], src[1], version)
        elif h.public != public:
            log.info("game %06d moved to %s:%d", h.code, src[0], src[1])
        h.public, h.local = public, (local_ip, local_port)
        h.version, h.flags = version, flags
        h.survivors, h.capacity, h.seen = survivors, capacity, now
        return [(self.reply(REGISTERED, struct.pack("<I", h.code) + pack_addr(*public)), src)]

    def on_unregister(self, body, src):
        (token,) = struct.unpack_from("<I", body)
        h = self.by_token.get(token)
        if h is not None and h.public == (ip_to_int(src[0]), src[1]):
            self.drop(h, "closed")
        return []

    def on_list(self, body, src):
        (version,) = struct.unpack_from("<B", body)
        self.expire()
        games = [h for h in self.by_token.values()
                 if h.version == version and h.flags & FLAG_PUBLIC
                 and not h.flags & FLAG_STARTED and h.survivors < h.capacity]
        games.sort(key=lambda h: h.created)
        games = games[:LIST_MAX]
        out = struct.pack("<B3x", len(games))
        for h in games:
            out += ENTRY.pack(h.code, h.survivors, h.capacity, h.flags, h.version)
        return [(self.reply(LIST, out), src)]

    def on_join(self, body, src):
        code, version = JOIN_BODY.unpack_from(body)
        local_ip, local_port, _ = ADDR.unpack_from(body, JOIN_BODY.size)
        now = self.clock()
        if not self.join_limit.allow(src[0], now):
            return [(self.reply(JOIN_FAIL, struct.pack("<IB3x", code, FAIL_BUSY)), src)]
        self.expire()
        h = self.by_code.get(code)
        if h is None:
            return [(self.reply(JOIN_FAIL, struct.pack("<IB3x", code, FAIL_NOT_FOUND)), src)]
        if h.version != version:
            return [(self.reply(JOIN_FAIL, struct.pack("<IB3x", code, FAIL_VERSION)), src)]
        # A started match still takes a JOIN: a crashed survivor rejoins by
        # code, and the host decides whether its HELLO reclaims a seat.
        if not h.flags & FLAG_STARTED and h.survivors >= h.capacity:
            return [(self.reply(JOIN_FAIL, struct.pack("<IB3x", code, FAIL_FULL)), src)]
        client_public = (ip_to_int(src[0]), src[1])
        log.info("game %06d: introducing %s:%d", code, src[0], src[1])
        peer = struct.pack("<I", code) + pack_addr(*h.public) + pack_addr(*h.local)
        intro = struct.pack("<I", code) + pack_addr(*client_public) + pack_addr(local_ip, local_port)
        host_addr = (int_to_ip(h.public[0]), h.public[1])
        return [(self.reply(PEER, peer), src), (self.reply(INTRO, intro), host_addr)]

    def on_probe(self, body, src):
        (nonce,) = struct.unpack_from("<I", body)
        return [(self.reply(PROBE_REPLY, struct.pack("<I", nonce) +
                            pack_addr(ip_to_int(src[0]), src[1])), src)]


class Protocol(asyncio.DatagramProtocol):
    def __init__(self, server):
        self.server = server
        self.transport = None

    def connection_made(self, transport):
        self.transport = transport

    def datagram_received(self, data, addr):
        for out, dst in self.server.handle(data, addr[:2]):
            self.transport.sendto(out, dst)

    def error_received(self, exc):
        log.debug("socket error: %s", exc)


async def serve(bind, port):
    loop = asyncio.get_running_loop()
    server = Rendezvous()
    transport, _ = await loop.create_datagram_endpoint(lambda: Protocol(server), local_addr=(bind, port))
    log.info("listening on udp %s:%d", bind, port)
    try:
        while True:
            await asyncio.sleep(10)
            server.expire()
    finally:
        transport.close()


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--bind", default="0.0.0.0")
    ap.add_argument("--port", type=int, default=27961)
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args()
    logging.basicConfig(level=logging.DEBUG if args.verbose else logging.INFO,
                        format="%(asctime)s %(message)s")
    asyncio.run(serve(args.bind, args.port))


if __name__ == "__main__":
    main()
