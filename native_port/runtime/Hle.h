#pragma once

#include <cstdint>

namespace cw::hle {

// Redirects the original function at `address` to `replacement` with a 5-byte jump.
void hookFunction(std::uint32_t address, const void* replacement, const char* name);
// True when CW_DISABLE_HOOKS lists `name`.
bool hookDisabled(const char* name);
void installHooks();

void installXapiHooks();
void installD3DHooks();
void installInputHooks();
// Virtual gamepad on port 1-3; pad is an Xbox gamepad state (CwPad layout), nullptr unplugs it.
void setVirtualPad(std::uint32_t port, const void* pad);
// Called with every gamepad state (CwPad layout) the game reads; may change it.
using PadFilter = void(__cdecl*)(std::uint32_t port, void* pad);
void setPadFilter(PadFilter filter);
void installAudioHooks();

} // namespace cw::hle
