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
  +0x14 "don't wait" byte per group: 0 = before the next group, wait until every enemy is dead
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
            keep_going = bool(readable(xbe, waits + group, 1)[0]) if waits else False
            groups.append({"delay": round(delay, 3), "waits_for_clear": not keep_going, "sound": sound, "path_effect": message, "units": units})
        level_step = readable(xbe, level_table + wave - 1, 1)[0] if level_table else 0
        waves.append({"wave": wave, "level_step": level_step, "groups": groups})
    return waves


INTRO = """# Thule Moon Academy waves

Generated by `tools/re/academy_waves.py` from default.xbe; the tables below are the game's own data. The engine
code is `ThuleAcademyScript` (see `symbols/manual.csv` and `native_port/runtime/game/GameObjects.h`).

## How waves work

1. **Pick a wave set.** When the match starts, the script reads the number of players (1-4) and loads one of four
   wave sets. Each set has 26 waves.
2. **Run a wave, one group at a time.** A wave is a list of *groups*. Running a group means:
   - spawn its units (each one an ODF at a named spawn point, sent along that spawn point's path towards the base),
   - play its voice-over line, if it has one (`MWM26_xxx` files in `Data/Sounds/VO`, the Academy instructor),
   - start its bonus-path effect, if it has one (`BonusPath*` / `EndPath*` paths in the map; bonus waves use these).
3. **Wait before the next group.** Unless the group is marked *keeps going*, the script first waits until **every
   enemy is dead**. Then it waits the group's **delay** (seconds) and runs the next group. So a group's delay is the
   pause *after* it, and the last group's delay is the pause before the next wave.
4. **Finish the wave.** After its last group, the wave number goes up and the **Academy level** goes up by the wave's
   level step. Level only matters for enemy toughness: every unit spawns with its health and ammo multiplied by
   `1 + 0.5 * floor((level - 1) / 20)` (x1 for levels 1-20, x1.5 for 21-40, ...).
5. **Loop.** After wave 26 the script starts again at wave 1 with the level it reached, so later loops are tougher.

Special waves:
- **Bonus waves** (level step 0, every 4th wave): no enemies, a sequence of bonus-path effects instead.
- **Gladiator** (2+ players, the bonus waves): a `GLADIATOR` entry ends co-op; players are put on separate teams and
  fight each other (the "elimination" round).
- **Wave 26** is the finale: a voice line, then an `EndPath` effect sequence, then a 20 s pause before the loop.

### Reading a wave table

| Column | Meaning |
|---|---|
| Group | order within the wave |
| Spawns | units created when the group runs (ODF names, so they can be looked up in `data.zwp`) |
| Then | what happens before the next group: *clear* = wait until all enemies are dead, then the delay |
| Also | voice-over line or bonus-path effect started with the group |

For example, wave 2 for one player:

| Group | Spawns | Then | Also |
|---|---|---|---|
| 1 | - | clear, 2 s | voice MWM26_075 (wave intro) |
| 2 | 2x cis_bike_speeder | **0.5 s, keeps going** | |
| 3 | 2x cis_bike_speeder | clear, 1 s | |
| 4 | 2x CIS_tank_fighter | clear, 1 s | bonus path BonusPath2_4 |
| 5 | 2x cis_bike_speeder, 2x CIS_tank_fighter | clear, 2 s | |
| 6 | - | clear, 3 s | voice MWM26_096 (wave outro) |

The intro line plays, two bikes arrive and two more follow half a second later without waiting. Once all four
are destroyed two fighter tanks come in, then a mixed group; after the last kill the outro plays and wave 3 starts
3 s later.

Change waves with a plugin (the tables are in the script object; `ThuleAcademyScript::wave()` in GameObjects.h).
"""


def describe_units(units):
    counts = {}
    for unit in units:
        counts[unit["unit"]] = counts.get(unit["unit"], 0) + 1
    return ", ".join(f"{count}x {name}" for name, count in counts.items())


def wave_kind(wave):
    names = [unit["unit"] for group in wave["groups"] for unit in group["units"]]
    if "GLADIATOR" in names:
        return "gladiator"
    if not [name for name in names if name]:
        effects = [group["path_effect"] or "" for group in wave["groups"]]
        return "finale" if any(effect.startswith("EndPath") for effect in effects) else "bonus"
    return "boss" if any("boss" in name.lower() for name in names) else "enemies"


def markdown(all_waves):
    lines = [INTRO]
    for players, waves in all_waves.items():
        lines += [f"## {players} player{'s' if players > 1 else ''}", "", "| Wave | Kind | Enemies | Level step |", "|---|---|---|---|"]
        for wave in waves:
            enemies = sum(1 for group in wave["groups"] for unit in group["units"] if unit["unit"] != "GLADIATOR")
            lines.append(f"| [{wave['wave']}](#{players}p-wave-{wave['wave']}) | {wave_kind(wave)} | {enemies} | +{wave['level_step']} |")
        lines.append("")
        for wave in waves:
            lines += [f'<a id="{players}p-wave-{wave["wave"]}"></a>', f"### {players}P wave {wave['wave']}", "",
                      "| Group | Spawns | Then | Also |", "|---|---|---|---|"]
            for index, group in enumerate(wave["groups"]):
                then = f"clear, {group['delay']:g} s" if group["waits_for_clear"] else f"**{group['delay']:g} s, keeps going**"
                also = []
                if group["sound"]:
                    also.append(f"voice {group['sound']}")
                if group["path_effect"]:
                    also.append(f"bonus path {group['path_effect']}")
                lines.append(f"| {index + 1} | {describe_units(group['units']) or '-'} | {then} | {'; '.join(also)} |")
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
