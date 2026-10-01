#include "Hle.h"

#include "Log.h"

#include <windows.h>

#include <cstdlib>
#include <cstring>
#include <string>

namespace cw::hle {

// CW_DISABLE_HOOKS="name,name" leaves the listed hooks out, to compare against the original game code.
bool hookDisabled(const char* name) {
    static const std::string list = [] {
        const char* value = std::getenv("CW_DISABLE_HOOKS");
        return value == nullptr ? std::string() : "," + std::string(value) + ",";
    }();
    return !list.empty() && list.find("," + std::string(name) + ",") != std::string::npos;
}

void hookFunction(std::uint32_t address, const void* replacement, const char* name) {
    if (hookDisabled(name)) {
        logf("hle: %s @ %08X disabled by CW_DISABLE_HOOKS", name, address);
        return;
    }
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
