// Computer players in the split-screen multiplayer lobby.
//
// Player 1 presses RB (keyboard R) to move between the lobby's player panels: 2, 3, 4, then back to their own.
// While another panel is selected, player 1's controller drives that panel instead of their own: START joins it as
// a computer player, A steps through it, left/right changes its team, B backs out. A selected empty panel gets a
// virtual gamepad (setVirtualPad) so the game shows it as "Press START to join"; one left without joining is
// unplugged again. Joined computer panels are labelled "AI".
//
// Lobby screen object: vtable 0x348E88; slot 3 runs when the screen opens. +0x24 four player panels. Panel: +0x7C
// index, +0x80 state (0 no controller, 1 press START to join, 2 profile, 4 team, 3 vehicle, 5 ready), +0x82 team
// (0 = team 1, 1 = team 2). The panel header is built at 0x12B060; at 0x12B0DA its name (wide string) is at the
// hooked code's esp + 0x18 and the panel in esi.

#include "cw_mod.h"
#include "game/GameSymbols.h"

#include <cstdint>
#include <cstring>
#include <cwchar>
#include <intrin.h>

namespace {

constexpr std::uint32_t kLobbyVtable = 0x00348E88;
constexpr int kLobbyOpenSlot = 3;
constexpr std::uint32_t kPanelNameReady = 0x0012B0DA;
constexpr std::uint32_t kPanelHeaderReady = 0x0012B1B0;
constexpr int kSlots = 4;
constexpr int kNotJoined = 1;  // panel states 0 and 1

const CwModApi* g_api = nullptr;
void(__fastcall* g_originalOpen)(std::uint8_t*, void*) = nullptr;
void(__cdecl* g_originalBeginMission)(const char*, const char*) = nullptr;

std::uint8_t* g_lobby = nullptr;  // while the lobby screen is up
int g_cursor = 0;                 // panel player 1 is controlling: 0 = their own, 1-3 = a computer player's
bool g_plugged[kSlots] = {};      // ports with a virtual gamepad
CwPad g_forwarded{};              // player 1's input, passed to the selected panel
bool g_switchHeld = false;

bool inLobby() {
    return g_lobby != nullptr && *reinterpret_cast<const std::uint32_t*>(g_lobby) == kLobbyVtable;
}

int panelState(int slot) {
    const std::uint8_t* panel = reinterpret_cast<std::uint8_t* const*>(g_lobby + 0x24)[slot];
    return panel != nullptr ? panel[0x80] : 0;
}

void plug(int slot) {
    if (!g_plugged[slot]) {
        const CwPad idle{};
        g_plugged[slot] = true;
        g_api->setVirtualPad(slot, &idle);
        g_api->log("ai_players: computer player %d plugged in", slot + 1);
    }
}

void unplug(int slot) {
    if (g_plugged[slot]) {
        g_plugged[slot] = false;
        g_api->setVirtualPad(slot, nullptr);
        g_api->log("ai_players: computer player %d removed", slot + 1);
    }
}

void selectPanel(int slot) {
    if (slot == g_cursor) {
        return;
    }
    g_cursor = slot;
    g_forwarded = CwPad{};
    if (slot != 0) {
        plug(slot);
    }
    g_api->log("ai_players: player 1 now controls panel %d", slot + 1);
}

void __cdecl padFilter(uint32_t port, CwPad* pad) {
    if (port == 0) {
        if (!inLobby()) {
            g_cursor = 0;
            return;
        }
        const bool switchPressed = pad->analog[CW_PAD_BLACK] > 128;
        if (switchPressed && !g_switchHeld) {
            selectPanel((g_cursor + 1) % kSlots);
        }
        g_switchHeld = switchPressed;
        // Computer panels nobody is controlling that went back to "press START to join" (or never joined) leave.
        for (int slot = 1; slot < kSlots; ++slot) {
            if (slot != g_cursor && g_plugged[slot] && panelState(slot) <= kNotJoined) {
                unplug(slot);
            }
        }
        if (g_cursor != 0) {
            g_forwarded = *pad;
            g_forwarded.analog[CW_PAD_BLACK] = 0;
            *pad = CwPad{};
        }
        return;
    }
    if (port < kSlots && g_plugged[port]) {
        *pad = inLobby() && g_cursor == static_cast<int>(port) ? g_forwarded : CwPad{};
    }
}

void __fastcall lobbyOpen(std::uint8_t* lobby, void* edx) {
    g_originalOpen(lobby, edx);
    g_lobby = lobby;
    g_cursor = 0;
}

void __cdecl panelNameReady(CwRegisters* registers) {
    const auto* panel = reinterpret_cast<const std::uint8_t*>(static_cast<std::uintptr_t>(registers->esi));
    const int slot = *reinterpret_cast<const std::int32_t*>(panel + 0x7C);
    if (slot > 0 && slot < kSlots && g_plugged[slot]) {
        auto* name = reinterpret_cast<wchar_t*>(static_cast<std::uintptr_t>(registers->esp + 4 + 0x18));
        std::wcscpy(name, L"AI");
    }
}

// The panel header while the panel is being set up: at 0x12B1B0 eax points to the name about to be shown.
void __cdecl panelHeaderReady(CwRegisters* registers) {
    static const wchar_t kName[] = L"AI";
    const auto* panel = reinterpret_cast<const std::uint8_t*>(static_cast<std::uintptr_t>(registers->esi));
    const int slot = *reinterpret_cast<const std::int32_t*>(panel + 0x7C);
    if (slot > 0 && slot < kSlots && g_plugged[slot]) {
        registers->eax = static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(kName));
    }
}

