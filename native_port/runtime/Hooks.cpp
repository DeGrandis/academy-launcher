#include "Hooks.h"

#include "Hle.h"
#include "Log.h"

#include <windows.h>

#include <MinHook.h>

extern "C" {
#include "hde/hde32.h"
}

#include <cstring>
#include <map>
#include <mutex>

namespace cw::hooks {

namespace {

std::mutex g_mutex;
bool g_initialized = false;

bool writeCode(std::uint32_t address, const void* bytes, std::uint32_t length) {
    auto* target = reinterpret_cast<void*>(static_cast<std::uintptr_t>(address));
    DWORD oldProtect;
    if (!VirtualProtect(target, length, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        return false;
    }
    std::memcpy(target, bytes, length);
    VirtualProtect(target, length, oldProtect, &oldProtect);
    FlushInstructionCache(GetCurrentProcess(), target, length);
    return true;
}

std::uint8_t* allocateStub(std::size_t size) {
    // Stubs live in one executable arena; hooks are installed once and never removed.
    static std::uint8_t* arena = nullptr;
    static std::size_t used = 0;
    constexpr std::size_t kArenaSize = 0x10000;
    if (arena == nullptr || used + size > kArenaSize) {
        arena = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, kArenaSize, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
        used = 0;
        if (arena == nullptr) {
            return nullptr;
        }
    }
    std::uint8_t* stub = arena + used;
    used += (size + 15) & ~std::size_t{15};
    return stub;
}

// Writes a 5-byte call/jmp into `buffer`; `location` is where those bytes will execute (when assembled elsewhere).
void emitJump(std::uint8_t* buffer, const void* destination, std::uint8_t opcode = 0xE9, const void* location = nullptr) {
    const auto* executesAt = static_cast<const std::uint8_t*>(location != nullptr ? location : buffer);
    buffer[0] = opcode;
    const auto relative = static_cast<std::int32_t>(static_cast<const std::uint8_t*>(destination) - (executesAt + 5));
    std::memcpy(buffer + 1, &relative, 4);
}

} // namespace

void initialize() {
    std::lock_guard lock(g_mutex);
    if (!g_initialized) {
        g_initialized = MH_Initialize() == MH_OK;
    }
}

bool detour(std::uint32_t target, const void* replacement, void** original, const char* name) {
    if (hle::hookDisabled(name)) {
        logf("hooks: %s @ %08X disabled by CW_DISABLE_HOOKS", name, target);
        return true;
    }
    initialize();
    std::lock_guard lock(g_mutex);
    // A game function can be detoured by several plugins: a later detour hooks the previous replacement instead, so
    // calls run newest first and each "original" leads to the one before it, ending at the game code.
    static std::map<std::uint32_t, const void*> replacements;
    auto* address = reinterpret_cast<void*>(static_cast<std::uintptr_t>(target));
    if (const auto previous = replacements.find(target); previous != replacements.end()) {
        address = const_cast<void*>(previous->second);
        logf("hooks: %s @ %08X is already detoured; chaining after %p", name, target, address);
    }
    MH_STATUS status = MH_CreateHook(address, const_cast<void*>(replacement), original);
    if (status == MH_OK) {
        status = MH_EnableHook(address);
    }
    if (status != MH_OK) {
        logf("hooks: detour %s @ %08X failed: %s", name, target, MH_StatusToString(status));
        return false;
    }
    replacements[target] = replacement;
    logf("hooks: detour %s @ %08X (original %p)", name, target, original != nullptr ? *original : nullptr);
    return true;
}

bool midHook(std::uint32_t address, MidHandler handler, const char* name) {
    if (hle::hookDisabled(name)) {
        logf("hooks: %s @ %08X disabled by CW_DISABLE_HOOKS", name, address);
        return true;
    }
    std::lock_guard lock(g_mutex);
    auto* code = reinterpret_cast<std::uint8_t*>(static_cast<std::uintptr_t>(address));

    // Copy whole instructions covering the 5-byte jump, relocating relative calls and jumps.
    std::uint8_t relocated[64];
    std::size_t relocatedLength = 0;
    std::size_t stolen = 0;
    while (stolen < 5) {
        hde32s instruction{};
        const unsigned length = hde32_disasm(code + stolen, &instruction);
        if (length == 0 || (instruction.flags & F_ERROR)) {
            logf("hooks: mid hook %s @ %08X: cannot decode instruction at +%zu", name, address, stolen);
            return false;
        }
        const std::uint8_t opcode = instruction.opcode;
        if ((opcode == 0xE8 || opcode == 0xE9) && length == 5) {
            const std::uintptr_t destination = reinterpret_cast<std::uintptr_t>(code + stolen + 5) + static_cast<std::int32_t>(instruction.imm.imm32);
            relocated[relocatedLength] = opcode;
            relocatedLength += 5;  // destination filled in once the stub address is known
            std::memcpy(relocated + relocatedLength - 4, &destination, 4);
        } else if ((instruction.flags & F_RELATIVE) != 0 || opcode == 0xC3 || opcode == 0xC2) {
            logf("hooks: mid hook %s @ %08X: instruction at +%zu cannot be moved", name, address, stolen);
            return false;
        } else {
            std::memcpy(relocated + relocatedLength, code + stolen, length);
            relocatedLength += length;
        }
        stolen += length;
    }

    // pushfd; pushad; push esp; call handler; add esp, 4; popad; popfd; <moved instructions>; jmp back
    std::uint8_t* stub = allocateStub(32 + relocatedLength);
    if (stub == nullptr) {
        return false;
    }
    std::size_t at = 0;
    stub[at++] = 0x9C;
    stub[at++] = 0x60;
    stub[at++] = 0x54;
    emitJump(stub + at, reinterpret_cast<const void*>(handler), 0xE8);
    at += 5;
    stub[at++] = 0x83;
    stub[at++] = 0xC4;
    stub[at++] = 0x04;
    stub[at++] = 0x61;
    stub[at++] = 0x9D;
    std::size_t copied = 0;
    while (copied < relocatedLength) {
        hde32s instruction{};
        const unsigned length = hde32_disasm(relocated + copied, &instruction);
        if ((relocated[copied] == 0xE8 || relocated[copied] == 0xE9) && length == 5) {
            std::uintptr_t destination;
            std::memcpy(&destination, relocated + copied + 1, 4);
            emitJump(stub + at, reinterpret_cast<const void*>(destination), relocated[copied]);
        } else {
            std::memcpy(stub + at, relocated + copied, length);
        }
        at += length;
        copied += length;
    }
    emitJump(stub + at, code + stolen);
    FlushInstructionCache(GetCurrentProcess(), stub, at + 5);

    std::uint8_t patch[16];
    std::memset(patch, 0x90, sizeof(patch));
    emitJump(patch, stub, 0xE9, code);
    if (!writeCode(address, patch, static_cast<std::uint32_t>(stolen))) {
        return false;
    }
    logf("hooks: mid hook %s @ %08X (%zu bytes moved, stub %p, handler %p)", name, address, stolen, stub, reinterpret_cast<void*>(handler));
    return true;
}

bool hookVirtual(std::uint32_t vtable, int slot, const void* replacement, void** original, const char* name) {
    if (hle::hookDisabled(name)) {
        logf("hooks: %s @ %08X disabled by CW_DISABLE_HOOKS", name, vtable);
        return true;
    }
    std::lock_guard lock(g_mutex);
    const std::uint32_t entry = vtable + static_cast<std::uint32_t>(slot) * 4;
    const auto previous = *reinterpret_cast<void**>(static_cast<std::uintptr_t>(entry));
    if (original != nullptr) {
        *original = previous;
    }
    if (!writeCode(entry, &replacement, 4)) {
        logf("hooks: virtual %s (vtable %08X slot %d) failed", name, vtable, slot);
        return false;
    }
    logf("hooks: virtual %s (vtable %08X slot %d, was %p)", name, vtable, slot, previous);
    return true;
}

bool patchBytes(std::uint32_t address, const void* bytes, std::uint32_t length, const char* name) {
    if (hle::hookDisabled(name)) {
        logf("hooks: %s @ %08X disabled by CW_DISABLE_HOOKS", name, address);
        return true;
    }
    std::lock_guard lock(g_mutex);
    const bool written = writeCode(address, bytes, length);
    logf("hooks: patch %s @ %08X (%u bytes)%s", name, address, length, written ? "" : " FAILED");
    return written;
}

} // namespace cw::hooks
