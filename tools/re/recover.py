"""Automated symbol recovery passes over default.xbe.

Usage: python tools/re/recover.py

Writes symbols/auto/*.csv (regenerated each run; curated names go in symbols/*.csv):
  hash_globals.csv  globals holding a hash of a name, set by static initializers (ODF section/key names)
  factories.csv     class factories: ODF classLabel -> prototype object, constructor, vtable chain
  vtables.csv       vtables (stored by constructors), their length and the functions that store them
  odf_fields.csv    ODF reads: section, key, scanf format, destination offset, reading function
  functions.csv     generated function names (constructors, vtable slots, ODF loaders)
  globals.csv       generated global names

Relies on analysis_exports/default_xbe/functions.csv for function bounds.
"""

import csv
import re
import sys
from collections import defaultdict
from pathlib import Path

import capstone
from capstone import x86

sys.path.insert(0, str(Path(__file__).resolve().parent))
from xbe import Xbe  # noqa: E402

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / "symbols" / "auto"
TEXT_START, TEXT_END = 0x11000, 0x2A2E00
CODE_END = 0x334440  # .text plus the Xbox library sections
RDATA_START, RDATA_END = 0x334440, 0x38A740
DATA_START, DATA_END = 0x38A740, 0x636AD0

HASH_FUNCTION = 0x22C820
ODF_READ = 0x22BE70
MAX_FUNCTION_BYTES = 0x20000


def load_functions():
    functions = []
    with open(ROOT / "analysis_exports" / "default_xbe" / "functions.csv", newline="") as handle:
        for row in csv.DictReader(handle):
            entry = int(row["entry"], 16)
            end = int(row["body_max"], 16)
            if TEXT_START <= entry < TEXT_END and end - entry < MAX_FUNCTION_BYTES:
                functions.append((entry, end + 1, row["name"]))
    functions.sort()
    return functions


def identifier(text):
    return re.sub(r"\W", "_", text.strip("[] ")).strip("_")


