"""Reads and writes Clone Wars .zwp archives (data.zwp).

Format (little endian):
  header (0x38 bytes): "NORK", u32 version (2), u32 0, u32 archive size, u32 entry count,
                       u32 directory offset, then zero padding
  entry data:          packed back to back from 0x38; each entry is a zlib stream, or stored
                       raw when compression does not make it smaller (compressed size == size)
  directory:           per entry: u32 0, u8 name length, name (no terminator),
                       u32 data offset, u32 size, u32 stored size

Usage:
  python tools/zwp/zwp.py list    <archive> [substring]
  python tools/zwp/zwp.py extract <archive> <out-dir> [substring]
  python tools/zwp/zwp.py build   <base-archive> <override-dir> <out-archive>
  python tools/zwp/zwp.py reads   <archive> <cw_runtime.log>   (entries the game loaded, in order)

`build` keeps every entry of the base archive in its original order, replaces entries whose
name matches a file in override-dir (case-insensitive), and appends files that are new.
"""

import re
import struct
import sys
import zlib
from pathlib import Path

MAGIC = b"NORK"
VERSION = 2
HEADER_SIZE = 0x38


class Entry:
    def __init__(self, name, offset, size, stored_size):
        self.name = name
        self.offset = offset
        self.size = size
        self.stored_size = stored_size

    @property
    def compressed(self):
        return self.stored_size != self.size


def read_directory(data):
    magic, version, _, _, count, directory = struct.unpack_from("<4sIIIII", data, 0)
    if magic != MAGIC or version != VERSION:
        raise ValueError(f"not a version {VERSION} NORK archive")
    entries = []
    position = directory
    for _ in range(count):
        length = data[position + 4]
        name = data[position + 5:position + 5 + length].decode("latin-1")
        position += 5 + length
        offset, size, stored_size = struct.unpack_from("<III", data, position)
        position += 12
        entries.append(Entry(name, offset, size, stored_size))
    return entries


def read_entry(data, entry):
    raw = data[entry.offset:entry.offset + entry.stored_size]
    content = zlib.decompress(raw) if entry.compressed else raw
    if len(content) != entry.size:
        raise ValueError(f"{entry.name}: expected {entry.size} bytes, got {len(content)}")
    return content


def pack(content):
    """Returns (stored bytes, is_compressed) using the game's rule: compress only when smaller."""
    packed = zlib.compress(content, 9)
    return (packed, True) if len(packed) < len(content) else (content, False)


def write_archive(path, items):
    """items: list of (name, stored bytes, uncompressed size)."""
    body = bytearray()
    directory = bytearray()
    for name, stored, size in items:
        encoded = name.encode("latin-1")
        if len(encoded) > 255:
            raise ValueError(f"name too long: {name}")
        directory += struct.pack("<IB", 0, len(encoded)) + encoded
        directory += struct.pack("<III", HEADER_SIZE + len(body), size, len(stored))
        body += stored
    directory_offset = HEADER_SIZE + len(body)
    total = directory_offset + len(directory)
    header = struct.pack("<4sIIIII", MAGIC, VERSION, 0, total, len(items), directory_offset)
    header += bytes(HEADER_SIZE - len(header))
    Path(path).write_bytes(header + body + directory)


def build(base_path, override_dir, out_path):
    data = Path(base_path).read_bytes()
    overrides = {}
    for file in Path(override_dir).rglob("*"):
        if file.is_file():
            overrides[file.name.lower()] = file
    items = []
    replaced = 0
    for entry in read_directory(data):
        override = overrides.pop(entry.name.lower(), None)
        if override is None:
            # Unchanged entries are copied without recompressing.
            items.append((entry.name, data[entry.offset:entry.offset + entry.stored_size], entry.size))
        else:
            content = override.read_bytes()
            stored, _ = pack(content)
            items.append((entry.name, stored, len(content)))
            replaced += 1
    for name, file in sorted(overrides.items()):
        content = file.read_bytes()
        stored, _ = pack(content)
        items.append((file.name, stored, len(content)))
    write_archive(out_path, items)
    print(f"wrote {out_path}: {len(items)} entries, {replaced} replaced, {len(overrides)} added")


def main(argv):
    if len(argv) < 3:
        sys.exit(__doc__)
    command, archive = argv[1], argv[2]
    if command == "build":
        if len(argv) != 5:
            sys.exit(__doc__)
        build(archive, argv[3], argv[4])
        return
    data = Path(archive).read_bytes()
    entries = read_directory(data)
    if command == "reads":
        # The runtime logs every seek on the archive handle; entry offsets identify what was loaded.
        log = Path(argv[3]).read_text(errors="replace")
        match = re.search(r"NtCreateFile\('D:\\data\.zwp'\) -> 0x00000000 handle (\w+)", log)
        if match is None:
            sys.exit("the log has no data.zwp open with a handle")
        by_offset = {entry.offset: entry.name for entry in entries}
        for position in re.findall(rf"NtSetInformationFile\({match.group(1)}, position=(\d+)\)", log):
            name = by_offset.get(int(position))
            if name is not None:
                print(name)
        return
    if command == "list":
        needle = argv[3].lower() if len(argv) > 3 else ""
        for entry in entries:
            if needle in entry.name.lower():
                print(f"{entry.offset:10d} {entry.size:9d} {entry.stored_size:9d} {entry.name}")
    elif command == "extract":
        out = Path(argv[3])
        needle = argv[4].lower() if len(argv) > 4 else ""
        out.mkdir(parents=True, exist_ok=True)
        count = 0
        for entry in entries:
            if needle in entry.name.lower():
                (out / entry.name).write_bytes(read_entry(data, entry))
                count += 1
        print(f"extracted {count} files to {out}")
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main(sys.argv)
