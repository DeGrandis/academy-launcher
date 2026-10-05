// Test-only telemetry for Conquest balance runs (tools/balance/run_conquest.ps1; never in a preset).
//
// Once per game second (from Powerup_Update, which every multiplayer mode runs each frame) it walks every object and
// sorts the ones with health into categories by side (team 1/11 = side 1 Republic, 2/12 = side 2 CIS):
//   player  - a player's vehicle ([[0x3A2F0C]+0x14][slot])
//   troop   - outpost-built units (team 11/12)
//   turret  - outpost turrets and guard towers (team 1/2, max health 750 or 800)
//   hq      - the HQ (max health 6000), gen - the HQ's shield generator (2000)
//   other   - anything else with health on a side (outpost buildings, ...)
// It tracks each object by handle between scans: health lost is "damage taken", and an object that is gone or at 0
// health counts as a death. Every 30 s it logs a "stats:" line with the running totals and the outposts each side
// holds; when an HQ dies it logs "stats: winner side N", and at the end screen a "final" report (with
// CW_STATS_EXIT=1 it then ends the process). CW_STATS_MINUTES=n ends the match n minutes after it starts, with a
// "final" report and "stats: time limit".
// CannonPhysics::Update is hooked to count shots per side and category. At t=60 it logs a census of every object
// with health (team, max health) once.
//
// Zones: 0x58-byte entries at 0x43E5B8 (count 0x43E550): +0x10 type (2 = outpost), +0x14 owner team, +0x28 build
// index (-1 = all built), +0x2C claimed.

#include "cw_mod.h"

#include <windows.h>

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <unordered_map>
#include <vector>

