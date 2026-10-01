// Clone Wars native port plugin interface.
//
// A plugin is a 32-bit DLL that exports
//     extern "C" __declspec(dllexport) int __cdecl CwModInit(const CwModApi* api);
// The runtime loads every DLL in <CW_MOD_ROOT>/plugins and <exe dir>/plugins after its own hooks are installed and
// before the game starts, and calls CwModInit once. Return non-zero on success.
//
// Game addresses and typed accessors come from native_port/runtime/game/GameSymbols.h (generated from symbols/).
// Game __thiscall functions are replaced with __fastcall functions taking (void* self, void* edx, ...).
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CW_MOD_API_VERSION 1

typedef struct CwRegisters {
    uint32_t edi, esi, ebp, esp, ebx, edx, ecx, eax;
    uint32_t eflags;
} CwRegisters;

typedef void(__cdecl* CwMidHandler)(CwRegisters* registers);

typedef struct CwModApi {
    uint32_t version;  // CW_MOD_API_VERSION
    // Writes a line to cw_runtime.log.
    void(__cdecl* log)(const char* format, ...);
    // Detours a game function; *original receives a callable pointer to the original code.
    int(__cdecl* detour)(uint32_t target, const void* replacement, void** original, const char* name);
    // Calls handler with the registers whenever execution reaches address, then continues the original code.
    int(__cdecl* midHook)(uint32_t address, CwMidHandler handler, const char* name);
    // Replaces a vtable slot; *original receives the previous function.
    int(__cdecl* hookVirtual)(uint32_t vtable, int slot, const void* replacement, void** original, const char* name);
    // Overwrites bytes in the game image.
    int(__cdecl* patchBytes)(uint32_t address, const void* bytes, uint32_t length, const char* name);
    // Host folders: the game files and the active mod overlay (empty string when none).
    const char* gameRoot;
    const char* modRoot;
} CwModApi;

typedef int(__cdecl* CwModInitFunction)(const CwModApi* api);

#ifdef __cplusplus
}
#endif
