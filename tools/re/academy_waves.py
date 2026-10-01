"""Decodes the Academy wave tables of the Thule Moon Academy script (multi5) from default.xbe.

Usage: python tools/re/academy_waves.py [--json out.json] [--markdown out.md]

The script object (vtable 0x360220, 0x4D8 bytes) holds 30 wave entries of 0x20 bytes at +0x24 (wave 0 is empty).
One of four setup functions fills them, chosen by the number of players (g_mpPlayerCount, 1..4). Per wave entry:
  +0x00 group count
  +0x04 spawn list: byte pairs (unit index, spawn point index); a unit whose name is null ends a group
  +0x08 float delay before each group
  +0x0C sound/voice cue name per group (null = none)
  +0x10 path effect record per group (0x14 bytes: time range min/max, two values, index byte into the
        BonusPath/EndPath name table at 0x396A60; the script shows something along that path)
  +0x14 "wait until all enemies are dead" byte per group
  +0x18, +0x1C effect start/stop lists per group (null-separated)
A per-wave byte table (+0x3E4) is added to the Academy level (g_academyLevel) after each wave.
"""

import json
import struct
import sys
from pathlib import Path

import capstone
from capstone import x86

sys.path.insert(0, str(Path(__file__).resolve().parent))
from xbe import Xbe  # noqa: E402

SETUPS = {1: 0x17E72D, 2: 0x17EED6, 3: 0x17F67B, 4: 0x17FE20}
UNIT_NAMES = 0x3968F8
SPAWN_POINTS = 0x396938
GOTO_PATHS = 0x3969C0
MESSAGES = 0x396A60
WAVES = 30
ENTRY_BASE = 0x24


def setup_fields(xbe, address):
    """this-relative offset -> value for every `mov dword ptr [reg + off], imm` in a setup function."""
    engine = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    engine.detail = True
    fields = {}
    registers = {}  # register -> known constant (None when unknown)
    stack = []      # values pushed by `push`
    for instruction in engine.disasm(xbe.read(address, 0x1000), address):
        mnemonic = instruction.mnemonic
        operands = instruction.operands
        if mnemonic == "ret":
            break

        def value_of(operand):
            if operand.type == x86.X86_OP_IMM:
                return operand.imm & 0xFFFFFFFF
            if operand.type == x86.X86_OP_REG:
                return registers.get(operand.reg)
            return None

        if mnemonic == "push":
            stack.append(value_of(operands[0]))
        elif mnemonic == "pop" and operands[0].type == x86.X86_OP_REG:
            registers[operands[0].reg] = stack.pop() if stack else None
        elif mnemonic in ("xor", "sub") and len(operands) == 2 and operands[0].type == x86.X86_OP_REG                 and operands[1].type == x86.X86_OP_REG and operands[0].reg == operands[1].reg:
            registers[operands[0].reg] = 0
        elif mnemonic in ("inc", "dec") and operands[0].type == x86.X86_OP_REG:
            current = registers.get(operands[0].reg)
            registers[operands[0].reg] = None if current is None else (current + (1 if mnemonic == "inc" else -1)) & 0xFFFFFFFF
        elif mnemonic in ("add", "sub") and len(operands) == 2 and operands[0].type == x86.X86_OP_REG:
            current, delta = registers.get(operands[0].reg), value_of(operands[1])
            registers[operands[0].reg] = None if current is None or delta is None else                 (current + delta if mnemonic == "add" else current - delta) & 0xFFFFFFFF
        elif mnemonic == "mov" and len(operands) == 2:
            destination, source = operands
            if destination.type == x86.X86_OP_REG:
                registers[destination.reg] = value_of(source)
            elif destination.type == x86.X86_OP_MEM and destination.mem.index == 0 and value_of(source) is not None:
                fields[destination.mem.disp] = value_of(source)
        elif operands and operands[0].type == x86.X86_OP_REG:
            registers[operands[0].reg] = None  # any other write makes the register unknown
    return fields


