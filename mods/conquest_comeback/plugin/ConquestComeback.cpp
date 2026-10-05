// Conquest comeback mechanic (like a loss bonus): outposts of the side that holds fewer of them build their turrets
// and units faster. Each outpost of the trailing side gets extra build progress of
//     strength x min(outposts behind, 3)
// times its normal rate, so a side that is 2 outposts behind at strength 0.5 builds twice as fast.
// CW_COMEBACK=<strength> (default 0.5; 0 = off). CW_COMEBACK_LOG=1 logs each outpost's build timer once a second.
//
// Zones: 0x58-byte entries at 0x43E5B8 (count 0x43E550): +0x10 type (2 = outpost), +0x14 owner team, +0x1C seconds
// spent on the current item (counts up to about 6, then the item is built and it starts over), +0x24 the game time
// the item completes (-1 until the last second), +0x28 index of the turret/unit being built (-1 = all built), +0x2C
// claimed. Each frame the plugin adds to +0x1C a multiple of how far it moved since the last frame.

#include "cw_mod.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace {

constexpr std::uint32_t kPowerupUpdate = 0x0007D520;  // every multiplayer frame
constexpr std::uint32_t kBeginMission = 0x00062750;
constexpr std::uint32_t kWorld = 0x003A2F0C;
constexpr std::uint32_t kZoneCount = 0x0043E550;
constexpr std::uint32_t kZones = 0x0043E5B8;
constexpr int kMaxZones = 20;

const CwModApi* g_api = nullptr;
void(__cdecl* g_originalBeginMission)(const char*, const char*) = nullptr;
bool g_conquest = false;
float g_strength = 0.5f;
bool g_log = false;
float g_lastTimer[kMaxZones];
int g_lastItem[kMaxZones];
double g_nextLog = 0.0;

int sideOf(int team) {
    const int side = team > 10 ? team - 10 : team;
    return side == 1 || side == 2 ? side : 0;
}

void __cdecl tick(CwRegisters*) {
    if (!g_conquest || g_strength <= 0.0f) {
        return;
    }
    const int count = std::min(*reinterpret_cast<const std::int32_t*>(kZoneCount), kMaxZones);
    int held[3] = {};
    for (int index = 0; index < count; ++index) {
        const std::uint8_t* zone = reinterpret_cast<const std::uint8_t*>(kZones + index * 0x58);
        if (*reinterpret_cast<const std::int32_t*>(zone + 0x10) == 2 && zone[0x2C] != 0) {
            ++held[sideOf(*reinterpret_cast<const std::int32_t*>(zone + 0x14))];
        }
    }
    auto* world = *reinterpret_cast<std::uint8_t**>(kWorld);
    const double now = world != nullptr ? *reinterpret_cast<const float*>(world + 0x28) : 0.0;
    const bool logNow = g_log && now >= g_nextLog;
    if (logNow) {
        g_nextLog = now + 1.0;
    }
    for (int index = 0; index < count; ++index) {
        std::uint8_t* zone = reinterpret_cast<std::uint8_t*>(kZones + index * 0x58);
        auto& timer = *reinterpret_cast<float*>(zone + 0x1C);  // seconds into building the current item
        const int item = *reinterpret_cast<const std::int32_t*>(zone + 0x28);
        const int side = sideOf(*reinterpret_cast<const std::int32_t*>(zone + 0x14));
        const bool building = *reinterpret_cast<const std::int32_t*>(zone + 0x10) == 2 && zone[0x2C] != 0 && item >= 0 && side != 0;
        if (building && item == g_lastItem[index]) {
            const float moved = timer - g_lastTimer[index];
            const int behind = held[3 - side] - held[side];
            if (behind > 0 && std::fabs(moved) < 1.0f) {
                timer += moved * g_strength * static_cast<float>(behind < 3 ? behind : 3);
            }
        }
        if (logNow && building) {
            g_api->log("comeback: t=%.0f zone %d side %d item %d timer %.2f (outposts %d vs %d)", now, index, side, item, timer,
                held[side], held[3 - side]);
        }
        g_lastTimer[index] = timer;
        g_lastItem[index] = building ? item : -2;
    }
}

void __cdecl beginMission(const char* mission, const char* directory) {
    g_originalBeginMission(mission, directory);
    static const char* const kMaps[] = {"multi6.", "multi8.", "multi10.", "multi12.", "multi19.", "cq1.", "cq2.", "cq3.", "cq4.", "cq5."};
    g_conquest = false;
    for (const char* map : kMaps) {
        if (mission != nullptr && _strnicmp(mission, map, std::strlen(map)) == 0) {
            g_conquest = true;
        }
    }
    for (int index = 0; index < kMaxZones; ++index) {
        g_lastItem[index] = -2;
    }
    g_nextLog = 0.0;
    if (g_conquest) {
        g_api->log("conquest_comeback: strength %.2f on %s", g_strength, mission);
    }
}

} // namespace

extern "C" __declspec(dllexport) int __cdecl CwModInit(const CwModApi* api) {
    if (api->version != CW_MOD_API_VERSION) {
        return 0;
    }
    g_api = api;
    if (const char* value = std::getenv("CW_COMEBACK")) {
        g_strength = static_cast<float>(std::atof(value));
    }
    g_log = std::getenv("CW_COMEBACK_LOG") != nullptr;
    return api->detour(kBeginMission, reinterpret_cast<const void*>(&beginMission), reinterpret_cast<void**>(&g_originalBeginMission),
               "Batch_BeginMission (conquest_comeback)")
        && api->midHook(kPowerupUpdate, &tick, "Powerup_Update (conquest_comeback)");
}
