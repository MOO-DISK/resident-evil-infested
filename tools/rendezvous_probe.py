#!/usr/bin/env python3
"""Check that a rendezvous server can be used for NAT hole punching.

Sends PROBE packets from one UDP socket and checks that:
  1. a reply comes back at all (the UDP port is open), and
  2. it comes back from the exact IP:port it was sent to. Port-restricted
     NATs drop replies from anywhere else, so a host whose outbound address
     differs from its inbound one (possible behind some cloud load balancers)
     cannot introduce players.
Also prints the public address the server saw for this machine.

    python3 tools/rendezvous_probe.py re1-infested.eastus.azurecontainer.io [27961]
"""
import random
import socket
import struct
import sys

MAGIC, PROTOCOL, PROBE, PROBE_REPLY = 0x52314552, 1, 11, 12


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    host = sys.argv[1]
    port = int(sys.argv[2]) if len(sys.argv) > 2 else 27961
    target = (socket.gethostbyname(host), port)
    print("server %s -> %s:%d" % (host, target[0], target[1]))
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.settimeout(1.0)
    nonce = random.getrandbits(32)
    for attempt in range(5):
        s.sendto(struct.pack("<IBBHI", MAGIC, PROTOCOL, PROBE, 0, nonce), target)
        try:
            data, src = s.recvfrom(2048)
        except socket.timeout:
            print("  attempt %d: no reply" % (attempt + 1))
            continue
        if len(data) < 18:
            print("  short reply from %s:%d" % src)
            continue
        magic, proto, kind, _, got, ip, pport, _ = struct.unpack_from("<IBBHIIHH", data)
        if magic != MAGIC or kind != PROBE_REPLY or got != nonce:
            print("  unexpected reply from %s:%d" % src)
            continue
        seen = "%d.%d.%d.%d:%d" % (ip >> 24, (ip >> 16) & 255, (ip >> 8) & 255, ip & 255, pport)
        print("  reply from %s:%d, your public address %s" % (src[0], src[1], seen))
        if src != target:
            print("FAIL: the reply came from a different address than the one probed;")
            print("      hole punching will not work through this server.")
            return 1
        print("OK: replies come from the probed address; the server is usable.")
        return 0
    print("FAIL: no reply - is the UDP port open and the server running?")
    return 1


if __name__ == "__main__":
    sys.exit(main())