namespace {

constexpr std::uint32_t kPowerupUpdateBody = 0x0007D539;  // Powerup_Update, past the network-client check
constexpr std::uint32_t kBeginMission = 0x00062750;
constexpr std::uint32_t kCannonUpdate = 0x00049540;
constexpr std::uint32_t kHealthComponent = 0xFBCD164A;
constexpr std::uint32_t kWorld = 0x003A2F0C;
constexpr std::uint32_t kZoneCount = 0x0043E550;
constexpr std::uint32_t kZones = 0x0043E5B8;

enum Category { kPlayer, kTroop, kTurret, kHq, kGenerator, kOther, kCategories };
const char* const kNames[kCategories] = {"player", "troop", "turret", "hq", "gen", "other"};

struct Tracked {
    int side;
    int category;
    int slot;  // player slot for kPlayer (0 = player 1, the idle observer in balance runs), else -1
    float health;
    float maxHealth;
    bool seen;
};

struct Totals {
    int alive[kCategories];
    float health[kCategories];
    float damage[kCategories];  // damage taken
    int deaths[kCategories];
    int shots[kCategories];     // fired by this category
};

const CwModApi* g_api = nullptr;
void(__cdecl* g_originalBeginMission)(const char*, const char*) = nullptr;
void(__fastcall* g_originalCannon)(std::uint8_t*, void*, float) = nullptr;
bool g_conquest = false;
std::unordered_map<int, Tracked> g_tracked;
Totals g_totals[3];  // [side]
double g_nextScan = 0.0;
double g_nextReport = 0.0;
int g_winner = 0;
volatile double g_lastScan = 0.0;  // game time of the last scan, 0 before the match
constexpr int kEndScreen = 18;
char g_mission[32] = {};

double gameTime() {
    auto* world = *reinterpret_cast<std::uint8_t**>(kWorld);
    return world != nullptr ? *reinterpret_cast<const float*>(world + 0x28) : 0.0;
}

std::uint8_t* player(int slot) {
    auto* world = *reinterpret_cast<std::uint8_t**>(kWorld);
    auto** players = world != nullptr ? *reinterpret_cast<std::uint8_t***>(world + 0x14) : nullptr;
    return players != nullptr ? players[slot] : nullptr;
}

int sideOf(int team) {
    const int side = team > 10 ? team - 10 : team;
    return side == 1 || side == 2 ? side : 0;
}

const float* healthBlock(const std::uint8_t* object) {
    auto getComponent = reinterpret_cast<const float*(__fastcall*)(const void*, void*, std::uint32_t)>(
        (*reinterpret_cast<void* const* const*>(object))[16]);
    return getComponent(object, nullptr, kHealthComponent);
}

int categorize(const std::uint8_t* object, int team, float maxHealth) {
    for (int slot = 0; slot < 8; ++slot) {
        if (object == player(slot)) {
            return kPlayer;
        }
    }
    if (team > 10 && maxHealth < 20000.0f) {
        return kTroop;
    }
    // Balance variants change these hulls; CW_STATS_TURRET_HULLS / CW_STATS_HQ_HULL say what they are.
    static const std::vector<float> turretHulls = [] {
        std::vector<float> hulls;
        const char* value = std::getenv("CW_STATS_TURRET_HULLS");
        const char* p = value != nullptr ? value : "750,800";
        while (*p != 0) {
            char* end = nullptr;
            const float hull = std::strtof(p, &end);
            if (end == p) {
                break;
            }
            hulls.push_back(hull);
            p = *end == ',' ? end + 1 : end;
        }
        return hulls;
    }();
    static const float hqHull = [] {
        const char* value = std::getenv("CW_STATS_HQ_HULL");
        return value != nullptr ? std::strtof(value, nullptr) : 6000.0f;
    }();
    for (float hull : turretHulls) {
        if (maxHealth == hull) {
            return kTurret;
        }
    }
    if (maxHealth == hqHull) {
        return kHq;
    }
    if (maxHealth == 2000.0f) {
        return kGenerator;
    }
    return kOther;
}

void zoneCounts(int* held, int* built) {
    const int count = *reinterpret_cast<const std::int32_t*>(kZoneCount);
    for (int index = 0; index < count && index < 20; ++index) {
        const std::uint8_t* zone = reinterpret_cast<const std::uint8_t*>(kZones + index * 0x58);
        const int side = sideOf(*reinterpret_cast<const std::int32_t*>(zone + 0x14));
        if (*reinterpret_cast<const std::int32_t*>(zone + 0x10) == 2 && side != 0 && zone[0x2C] != 0) {
            ++held[side];
            if (*reinterpret_cast<const std::int32_t*>(zone + 0x28) < 0) {
                ++built[side];
            }
        }
    }
}

void report(double now, const char* label) {
    int held[3] = {};
    int built[3] = {};
    zoneCounts(held, built);
    for (int side = 1; side <= 2; ++side) {
        const Totals& t = g_totals[side];
        char line[1024];
        int length = std::snprintf(line, sizeof(line), "stats: %s t=%.0f side %d outposts %d built %d |", label, now, side, held[side], built[side]);
        for (int c = 0; c < kCategories; ++c) {
            length += std::snprintf(line + length, sizeof(line) - length, " %s alive %d hp %.0f dmg %.0f deaths %d shots %d |", kNames[c],
                t.alive[c], t.health[c], t.damage[c], t.deaths[c], t.shots[c]);
        }
        g_api->log("%s", line);
    }
}

void scan(double now, bool census) {
    auto* world = *reinterpret_cast<std::uint8_t**>(kWorld);
    auto* tree = world != nullptr ? *reinterpret_cast<std::uint8_t**>(world + 0x18) : nullptr;
    if (tree == nullptr) {
        return;
    }
    for (auto& entry : g_tracked) {
        entry.second.seen = false;
    }
    for (int side = 1; side <= 2; ++side) {
        std::memset(g_totals[side].alive, 0, sizeof(g_totals[side].alive));
        std::memset(g_totals[side].health, 0, sizeof(g_totals[side].health));
    }
    std::vector<const std::uint8_t*> pending;
    if (auto* root = *reinterpret_cast<const std::uint8_t* const*>(tree + 0x08)) {
        pending.push_back(root);
    }
    while (!pending.empty() && pending.size() < 100000) {
        const std::uint8_t* node = pending.back();
        pending.pop_back();
        constexpr std::uint32_t kChildren[] = {0x08, 0x0C};
        for (std::uint32_t child : kChildren) {
            if (auto* next = *reinterpret_cast<const std::uint8_t* const*>(node + child)) {
                pending.push_back(next);
            }
        }
        const std::uint8_t* object = *reinterpret_cast<const std::uint8_t* const*>(node + 0x18);
        if (object == nullptr || ((*reinterpret_cast<const std::uint32_t*>(object + 0x04) >> 8) & 1) == 0) {
            continue;
        }
        const int team = *reinterpret_cast<const std::int32_t*>(object + 0xC0);
        const int side = sideOf(team);
        const float* block = healthBlock(object);
        if (block == nullptr || block[0x18 / 4] <= 0.0f) {
            continue;
        }
        if (census) {
            g_api->log("stats: census team %d max %.0f hp %.0f", team, block[0x18 / 4], block[0x14 / 4]);
        }
        if (side == 0) {
            continue;
        }
        const float health = block[0x14 / 4];
        const float maxHealth = block[0x18 / 4];
        const int handle = *reinterpret_cast<const std::int32_t*>(node + 0x14);
        auto found = g_tracked.find(handle);
        if (found == g_tracked.end()) {
            if (health <= 0.0f) {
                continue;  // a wreck still in the world: already counted
            }
            int slot = -1;
            for (int candidate = 0; candidate < 8; ++candidate) {
                if (object == player(candidate)) {
                    slot = candidate;
                }
            }
            found = g_tracked.emplace(handle, Tracked{side, categorize(object, team, maxHealth), slot, health, maxHealth, false}).first;
        }
        Tracked& tracked = found->second;
        tracked.seen = true;
        if (health < tracked.health) {
            g_totals[tracked.side].damage[tracked.category] += tracked.health - health;
        }
        tracked.health = health;
        if (health > 0.0f) {
            ++g_totals[side].alive[tracked.category];
            g_totals[side].health[tracked.category] += health;
        }
    }
    for (auto it = g_tracked.begin(); it != g_tracked.end();) {
        Tracked& tracked = it->second;
        if (!tracked.seen || tracked.health <= 0.0f) {
            if (!tracked.seen && tracked.health > 0.0f) {
                g_totals[tracked.side].damage[tracked.category] += tracked.health;
            }
            ++g_totals[tracked.side].deaths[tracked.category];
            if (tracked.category == kHq && g_winner == 0) {
                g_winner = 3 - tracked.side;
                g_api->log("stats: winner side %d at t=%.0f (%s)", g_winner, now, g_mission);
            }
            if (tracked.category == kPlayer) {
                g_api->log("stats: t=%.0f side %d player destroyed (slot %d)", now, tracked.side, tracked.slot);
            } else if (tracked.category == kHq || tracked.category == kGenerator) {
                g_api->log("stats: t=%.0f side %d %s destroyed", now, tracked.side, kNames[tracked.category]);
            }
            it = g_tracked.erase(it);
        } else {
            ++it;
        }
    }
}

void __cdecl tick(CwRegisters*) {
    if (!g_conquest) {
        return;
    }
    const double now = gameTime();
    if (now < g_nextScan) {
        return;
    }
    g_nextScan = now + 1.0;
    g_lastScan = now;
    static bool censusDone = false;
    scan(now, !censusDone && now > 60.0);
    censusDone = censusDone || now > 60.0;
    if (now >= g_nextReport && g_winner == 0) {
        g_nextReport = now + 30.0;
        report(now, "tick");
    }
    static const double limit = [] {
        const char* value = std::getenv("CW_STATS_MINUTES");
        return value != nullptr ? std::atof(value) * 60.0 : 0.0;
    }();
    if (limit > 0.0 && now >= limit) {
        g_api->log("stats: time limit at t=%.0f (%s)", now, g_mission);
        report(now, "final");
        g_api->log("stats: match over; exiting");
        ExitProcess(0);
    }
}

// The end screen ("Conquest battle is over", screen 18) stops the game clock and Powerup_Update, so a thread watches
// for it: it writes the final report (the winner is the side whose HQ is still standing) and, with CW_STATS_EXIT=1,
// ends the process.
DWORD WINAPI watchEnd(LPVOID) {
    for (;;) {
        Sleep(100);
        const int screen = *reinterpret_cast<volatile std::int32_t*>(0x0038F7A8);
        if (!g_conquest || g_lastScan <= 0.0 || screen != kEndScreen) {
            continue;
        }
        Sleep(500);  // let a last scan land if the clock is still running
        if (g_winner == 0) {
            const bool standing[3] = {false, g_totals[1].alive[kHq] > 0, g_totals[2].alive[kHq] > 0};
            g_winner = standing[1] != standing[2] ? (standing[1] ? 1 : 2) : 0;
            g_api->log("stats: winner side %d at t=%.0f (%s; end screen)", g_winner, g_lastScan, g_mission);
        }
        report(g_lastScan, "final");
        const char* exitSetting = std::getenv("CW_STATS_EXIT");
        if (exitSetting != nullptr && exitSetting[0] == '1') {
            g_api->log("stats: match over; exiting");
            ExitProcess(0);
        }
        g_conquest = false;
    }
}

void __fastcall cannonUpdate(std::uint8_t* cannon, void* edx, float dt) {
    const float before = *reinterpret_cast<float*>(cannon + 0x1FC);
    g_originalCannon(cannon, edx, dt);
    if (!g_conquest || *reinterpret_cast<float*>(cannon + 0x1FC) >= before) {
        return;
    }
    const std::uint8_t* owner = *reinterpret_cast<std::uint8_t* const*>(cannon + 0x1C);
    if (owner == nullptr) {
        return;
    }
    const int team = *reinterpret_cast<const std::int32_t*>(owner + 0xC0);
    const int side = sideOf(team);
    if (side == 0) {
        return;
    }
    const float* block = healthBlock(owner);
    const float maxHealth = block != nullptr ? block[0x18 / 4] : 0.0f;
    ++g_totals[side].shots[categorize(owner, team, maxHealth)];
}

void __cdecl beginMission(const char* mission, const char* directory) {
    g_originalBeginMission(mission, directory);
    static const char* const kMaps[] = {"multi6.", "multi8.", "multi10.", "multi12."};
    g_conquest = false;
    for (const char* map : kMaps) {
        if (mission != nullptr && _strnicmp(mission, map, std::strlen(map)) == 0) {
            g_conquest = true;
        }
    }
    std::strncpy(g_mission, mission != nullptr ? mission : "", sizeof(g_mission) - 1);
    g_tracked.clear();
    std::memset(g_totals, 0, sizeof(g_totals));
    g_nextScan = g_nextReport = 0.0;
    g_winner = 0;
    g_lastScan = 0.0;
    if (g_conquest) {
        g_api->log("stats: conquest match on %s", g_mission);
    }
}

} // namespace

extern "C" __declspec(dllexport) int __cdecl CwModInit(const CwModApi* api) {
    if (api->version != CW_MOD_API_VERSION) {
        return 0;
    }
    g_api = api;
    CreateThread(nullptr, 0, &watchEnd, nullptr, 0, nullptr);
    return api->detour(kBeginMission, reinterpret_cast<const void*>(&beginMission), reinterpret_cast<void**>(&g_originalBeginMission),
               "Batch_BeginMission (conquest_stats)")
        && api->midHook(kPowerupUpdateBody, &tick, "Powerup_Update (conquest_stats)")
        && api->detour(kCannonUpdate, reinterpret_cast<const void*>(&cannonUpdate), reinterpret_cast<void**>(&g_originalCannon),
               "CannonPhysics::Update (conquest_stats)");
}
