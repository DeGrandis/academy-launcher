#include "Hle.h"

#include "Log.h"

#include <windows.h>

#include <cstring>

namespace cw::hle {

void hookFunction(std::uint32_t address, const void* replacement, const char* name) {
    auto* code = reinterpret_cast<std::uint8_t*>(static_cast<std::uintptr_t>(address));
    const auto relative = static_cast<std::int32_t>(reinterpret_cast<const std::uint8_t*>(replacement) - (code + 5));
    code[0] = 0xE9;
    std::memcpy(code + 1, &relative, 4);
    logf("hle: %s @ %08X", name, address);
}

void installHooks() {
    installXapiHooks();
    installD3DHooks();
    installInputHooks();
    installAudioHooks();
    FlushInstructionCache(GetCurrentProcess(), nullptr, 0);
}

} // namespace cw::hle