def string_or_none(xbe, pointer):
    if pointer == 0:
        return None
    try:
        return xbe.cstring(pointer, 64)
    except ValueError:
        return None


def readable(xbe, address, length):
    try:
        return xbe.read(address, length)
    except ValueError:
        return bytes(length)  # uninitialized data: zero at load


def decode(xbe, players):
    fields = setup_fields(xbe, SETUPS[players])
    level_table = fields.get(0x3E4, 0)
    waves = []
    for wave in range(1, WAVES):
        entry = ENTRY_BASE + wave * 0x20
        count = fields.get(entry, 0)
        if count == 0:
            continue
        spawn_list = fields.get(entry + 0x04, 0)
        delays = fields.get(entry + 0x08, 0)
        sounds = fields.get(entry + 0x0C, 0)
        messages = fields.get(entry + 0x10, 0)
        waits = fields.get(entry + 0x14, 0)
        groups = []
        position = spawn_list
        for group in range(count):
            units = []
            while True:
                unit, spawn = struct.unpack("<bb", readable(xbe, position, 2))
                position += 2
                name = string_or_none(xbe, xbe.u32(UNIT_NAMES + unit * 4)) if unit >= 0 else None
                if name is None:
                    break
                units.append({"unit": name, "spawn": string_or_none(xbe, xbe.u32(SPAWN_POINTS + spawn * 4)),
                              "path": string_or_none(xbe, xbe.u32(GOTO_PATHS + spawn * 4))})
            delay = struct.unpack("<f", readable(xbe, delays + group * 4, 4))[0] if delays else 0.0
            sound = string_or_none(xbe, struct.unpack("<I", readable(xbe, sounds + group * 4, 4))[0]) if sounds else None
            record = readable(xbe, messages + group * 0x14, 0x14) if messages else bytes(0x14)
            path_index = record[0x10]
            message = string_or_none(xbe, xbe.u32(MESSAGES + path_index * 4)) if path_index else None
            wait = bool(readable(xbe, waits + group, 1)[0]) if waits else False
            groups.append({"delay": round(delay, 3), "wait_for_clear": wait, "sound": sound, "path_effect": message, "units": units})
        level_step = readable(xbe, level_table + wave - 1, 1)[0] if level_table else 0
        waves.append({"wave": wave, "level_step": level_step, "groups": groups})
    return waves


def markdown(all_waves):
    lines = ["# Thule Moon Academy waves", "", "Generated by `tools/re/academy_waves.py` from default.xbe.", ""]
    for players, waves in all_waves.items():
        lines += [f"## {players} player{'s' if players > 1 else ''}", ""]
        for wave in waves:
            lines.append(f"### Wave {wave['wave']} (level +{wave['level_step']})")
            for index, group in enumerate(wave["groups"]):
                units = {}
                for unit in group["units"]:
                    units[unit["unit"]] = units.get(unit["unit"], 0) + 1
                summary = ", ".join(f"{count}x {name}" for name, count in units.items()) or "(none)"
                extras = []
                if group["wait_for_clear"]:
                    extras.append("waits for clear")
                if group["sound"]:
                    extras.append(f"sound {group['sound']}")
                if group["path_effect"]:
                    extras.append(f"path effect {group['path_effect']}")
                lines.append(f"- group {index + 1}: after {group['delay']}s: {summary}" + (f" ({'; '.join(extras)})" if extras else ""))
            lines.append("")
    return "\n".join(lines)


def main(argv):
    xbe = Xbe()
    all_waves = {players: decode(xbe, players) for players in SETUPS}
    if "--json" in argv:
        Path(argv[argv.index("--json") + 1]).write_text(json.dumps(all_waves, indent=1))
    if "--markdown" in argv:
        Path(argv[argv.index("--markdown") + 1]).write_text(markdown(all_waves), encoding="utf-8")
    if "--json" not in argv and "--markdown" not in argv:
        print(markdown({1: all_waves[1]}))


if __name__ == "__main__":
    main(sys.argv)
