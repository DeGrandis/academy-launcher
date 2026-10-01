#include "ModLoader.h"

#include "Hooks.h"
#include "Log.h"

#include "../sdk/cw_mod.h"

#include <windows.h>

#include <algorithm>
#include <cstdarg>
#include <cstdlib>
#include <string>
#include <vector>

namespace cw::mods {

namespace {

std::string g_gameRoot;
std::string g_modRoot;

void __cdecl apiLog(const char* format, ...) {
    va_list args;
    va_start(args, format);
    logv(format, args);
    va_end(args);
}

int __cdecl apiDetour(uint32_t target, const void* replacement, void** original, const char* name) {
    return hooks::detour(target, replacement, original, name) ? 1 : 0;
}

int __cdecl apiMidHook(uint32_t address, CwMidHandler handler, const char* name) {
    return hooks::midHook(address, reinterpret_cast<hooks::MidHandler>(handler), name) ? 1 : 0;
}

int __cdecl apiHookVirtual(uint32_t vtable, int slot, const void* replacement, void** original, const char* name) {
    return hooks::hookVirtual(vtable, slot, replacement, original, name) ? 1 : 0;
}

int __cdecl apiPatchBytes(uint32_t address, const void* bytes, uint32_t length, const char* name) {
    return hooks::patchBytes(address, bytes, length, name) ? 1 : 0;
}

static_assert(sizeof(CwRegisters) == sizeof(hooks::Registers), "plugin register layout must match the runtime's");

void loadFrom(const std::filesystem::path& directory, const CwModApi& api) {
    std::error_code error;
    if (!std::filesystem::is_directory(directory, error)) {
        return;
    }
    std::vector<std::filesystem::path> plugins;
    for (const auto& entry : std::filesystem::directory_iterator(directory, error)) {
        if (entry.path().extension() == ".dll") {
            plugins.push_back(entry.path());
        }
    }
    std::sort(plugins.begin(), plugins.end());
    for (const auto& path : plugins) {
        HMODULE module = LoadLibraryW(path.c_str());
        if (module == nullptr) {
            logf("mods: cannot load %s (error %lu)", path.string().c_str(), GetLastError());
            continue;
        }
        auto init = reinterpret_cast<CwModInitFunction>(GetProcAddress(module, "CwModInit"));
        if (init == nullptr) {
            logf("mods: %s has no CwModInit export", path.string().c_str());
            continue;
        }
        const int result = init(&api);
        logf("mods: %s at %p initialized (%s)", path.filename().string().c_str(), reinterpret_cast<void*>(module), result != 0 ? "ok" : "FAILED");
    }
}

void __cdecl selfTestHandler(CwRegisters* registers) {
    static int calls = 0;
    if (++calls <= 3) {
        logf("hooks: self-test mid hook reached (esp=%08X ebp=%08X)", registers->esp, registers->ebp);
    }
}

} // namespace

void loadPlugins(const std::filesystem::path& gameRoot, const std::filesystem::path& exeDirectory) {
    g_gameRoot = gameRoot.string();
    const char* modRoot = std::getenv("CW_MOD_ROOT");
    g_modRoot = modRoot != nullptr ? modRoot : "";
    static CwModApi api{};
    api.version = CW_MOD_API_VERSION;
    api.log = &apiLog;
    api.detour = &apiDetour;
    api.midHook = &apiMidHook;
    api.hookVirtual = &apiHookVirtual;
    api.patchBytes = &apiPatchBytes;
    api.gameRoot = g_gameRoot.c_str();
    api.modRoot = g_modRoot.c_str();
    hooks::initialize();
    if (const char* address = std::getenv("CW_HOOK_SELFTEST")) {
        // Debugging aid: a runtime-side mid hook at the given address.
        hooks::midHook(static_cast<std::uint32_t>(std::strtoul(address, nullptr, 16)), reinterpret_cast<hooks::MidHandler>(&selfTestHandler), "self-test");
    }
    loadFrom(exeDirectory / "plugins", api);
    if (!g_modRoot.empty()) {
        loadFrom(std::filesystem::path(g_modRoot) / "plugins", api);
    }
}

} // namespace cw::mods
