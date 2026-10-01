"""Reads the original default.xbe by virtual address, and disassembles game code.

Usage: python tools/re/xbe.py dis <address> [count]     disassemble count instructions (default 40)
       python tools/re/xbe.py dump <address> [length]   hex dump
"""

import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
XBE = ROOT / "extracted_iso" / "default.xbe"


class Xbe:
    def __init__(self, path=XBE):
        self.data = Path(path).read_bytes()
        base = struct.unpack_from("<I", self.data, 0x104)[0]
        count = struct.unpack_from("<I", self.data, 0x11C)[0]
        headers = struct.unpack_from("<I", self.data, 0x120)[0] - base
        self.sections = []
        for index in range(count):
            flags, address, size, raw, raw_size, name_address = struct.unpack_from("<6I", self.data, headers + index * 56)
            name_offset = name_address - base
            name = self.data[name_offset:self.data.index(b"\0", name_offset)].decode()
            self.sections.append((name, address, size, raw, raw_size))

    def read(self, address, length):
        for _, start, size, raw, raw_size in self.sections:
            if start <= address < start + size:
                offset = address - start
                chunk = self.data[raw + offset:raw + min(offset + length, raw_size)]
                return chunk + bytes(length - len(chunk))
        raise ValueError(f"{address:08X} is not in any section")

    def u32(self, address):
        return struct.unpack("<I", self.read(address, 4))[0]

    def cstring(self, address, limit=256):
        raw = self.read(address, limit)
        return raw[:raw.index(b"\0")].decode("latin-1") if b"\0" in raw else raw.decode("latin-1")

    def section_of(self, address):
        for name, start, size, _, _ in self.sections:
            if start <= address < start + size:
                return name
        return None


def disassemble(xbe, address, count):
    import capstone
    engine = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    code = xbe.read(address, count * 15)
    for index, instruction in enumerate(engine.disasm(code, address)):
        if index >= count:
            break
        print(f"{instruction.address:08X}  {instruction.bytes.hex():<20} {instruction.mnemonic} {instruction.op_str}")


def main(argv):
    if len(argv) < 3:
        sys.exit(__doc__)
    xbe = Xbe()
    address = int(argv[2], 16)
    if argv[1] == "dis":
        disassemble(xbe, address, int(argv[3]) if len(argv) > 3 else 40)
    elif argv[1] == "dump":
        length = int(argv[3], 0) if len(argv) > 3 else 64
        data = xbe.read(address, length)
        for offset in range(0, length, 16):
            row = data[offset:offset + 16]
            print(f"{address + offset:08X}  {row.hex(' '):<48} {''.join(chr(b) if 32 <= b < 127 else '.' for b in row)}")
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main(sys.argv)
