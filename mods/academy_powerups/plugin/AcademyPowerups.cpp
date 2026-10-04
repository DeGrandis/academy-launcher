// Powerup Academy: a second Thule Moon Academy (multi18.wld, a copy of multi5.wld with a 60 s powerup respawn)
// whose powerup changes type every time it respawns, alternating Super Blasters and Invincibility.
//
// Map powerups live in a table of 0x3C-byte entries at 0x43AE78 (count at 0x43AE64): +0x00 type (itemdesc.cfg
// Type), +0x04 position, +0x14 respawn time, +0x1C respawn countdown, +0x24 handle of the pickup object (0 while
// taken). Powerup_Update (0x7D520) creates the pickup at 0x7D858 when the countdown has run out; the hook there
// changes the entry's type just before that.

#include "cw_mod.h"

#include <cstdint>
#include <cstring>

namespace {

constexpr std::uint32_t kBatchBeginMission = 0x00062750;  // void __cdecl(const char* mission, const char* directory)
constexpr std::uint32_t kPowerupCreatePickup = 0x0007D858; // esi = entry + 0x0C, edi = entry index
constexpr int kMaxPowerups = 100;
constexpr char kMission[] = "multi18";

// itemdesc.cfg types: 1 Quad Damage (Super Blasters), 6 Ultimate Health (invincibility). Left out: 7 Total Offense
// ("Disintegration Field"), which the collision handler (0x332B0) only lets destroy vehicles that belong to players,
// so it does nothing against the Academy's AI waves; and 8 Invisibility (cloak), which hardly matters against them.
constexpr int kCycle[] = {1, 6};
const char* const kCycleNames[] = {"Super Blasters", "Invincibility"};

const CwModApi* g_api = nullptr;
void(__cdecl* g_originalBeginMission)(const char*, const char*) = nullptr;
bool g_active = false;
bool g_spawned[kMaxPowerups] = {};

void __cdecl beginMission(const char* mission, const char* directory) {
    g_active = mission != nullptr && _strnicmp(mission, kMission, sizeof(kMission) - 1) == 0;
    std::memset(g_spawned, 0, sizeof(g_spawned));
    g_api->log("academy_powerups: mission '%s'%s", mission != nullptr ? mission : "(null)", g_active ? " (powerup cycle on)" : "");
    g_originalBeginMission(mission, directory);
}

void __cdecl createPickup(CwRegisters* registers) {
    const int index = static_cast<int>(registers->edi);
    if (!g_active || index < 0 || index >= kMaxPowerups) {
        return;
    }
    auto* type = reinterpret_cast<int*>(static_cast<std::uintptr_t>(registers->esi - 0x0C));
    int position = -1;
    for (int i = 0; i < static_cast<int>(sizeof(kCycle) / sizeof(kCycle[0])); ++i) {
        if (*type == kCycle[i]) {
            position = i;
        }
    }
    if (position < 0) {
        return;
    }
    // The first appearance keeps the map's type; every respawn after it moves to the next one.
    if (g_spawned[index]) {
        position = (position + 1) % static_cast<int>(sizeof(kCycle) / sizeof(kCycle[0]));
        *type = kCycle[position];
    }
    g_spawned[index] = true;
    g_api->log("academy_powerups: powerup %d spawns as %s", index, kCycleNames[position]);
}

} // namespace

extern "C" __declspec(dllexport) int __cdecl CwModInit(const CwModApi* api) {
    if (api->version != CW_MOD_API_VERSION) {
        return 0;
    }
    g_api = api;
    return api->detour(kBatchBeginMission, reinterpret_cast<const void*>(&beginMission), reinterpret_cast<void**>(&g_originalBeginMission),
               "Batch_BeginMission (academy_powerups)")
        && api->midHook(kPowerupCreatePickup, &createPickup, "Powerup_CreatePickup (academy_powerups)");
}
