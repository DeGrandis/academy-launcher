// Lets one player start any local multiplayer mode (Conquest, Deathmatch, King of the Hill), for testing maps alone.
//
// The split-screen lobby's "can start" check (0x12A9A0, bool __thiscall) needs at least two ready players, and one
// on each team when teams are on. This keeps the game's answer when it allows a start, and otherwise allows it when
// at least one player is ready and nobody who joined is still choosing.
//
// Lobby layout: +0x24 four player panels, +0x34 index of the panel that always counts as ready (player one).
// Panel: +0x80 state (0/1 = no controller / not joined, 5 = ready), +0x82 team.

#include "cw_mod.h"

#include <cstdint>

namespace {

constexpr std::uint32_t kLobbyCanStart = 0x0012A9A0;
constexpr std::uint8_t kPanelReady = 5;

const CwModApi* g_api = nullptr;
bool(__fastcall* g_originalCanStart)(std::uint8_t*, void*) = nullptr;
bool g_logged = false;

bool __fastcall canStart(std::uint8_t* lobby, void* edx) {
    if (g_originalCanStart(lobby, edx)) {
        return true;
    }
    const auto host = *reinterpret_cast<std::int32_t*>(lobby + 0x34);
    int ready = 0;
    for (int index = 0; index < 4; ++index) {
        const std::uint8_t* panel = reinterpret_cast<std::uint8_t* const*>(lobby + 0x24)[index];
        if (panel == nullptr) {
            continue;
        }
        const std::uint8_t state = panel[0x80];
        if (state == kPanelReady || index == host) {
            ++ready;
        } else if (state > 1) {
            return false;  // someone joined but is still picking a team or vehicle
        }
    }
    if (ready == 0) {
        return false;
    }
    if (!g_logged) {
        g_logged = true;
        g_api->log("mp_solo: allowing a start with %d ready player(s)", ready);
    }
    return true;
}

} // namespace

extern "C" __declspec(dllexport) int __cdecl CwModInit(const CwModApi* api) {
    if (api->version != CW_MOD_API_VERSION) {
        return 0;
    }
    g_api = api;
    return api->detour(kLobbyCanStart, reinterpret_cast<const void*>(&canStart), reinterpret_cast<void**>(&g_originalCanStart),
        "Lobby_CanStart (mp_solo)");
}
