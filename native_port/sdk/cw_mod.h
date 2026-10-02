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

// Registers at a mid hook (saved with pushfd; pushad): the hooked code's esp is esp + 4.
typedef struct CwRegisters {
    uint32_t edi, esi, ebp, esp, ebx, edx, ecx, eax;
    uint32_t eflags;
} CwRegisters;

typedef void(__cdecl* CwMidHandler)(CwRegisters* registers);

// An Xbox gamepad's state, as the game reads it.
enum {
    CW_PAD_DPAD_UP = 0x01, CW_PAD_DPAD_DOWN = 0x02, CW_PAD_DPAD_LEFT = 0x04, CW_PAD_DPAD_RIGHT = 0x08,
    CW_PAD_START = 0x10, CW_PAD_BACK = 0x20, CW_PAD_LEFT_THUMB = 0x40, CW_PAD_RIGHT_THUMB = 0x80,
};
enum { CW_PAD_A, CW_PAD_B, CW_PAD_X, CW_PAD_Y, CW_PAD_BLACK, CW_PAD_WHITE, CW_PAD_LEFT_TRIGGER, CW_PAD_RIGHT_TRIGGER };
typedef struct CwPad {
    uint16_t buttons;   // CW_PAD_DPAD_UP | ...
    uint8_t analog[8];  // indexed by CW_PAD_A ...; 0-255
    int16_t thumbLX, thumbLY, thumbRX, thumbRY;
} CwPad;

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
    // Virtual gamepad on port 1-3 (players 2-4): the game sees a controller plugged in there that reads `pad`
    // (copied; call again whenever the state changes, for example every frame). nullptr unplugs it. Call it from
    // CwModInit to have the pad present from the start.
    void(__cdecl* setVirtualPad)(uint32_t port, const CwPad* pad);
    // Sees (and may change) every gamepad state the game reads, real or virtual, just before the game gets it;
    // port 0 is player 1 (host controller + keyboard). One filter; a later call replaces it.
    void(__cdecl* setPadFilter)(void(__cdecl* filter)(uint32_t port, CwPad* pad));
} CwModApi;

typedef int(__cdecl* CwModInitFunction)(const CwModApi* api);

#ifdef __cplusplus
}
#endif
