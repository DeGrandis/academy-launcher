"""Checks a running relay: python test_relay.py [host] [port]

Two fake games join a room; a broadcast and a directed datagram must arrive at the other one, a game of another build
must get nothing, and a query must report the room.
"""

import socket
import struct
import sys
import time

GAME, KEEPALIVE, QUERY, REPLY = 0x324E5743, 0x4B4E5743, 0x514E5743, 0x524E5743
host = sys.argv[1] if len(sys.argv) > 1 else "127.0.0.1"
port = int(sys.argv[2]) if len(sys.argv) > 2 else 3074
relay = (host, port)
ROOM, BUILD = 0x1234ABCD, 0xB01DFACE
A, B, C = 0x0100000A, 0x0200000A, 0x0300000A  # 10.0.0.1, 10.0.0.2, 10.0.0.3 as the game stores them


def game():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.bind(("0.0.0.0", 0))
    s.settimeout(1.0)
    return s


def datagram(build, source, destination, payload):
    return struct.pack("<IIIIIHH", GAME, build, ROOM, source, destination, 1000, 1000) + payload


def receive(s):
    try:
        return s.recv(2048)
    except socket.timeout:
        return None


a, b, c = game(), game(), game()
a.sendto(struct.pack("<IIII", KEEPALIVE, BUILD, ROOM, A), relay)
b.sendto(struct.pack("<IIII", KEEPALIVE, BUILD, ROOM, B), relay)
c.sendto(struct.pack("<IIII", KEEPALIVE, 0xDEADBEEF, ROOM, C), relay)  # another build: refused
time.sleep(0.3)

a.sendto(datagram(BUILD, A, 0xFFFFFFFF, b"discovery"), relay)
got = receive(b)
assert got is not None and got.endswith(b"discovery"), f"broadcast not forwarded: {got!r}"
b.sendto(datagram(BUILD, B, A, b"hello"), relay)
got = receive(a)
assert got is not None and got.endswith(b"hello"), f"directed datagram not forwarded: {got!r}"
assert receive(c) is None, "a game of another build received traffic"

q = game()
q.sendto(struct.pack("<II", QUERY, ROOM), relay)
magic, room, players, build = struct.unpack("<IIII", receive(q))
assert (magic, room, players, build) == (REPLY, ROOM, 2, BUILD), (hex(magic), room, players, hex(build))
q.sendto(struct.pack("<II", QUERY, 42), relay)
assert struct.unpack("<IIII", receive(q))[2] == 0, "an unknown room reported players"
print("relay OK")