class Recovery:
    def __init__(self):
        self.xbe = Xbe()
        self.functions = load_functions()
        self.engine = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
        self.engine.detail = True
        self.instructions = {}

    def disassemble(self, start, end):
        key = (start, end)
        if key not in self.instructions:
            self.instructions[key] = list(self.engine.disasm(self.xbe.read(start, end - start), start))
        return self.instructions[key]

    def sweep(self):
        """Linear disassembly of all of .text as (start, end) chunks split at padding; static initializers are
        not in Ghidra's function list."""
        if not hasattr(self, "_sweep"):
            self._sweep = []
            data = self.xbe.read(TEXT_START, TEXT_END - TEXT_START)
            position = 0
            while position < len(data):
                chunk = []
                for instruction in self.engine.disasm(data[position:position + 0x4000], TEXT_START + position):
                    chunk.append(instruction)
                if not chunk:
                    position += 1
                    continue
                self._sweep.append(chunk)
                position = chunk[-1].address + chunk[-1].size - TEXT_START
        return self._sweep

    def function_containing(self, address):
        low, high = 0, len(self.functions) - 1
        while low <= high:
            middle = (low + high) // 2
            start, end, _ = self.functions[middle]
            if address < start:
                high = middle - 1
            elif address >= end:
                low = middle + 1
            else:
                return start
        return None

    def string_at(self, address):
        if not RDATA_START <= address < DATA_END:
            return None
        try:
            text = self.xbe.cstring(address, 128)
        except ValueError:
            return None
        return text if text and all(32 <= ord(c) < 127 for c in text) else None

    def is_code(self, address):
        return 0x11000 <= address < CODE_END

    def vtable_length(self, address):
        if not RDATA_START <= address < RDATA_END:
            return 0
        length = 0
        while length < 512 and self.is_code(self.xbe.u32(address + length * 4)):
            length += 1
        return length

    @staticmethod
    def immediate(operand):
        return operand.imm & 0xFFFFFFFF if operand.type == x86.X86_OP_IMM else None

    # --- passes ------------------------------------------------------------------

    def hash_globals(self):
        """mov eax, name / strlen loop / push 0 / push len / push name / call hash / mov [global], eax"""
        found = {}
        for code in self.sweep():
            for index, instruction in enumerate(code):
                if instruction.mnemonic != "call" or self.immediate(instruction.operands[0]) != HASH_FUNCTION:
                    continue
                name = None
                for previous in reversed(code[max(0, index - 4):index]):
                    if previous.mnemonic == "push" and self.immediate(previous.operands[0]) is not None:
                        name = self.string_at(self.immediate(previous.operands[0]))
                        if name:
                            break
                for following in code[index + 1:index + 4]:
                    if following.mnemonic == "mov" and following.operands[0].type == x86.X86_OP_MEM \
                            and following.operands[0].mem.base == 0 and following.operands[1].type == x86.X86_OP_REG:
                        target = following.operands[0].mem.disp & 0xFFFFFFFF
                        if name and DATA_START <= target < DATA_END:
                            found[target] = name
                        break
        return found

    def vtable_stores(self):
        """Every `mov dword ptr [reg], vtable` with a plausible vtable, keyed by vtable."""
        stores = defaultdict(set)
        for start, end, _ in self.functions:
            for instruction in self.disassemble(start, end):
                if instruction.mnemonic != "mov" or len(instruction.operands) != 2:
                    continue
                destination, source = instruction.operands
                if destination.type != x86.X86_OP_MEM or destination.mem.base == 0 or destination.mem.disp != 0 \
                        or destination.mem.index != 0 or source.type != x86.X86_OP_IMM or destination.size != 4:
                    continue
                value = source.imm & 0xFFFFFFFF
                if self.vtable_length(value) >= 2:
                    stores[value].add(start)
        return stores

    def last_vtable_store(self, constructor):
        """The final vtable a constructor installs (its own class)."""
        end = next((e for s, e, _ in self.functions if s == constructor), constructor + 0x2000)
        vtable = None
        base_constructor = None
        for instruction in self.disassemble(constructor, end):
            if instruction.mnemonic == "call" and base_constructor is None and vtable is None:
                base_constructor = self.immediate(instruction.operands[0])
            if instruction.mnemonic == "mov" and len(instruction.operands) == 2:
                destination, source = instruction.operands
                if destination.type == x86.X86_OP_MEM and destination.mem.disp == 0 and destination.mem.base != 0 \
                        and source.type == x86.X86_OP_IMM and self.vtable_length(source.imm & 0xFFFFFFFF) >= 2:
                    vtable = source.imm & 0xFFFFFFFF
            if instruction.mnemonic == "ret":
                break
        return vtable, base_constructor

    def class_name_near(self, label_address):
        """Strings of one source file are pooled together: the class name follows the label."""
        address = label_address
        for _ in range(8):
            text = self.string_at(address)
            if text is None:
                address += 4
                continue
            if text.endswith("Class") and text[0].isupper():
                return text
            address = (address + len(text) + 4) & ~3
        return None

    def factories(self):
        """push label / mov ecx, prototype / call constructor"""
        result = []
        for code in self.sweep():
            for index in range(len(code) - 2):
                first, second, third = code[index:index + 3]
                if first.mnemonic != "push" or second.mnemonic != "mov" or third.mnemonic != "call":
                    continue
                label_address = self.immediate(first.operands[0])
                if label_address is None or second.operands[0].type != x86.X86_OP_REG \
                        or second.operands[0].reg != x86.X86_REG_ECX or second.operands[1].type != x86.X86_OP_IMM:
                    continue
                label = self.string_at(label_address)
                prototype = second.operands[1].imm & 0xFFFFFFFF
                constructor = self.immediate(third.operands[0])
                if not label or not DATA_START <= prototype < DATA_END or constructor is None \
                        or not TEXT_START <= constructor < TEXT_END or not re.fullmatch(r"[A-Za-z_][\w ]*", label):
                    continue
                chain = []
                current = constructor
                for _ in range(12):
                    vtable, base = self.last_vtable_store(current)
                    if vtable is None:
                        break
                    chain.append((current, vtable))
                    if base is None or not TEXT_START <= base < TEXT_END:
                        break
                    current = base
                result.append({
                    "label": label,
                    "class": self.class_name_near(label_address) or "",
                    "prototype": prototype,
                    "constructor": constructor,
                    "chain": chain,
                })
        return result

    def odf_fields(self):
        """call odf_read(file, "[Section]", "key", " = fmt", &dest...) with dest = reg + offset."""
        rows = []
        for start, end, _ in self.functions:
            code = self.disassemble(start, end)
            for index, instruction in enumerate(code):
                if instruction.mnemonic != "call" or self.immediate(instruction.operands[0]) != ODF_READ:
                    continue
                pushes = []
                for previous in reversed(code[max(0, index - 24):index]):
                    if previous.mnemonic == "call":
                        break
                    if previous.mnemonic == "push":
                        pushes.append(previous)
                # pushes[0] is the first argument (pushed last).
                if len(pushes) < 5:
                    continue
                section = self.string_at(self.immediate(pushes[1].operands[0]) or 0)
                key = self.string_at(self.immediate(pushes[2].operands[0]) or 0)
                fmt = self.string_at(self.immediate(pushes[3].operands[0]) or 0)
                if not section or not key or fmt is None:
                    continue
                offset = self.destination_offset(code, pushes[4])
                rows.append((start, section, key, fmt.strip(), offset, instruction.address))
        return rows

    @staticmethod
    def destination_offset(code, push):
        operand = push.operands[0]
        if operand.type != x86.X86_OP_REG:
            return None
        register = operand.reg
        position = code.index(push)
        for previous in reversed(code[max(0, position - 8):position]):
            if previous.mnemonic in ("add", "lea", "mov") and previous.operands and \
                    previous.operands[0].type == x86.X86_OP_REG and previous.operands[0].reg == register:
                if previous.mnemonic == "add" and previous.operands[1].type == x86.X86_OP_IMM:
                    return previous.operands[1].imm
                if previous.mnemonic == "lea" and previous.operands[1].mem.base not in (x86.X86_REG_ESP, x86.X86_REG_EBP):
                    return previous.operands[1].mem.disp
                return None
        return None


