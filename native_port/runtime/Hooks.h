#pragma once

#include <cstdint>

namespace cw::hooks {

// Registers as saved by a mid-function hook (pushfd; pushad). Handlers may change any of them except esp;
// the hooked code continues with the changed values.
struct Registers {
    std::uint32_t edi, esi, ebp, esp, ebx, edx, ecx, eax;
    std::uint32_t eflags;
};
using MidHandler = void(__cdecl*)(Registers* registers);

void initialize();

// Detours the function at `target`: callers reach `replacement`, which can call the original through
// *original. Game __thiscall functions are replaced with __fastcall(void* self, void* edx, ...).
bool detour(std::uint32_t target, const void* replacement, void** original, const char* name);

// Runs `handler` with the registers whenever execution reaches `address`, then continues the original code.
// The instructions overwritten at `address` (5+ bytes) must not be jump targets.
bool midHook(std::uint32_t address, MidHandler handler, const char* name);

// Replaces slot `slot` of the vtable at `vtable`; *original receives the previous function.
bool hookVirtual(std::uint32_t vtable, int slot, const void* replacement, void** original, const char* name);

// Overwrites code or data in the game image.
bool patchBytes(std::uint32_t address, const void* bytes, std::uint32_t length, const char* name);

} // namespace cw::hooks
