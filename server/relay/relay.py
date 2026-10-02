"""Academy relay: forwards Clone Wars virtual-network datagrams between the games in a room.

Games behind home routers cannot reach each other directly without port forwarding. Each game instead sends every
datagram to this relay (CW_NET_RELAY=host:port, CW_NET_ROOM=<join code>); the relay forwards it to the room member
with the destination virtual IP, or to every other member for a broadcast. It never looks at the game payload.

Datagrams (little-endian u32 fields, magic first; see native_port/runtime/HleNet.cpp):
  CWN2 build room srcIp dstIp srcPort:dstPort payload   game traffic (24-byte header)
  CWNK build room ip                                     keepalive: join/refresh membership (every 5 s)
  CWNQ room                                              query  -> CWNR room players build

A member is identified by (room, virtual IP) and expires after EXPIRE_SECONDS without traffic. A room holds games of a
single build (version + mod fingerprint): the first member sets it, and others are told "build mismatch" via the
query and never receive traffic.

Settings (environment): RELAY_PORT (3074), RELAY_EXPIRE (30), RELAY_MAX_ROOMS (1000), RELAY_MAX_MEMBERS (8),
RELAY_LOG (info|debug).
"""

import asyncio
import logging
import os
import struct
import time

MAGIC_GAME = 0x324E5743   # "CWN2"
MAGIC_KEEPALIVE = 0x4B4E5743  # "CWNK"
MAGIC_QUERY = 0x514E5743  # "CWNQ"
MAGIC_REPLY = 0x524E5743  # "CWNR"
BROADCAST = 0xFFFFFFFF

PORT = int(os.environ.get("RELAY_PORT", "3074"))
EXPIRE_SECONDS = float(os.environ.get("RELAY_EXPIRE", "30"))
MAX_ROOMS = int(os.environ.get("RELAY_MAX_ROOMS", "1000"))
MAX_MEMBERS = int(os.environ.get("RELAY_MAX_MEMBERS", "8"))

log = logging.getLogger("relay")


class Member:
    __slots__ = ("address", "seen")

    def __init__(self, address):
        self.address = address
        self.seen = time.monotonic()


class Room:
    __slots__ = ("build", "members")

    def __init__(self, build):
        self.build = build
        self.members = {}  # virtual IP -> Member


class Relay(asyncio.DatagramProtocol):
    def __init__(self):
        self.rooms = {}  # room id -> Room
        self.transport = None
        self.forwarded = 0

    def connection_made(self, transport):
        self.transport = transport

    def join(self, room_id, build, ip, address):
        """Adds or refreshes a member; returns its room, or None when refused."""
        room = self.rooms.get(room_id)
        if room is None:
            if len(self.rooms) >= MAX_ROOMS:
                return None
            room = self.rooms[room_id] = Room(build)
            log.info("room %08x opened (build %08x) by %s", room_id, build, address[0])
        if room.build != build:
            return None
        member = room.members.get(ip)
        if member is None:
            if len(room.members) >= MAX_MEMBERS:
                return None
            member = room.members[ip] = Member(address)
            log.info("room %08x: %s joined from %s:%d (%d in room)", room_id, ip_text(ip), *address, len(room.members))
        member.address = address
        member.seen = time.monotonic()
        return room

    def datagram_received(self, data, address):
        if len(data) < 8:
            return
        magic, = struct.unpack_from("<I", data)
        if magic == MAGIC_QUERY:
            room_id, = struct.unpack_from("<I", data, 4)
            room = self.rooms.get(room_id)
            players, build = (len(room.members), room.build) if room else (0, 0)
            self.transport.sendto(struct.pack("<IIII", MAGIC_REPLY, room_id, players, build), address)
        elif magic == MAGIC_KEEPALIVE and len(data) >= 16:
            build, room_id, ip = struct.unpack_from("<III", data, 4)
            self.join(room_id, build, ip, address)
        elif magic == MAGIC_GAME and len(data) >= 24:
            build, room_id, source, destination = struct.unpack_from("<IIII", data, 4)
            room = self.join(room_id, build, source, address)
            if room is None:
                return
            if destination == BROADCAST:
                for ip, member in room.members.items():
                    if ip != source:
                        self.transport.sendto(data, member.address)
                        self.forwarded += 1
            else:
                member = room.members.get(destination)
                if member is not None:
                    self.transport.sendto(data, member.address)
                    self.forwarded += 1

    def expire(self):
        now = time.monotonic()
        for room_id in list(self.rooms):
            room = self.rooms[room_id]
            for ip in [ip for ip, member in room.members.items() if now - member.seen > EXPIRE_SECONDS]:
                del room.members[ip]
                log.info("room %08x: %s left (%d in room)", room_id, ip_text(ip), len(room.members))
            if not room.members:
                del self.rooms[room_id]
                log.info("room %08x closed", room_id)


def ip_text(ip):
    return ".".join(str(b) for b in struct.pack("<I", ip))


async def main():
    logging.basicConfig(level=os.environ.get("RELAY_LOG", "info").upper(), format="%(asctime)s %(message)s")
    loop = asyncio.get_running_loop()
    transport, relay = await loop.create_datagram_endpoint(Relay, local_addr=("0.0.0.0", PORT))
    log.info("relay listening on UDP %d", PORT)
    try:
        while True:
            await asyncio.sleep(5)
            relay.expire()
            log.debug("%d rooms, %d datagrams forwarded", len(relay.rooms), relay.forwarded)
    finally:
        transport.close()


if __name__ == "__main__":
    asyncio.run(main())