def write_csv(name, header, rows):
    OUT.mkdir(parents=True, exist_ok=True)
    with open(OUT / name, "w", newline="") as handle:
        writer = csv.writer(handle, lineterminator="\n")
        writer.writerow(header)
        writer.writerows(rows)
    print(f"{name}: {len(rows)} rows")


def main():
    recovery = Recovery()
    function_names = {}
    global_names = {}

    hashes = recovery.hash_globals()
    write_csv("hash_globals.csv", ["address", "name"], [(f"{a:08X}", n) for a, n in sorted(hashes.items())])
    for address, name in hashes.items():
        global_names[address] = (f"g_hash_{identifier(name)}", f"hash of \"{name}\"")

    factories = recovery.factories()
    class_names = sorted({name for name in hashes.values() if name.endswith("Class") and name[0].isupper()})

    game_object_constructor = 0x1F6C0  # GameObjectClass::GameObjectClass

    def is_game_object(factory):
        return any(constructor == game_object_constructor for constructor, _ in factory["chain"])

    def class_for_label(factory):
        if not is_game_object(factory):
            # Other factories (AI processes, controllers, missions, movies) are named by their label.
            return f"Factory_{identifier(factory['label'])}" if factory["chain"] else ""
        if factory["class"]:
            return factory["class"]
        label = factory["label"].lower().replace("cw_", "")
        matches = [name for name in class_names if name.lower() == label + "class"]
        return matches[0] if matches else ""

    # Each constructor installs exactly one class's vtable; name constructors from the factories that use them.
    constructor_class = {}
    for factory in factories:
        name = class_for_label(factory)
        if name and factory["chain"]:
            constructor_class.setdefault(factory["chain"][0][0], name)
    constructor_vtable = {}
    parent_vtable = {}
    for factory in factories:
        chain = factory["chain"]
        for index, (constructor, vtable) in enumerate(chain):
            constructor_vtable[constructor] = vtable
            if index + 1 < len(chain):
                parent_vtable[vtable] = chain[index + 1][1]
    vtable_class = {vtable: constructor_class[c] for c, vtable in constructor_vtable.items() if c in constructor_class}
    for constructor, vtable in constructor_vtable.items():
        vtable_class.setdefault(vtable, f"Class_{vtable:08X}")
        name = vtable_class[vtable]
        function_names.setdefault(constructor, (f"{name}::{name}", "constructor"))

    rows = []
    for factory in factories:
        chain = " ".join(f"{c:08X}:{v:08X}" for c, v in factory["chain"])
        class_name = class_for_label(factory)
        rows.append((factory["label"], class_name, f"{factory['prototype']:08X}", f"{factory['constructor']:08X}", chain))
        label = identifier(factory["label"])
        global_names.setdefault(factory["prototype"], (f"g_prototype_{label}", f"{class_name} prototype for classLabel \"{factory['label']}\""))
    write_csv("factories.csv", ["label", "class", "prototype", "constructor", "constructor_vtable_chain"], rows)

    # Zero-engine types return their name hash from a tiny virtual (`mov eax, [g_hash_Name]; ret`), usually slot 2.
    stores = recovery.vtable_stores()
    type_slot = {}
    for vtable in stores:
        if vtable in vtable_class:
            continue
        for slot in range(min(8, recovery.vtable_length(vtable))):
            target = recovery.xbe.u32(vtable + slot * 4)
            code = recovery.xbe.read(target, 6)
            if code[0] == 0xA1 and code[5] == 0xC3:
                hashed = int.from_bytes(code[1:5], "little")
                if hashed in hashes:
                    vtable_class[vtable] = identifier(hashes[hashed])
                    type_slot[vtable] = (slot, target)
                    break
    # Subclasses that do not override the type-hash virtual share it; the one that defines it keeps the plain name.
    sharing = defaultdict(list)
    for vtable, (slot, target) in type_slot.items():
        sharing[target].append(vtable)
    for target, vtables in sharing.items():
        name = vtable_class[vtables[0]]
        function_names.setdefault(target, (f"{name}::GetTypeHash", "returns the type name hash"))
        if len(vtables) > 1:
            # The defining class's vtable is the one with the fewest slots that the others extend.
            vtables.sort(key=lambda v: (recovery.vtable_length(v), v))
            for vtable in vtables[1:]:
                vtable_class[vtable] = f"{name}_{vtable:08X}"
    # Parents of hash-named vtables: a constructor that installs this vtable after calling a base constructor.
    for vtable in type_slot:
        for function in stores[vtable]:
            installed, base = recovery.last_vtable_store(function)
            if installed == vtable and base:
                base_vtable, _ = recovery.last_vtable_store(base)
                if base_vtable and base_vtable != vtable:
                    parent_vtable.setdefault(vtable, base_vtable)
                function_names.setdefault(function, (f"{vtable_class[vtable]}::{vtable_class[vtable]}", "constructor"))

    # A slot is named after the most-derived class that changes it; inherited slots keep the base name.
    slot_owner = {}
    rows = []
    for vtable in sorted(stores):
        length = recovery.vtable_length(vtable)
        owner = vtable_class.get(vtable, "")
        parent = parent_vtable.get(vtable)
        rows.append((f"{vtable:08X}", length, owner, f"{parent:08X}" if parent else "", " ".join(f"{s:08X}" for s in sorted(stores[vtable]))))
        global_names.setdefault(vtable, (f"{owner}_vtable" if owner else f"vt_{vtable:08X}", f"vtable, {length} slots"))
    # Functions shared by unrelated vtables (common bases we cannot name yet) are left unnamed.
    containing = defaultdict(set)
    for vtable in stores:
        for slot in range(recovery.vtable_length(vtable)):
            containing[recovery.xbe.u32(vtable + slot * 4)].add(vtable)

    def ancestors(vtable):
        chain = {vtable}
        while vtable in parent_vtable:
            vtable = parent_vtable[vtable]
            chain.add(vtable)
        return chain

    for vtable, owner in sorted(vtable_class.items(), key=lambda item: -len(ancestors(item[0]))):
        parent = parent_vtable.get(vtable)
        length = recovery.vtable_length(vtable)
        parent_length = recovery.vtable_length(parent) if parent else 0
        for slot in range(length):
            target = recovery.xbe.u32(vtable + slot * 4)
            inherited = parent is not None and slot < parent_length and recovery.xbe.u32(parent + slot * 4) == target
            # Every vtable using this function must derive from this one (or be it) for the name to be right.
            users = containing[target]
            unique = all(vtable in ancestors(user) for user in users)
            if TEXT_START <= target < TEXT_END and not inherited and unique:
                slot_owner.setdefault(target, owner)
                function_names.setdefault(target, (f"{owner}::vf{slot:02X}", f"vtable slot {slot} (0x{slot * 4:X})"))
    write_csv("vtables.csv", ["vtable", "slots", "class", "parent_vtable", "stored_by"], rows)
    # Functions that install a class's vtable are its constructors and destructors.
    depth = {}
    for vtable in vtable_class:
        level, current = 0, vtable
        while current in parent_vtable:
            level, current = level + 1, parent_vtable[current]
        depth[vtable] = level
    installs = defaultdict(list)
    for vtable, functions in stores.items():
        if vtable in vtable_class:
            for function in functions:
                installs[function].append(vtable)
    for function, vtables in installs.items():
        vtable = max(vtables, key=lambda v: depth[v])
        owner = vtable_class[vtable]
        slot_owner.setdefault(function, owner)
        function_names.setdefault(function, (f"{owner}::Construct_{function:06X}", "installs the class vtable (constructor or destructor variant)"))

    fields = recovery.odf_fields()
    write_csv("odf_fields.csv", ["class", "function", "section", "key", "format", "offset", "call"],
              [(slot_owner.get(f, ""), f"{f:08X}", s, k, fmt, "" if o is None else f"0x{o:X}", f"{c:08X}") for f, s, k, fmt, o, c in fields])
    loader_sections = defaultdict(set)
    for function, section, *_ in fields:
        loader_sections[function].add(section)
    for function, sections in loader_sections.items():
        if function not in function_names:
            function_names[function] = (f"odf_load_{'_'.join(sorted(identifier(s) for s in sections))[:60]}", "reads ODF keys")
        elif function in slot_owner:
            owner = slot_owner[function]
            function_names[function] = (f"{owner}::LoadOdf_{function:06X}", "reads ODF keys; " + function_names[function][1])

    write_csv("functions.csv", ["address", "name", "comment"], [(f"{a:08X}", n, c) for a, (n, c) in sorted(function_names.items())])
    write_csv("globals.csv", ["address", "name", "comment"], [(f"{a:08X}", n, c) for a, (n, c) in sorted(global_names.items())])


if __name__ == "__main__":
    main()