void __cdecl beginMission(const char* mission, const char* directory) {
    g_lobby = nullptr;
    g_cursor = 0;
    int computers = 0;
    for (int slot = 1; slot < kSlots; ++slot) {
        computers += g_plugged[slot] ? 1 : 0;
    }
    if (computers > 0) {
        g_api->log("ai_players: %s starts with %d computer player(s)", mission != nullptr ? mission : "?", computers);
    }
    g_originalBeginMission(mission, directory);
}

// Computer players get no screen of their own: the split-screen layout and the per-player HUD loop ask
// IsLocalPlayer(slot) (0x739F0, bool __cdecl(int slot)) from the render code (0xEE000-0xF0000); for a computer slot
// that answer is "no", so player 1 keeps the whole screen. Everywhere else (controls, match logic) it stays a local
// player.
constexpr std::uint32_t kIsLocalPlayer = 0x000739F0;
constexpr std::uintptr_t kRenderCodeStart = 0x000EE000;
constexpr std::uintptr_t kRenderCodeEnd = 0x000F0000;
bool(__cdecl* g_originalIsLocalPlayer)(int) = nullptr;

// The 3D views are laid out by ViewRectForPlayer (0xEDD50; stack: camera, index among the players with a screen,
// number of such players, ...), whose count in the 3D pass is worked out separately; take the computer players
// out of it there (a count of 1 or less is the full screen).
constexpr std::uint32_t kViewRectForPlayer = 0x000EDD50;

void __cdecl viewRectForPlayer(CwRegisters* registers) {
    if (inLobby()) {
        return;
    }
    int computers = 0;
    for (int slot = 1; slot < kSlots; ++slot) {
        computers += g_plugged[slot] ? 1 : 0;
    }
    auto& count = *reinterpret_cast<std::int32_t*>(static_cast<std::uintptr_t>(registers->esp + 4 + 0xC));
    if (computers > 0 && count > 1) {
        count = count - computers < 1 ? 1 : count - computers;
    }
}

bool __cdecl isLocalPlayer(int slot) {
    const auto caller = reinterpret_cast<std::uintptr_t>(_ReturnAddress());
    if (slot > 0 && slot < kSlots && g_plugged[slot] && !inLobby() && caller >= kRenderCodeStart && caller < kRenderCodeEnd) {
        return false;
    }
    return g_originalIsLocalPlayer(slot);
}
} // namespace

extern "C" __declspec(dllexport) int __cdecl CwModInit(const CwModApi* api) {
    if (api->version != CW_MOD_API_VERSION) {
        return 0;
    }
    g_api = api;
    if (api->setVirtualPad == nullptr || api->setPadFilter == nullptr) {
        api->log("ai_players: this runtime has no virtual gamepad support");
        return 0;
    }
    api->setPadFilter(&padFilter);
    return api->hookVirtual(kLobbyVtable, kLobbyOpenSlot, reinterpret_cast<const void*>(&lobbyOpen),
               reinterpret_cast<void**>(&g_originalOpen), "Lobby open (ai_players)")
        && api->midHook(kPanelNameReady, &panelNameReady, "Lobby panel name (ai_players)")
        && api->midHook(kPanelHeaderReady, &panelHeaderReady, "Lobby panel header (ai_players)")
        && api->detour(kIsLocalPlayer, reinterpret_cast<const void*>(&isLocalPlayer), reinterpret_cast<void**>(&g_originalIsLocalPlayer),
               "IsLocalPlayer (ai_players: no screen for computer players)")
        && api->midHook(kViewRectForPlayer, &viewRectForPlayer, "ViewRectForPlayer (ai_players: full screen)")
        && api->detour(reinterpret_cast<std::uint32_t>(cw::game::Batch_BeginMission), reinterpret_cast<const void*>(&beginMission),
               reinterpret_cast<void**>(&g_originalBeginMission), "Batch_BeginMission (ai_players)");
}
