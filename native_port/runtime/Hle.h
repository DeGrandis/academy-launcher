#pragma once

#include <cstdint>

namespace cw::hle {

// Redirects the original function at `address` to `replacement` with a 5-byte jump.
void hookFunction(std::uint32_t address, const void* replacement, const char* name);
void installHooks();

void installXapiHooks();
void installD3DHooks();
void installInputHooks();
void installAudioHooks();

} // namespace cw::hle
