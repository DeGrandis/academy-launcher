// Conquest comeback and pacing mechanics. Balance simulations (docs/conquest-balance.md) kept only the first: faster
// respawn when behind, on by default at 2. The others are off unless their setting is > 0.
//
//   CW_MECH_RESPAWN=k    Faster respawn when behind (default 2): a dead player's respawn countdown runs
//                        1 + k x (outposts behind, at most 3) times as fast. Players see it on screen: when their
//                        side falls behind, and while they wait to respawn ("BEHIND - FAST RESPAWN x3").
//   CW_MECH_LASTSTAND=k  Last-stand turrets: the trailing side's outpost turrets shrug off a fraction
//                        min(0.75, k x outposts behind) of the damage they take.
//   CW_MECH_BOUNTY=k     Bounty on the leader: every turret, troop or tank the leading side loses gives the trailing
//                        side k seconds of build progress at its outposts that are building (or, if none is, repairs
//                        its most damaged turret by 50 x k).
//   CW_MECH_SUDDEN=m     Sudden death: after m minutes both HQs lose their shields and stop regenerating them.
//
// "Behind" counts claimed outposts (zone table at 0x43E5B8, 0x58 bytes each, count at 0x43E550: +0x10 type 2 =
// outpost, +0x14 owner team, +0x1C seconds into the current build item, +0x28 item index, -1 = all built, +0x2C
// claimed). Sides: team 1/11 Republic, 2/12 CIS (players and buildings / outpost troops).
// Health block (GetComponent 0xFBCD164A): +0x14 hull, +0x18 max hull, +0x1C shield, +0x20 max shield, +0x24 shield
// regeneration per second. Player slots 0x5EC32C + slot x 0x80: +0x04 respawn timer (counts up about 3 s after a
// death, then down from 0; the player respawns at -10).
// On-screen messages use the pickup message the multiplayer HUD draws (Powerup_DrawHud, 0x7C780): per player at
// 0x43ADD8 + slot x 12, +0 the pickup type whose label is shown, +8 when it disappears (ms, clock 0x2306B0). Conquest
// has no "Tag" pickups (type 4), so its label (0x43A888 + 4 x 0x50 + 0x10, up to 31 characters) carries the text.

#include "cw_mod.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <unordered_map>
#include <vector>

namespace {

constexpr std::uint32_t kPowerupUpdate = 0x0007D520;  // every multiplayer frame
constexpr std::uint32_t kBeginMission = 0x00062750;
constexpr std::uint32_t kWorld = 0x003A2F0C;
constexpr std::uint32_t kZoneCount = 0x0043E550;
constexpr std::uint32_t kZones = 0x0043E5B8;
constexpr std::uint32_t kPlayerSlots = 0x005EC32C;
constexpr std::uint32_t kHealthComponent = 0xFBCD164A;
constexpr int kMaxZones = 20;
constexpr double kScanPeriod = 0.1;
constexpr std::uint32_t kMessages = 0x0043ADD8;      // per player 12 bytes: +0 type, +8 hide time (ms)
constexpr std::uint32_t kPickupTypes = 0x0043A888;   // per type 0x50 bytes, +0x10 label (wide)
constexpr std::uint32_t kClockMs = 0x002306B0;       // int __cdecl(): milliseconds
constexpr int kMessageType = 4;                      // "Tag": unused in Conquest

enum Kind { kTurret, kTroop, kTank, kHq, kOther };

struct Tracked {
    int side;
    Kind kind;
    float health;
    float* block;
    bool seen;
};

const CwModApi* g_api = nullptr;
void(__cdecl* g_originalBeginMission)(const char*, const char*) = nullptr;
bool g_conquest = false;
float g_respawn = 0.0f, g_lastStand = 0.0f, g_bounty = 0.0f, g_sudden = 0.0f;
bool g_suddenDone = false;
float g_lastRespawnTimer[8];
int g_lastBehind[3];
double g_nextScan = 0.0;
std::unordered_map<int, Tracked> g_tracked;
// Effect counters, logged every minute ("conquest_mechanics: t=..."), for checking that each mechanic acts.
double g_respawnSaved = 0.0, g_refunded = 0.0, g_nextReport = 60.0;
int g_bounties = 0;

int sideOf(int team) {
    const int side = team > 10 ? team - 10 : team;
    return side == 1 || side == 2 ? side : 0;
}

std::uint8_t* world() {
    return *reinterpret_cast<std::uint8_t**>(kWorld);
}

std::uint8_t* player(int slot) {
    auto** players = world() != nullptr ? *reinterpret_cast<std::uint8_t***>(world() + 0x14) : nullptr;
    return players != nullptr ? players[slot] : nullptr;
}

std::uint8_t* zone(int index) {
    return reinterpret_cast<std::uint8_t*>(kZones + index * 0x58);
}

int zoneCount() {
    return std::min(*reinterpret_cast<const std::int32_t*>(kZoneCount), kMaxZones);
}

void heldBySide(int* held) {
    held[1] = held[2] = 0;
    for (int index = 0; index < zoneCount(); ++index) {
        const std::uint8_t* z = zone(index);
        if (*reinterpret_cast<const std::int32_t*>(z + 0x10) == 2 && z[0x2C] != 0) {
            ++held[sideOf(*reinterpret_cast<const std::int32_t*>(z + 0x14))];
        }
    }
}

float* healthBlock(const std::uint8_t* object) {
    auto getComponent = reinterpret_cast<float*(__fastcall*)(const void*, void*, std::uint32_t)>(
        (*reinterpret_cast<void* const* const*>(object))[16]);
    return getComponent(object, nullptr, kHealthComponent);
}

Kind kindOf(const std::uint8_t* object, int team, float maxHull) {
    for (int slot = 0; slot < 8; ++slot) {
        if (object == player(slot)) {
            return kTank;
        }
    }
    if (team > 10 && maxHull < 20000.0f) {
        return kTroop;
    }
    if (maxHull == 750.0f || maxHull == 800.0f) {
        return kTurret;
    }
    return maxHull == 6000.0f ? kHq : kOther;
}

// Shows `text` to the player in `slot` for `ms` (the multiplayer HUD's pickup message line).
void showMessage(int slot, const wchar_t* text, int ms) {
    auto* label = reinterpret_cast<wchar_t*>(kPickupTypes + kMessageType * 0x50 + 0x10);
    std::wcsncpy(label, text, 31);
    label[31] = 0;
    auto* message = reinterpret_cast<std::uint8_t*>(kMessages + slot * 12);
    message[0] = kMessageType;
    *reinterpret_cast<std::int32_t*>(message + 8) = reinterpret_cast<int(__cdecl*)()>(kClockMs)() + ms;
    static const wchar_t* lastLogged = nullptr;
    if (slot == 0 && ms >= 1000 && lastLogged != text) {  // player 1's "falls behind" notices (not the per-frame countdown)
        lastLogged = text;
        g_api->log("conquest_mechanics: player 1 sees '%ls'", text);
    }
}

// Bounty: build progress at the trailing side's building outposts, else a repair of its most damaged turret.
void payBounty(int side) {
    ++g_bounties;
    bool paid = false;
    for (int index = 0; index < zoneCount(); ++index) {
        std::uint8_t* z = zone(index);
        if (*reinterpret_cast<const std::int32_t*>(z + 0x10) == 2 && z[0x2C] != 0 &&
            sideOf(*reinterpret_cast<const std::int32_t*>(z + 0x14)) == side && *reinterpret_cast<const std::int32_t*>(z + 0x28) >= 0) {
            *reinterpret_cast<float*>(z + 0x1C) += g_bounty;
            paid = true;
        }
    }
    if (paid) {
        return;
    }
    Tracked* worst = nullptr;
    for (auto& entry : g_tracked) {
        Tracked& t = entry.second;
        if (t.side == side && t.kind == kTurret && t.seen && t.health > 0.0f && t.health < t.block[0x18 / 4] &&
            (worst == nullptr || t.health / t.block[0x18 / 4] < worst->health / worst->block[0x18 / 4])) {
            worst = &t;
        }
    }
    if (worst != nullptr) {
        worst->block[0x14 / 4] = std::min(worst->block[0x18 / 4], worst->block[0x14 / 4] + 50.0f * g_bounty);
        worst->health = worst->block[0x14 / 4];
    }
}

void scan(const int* held) {
    auto* tree = *reinterpret_cast<std::uint8_t**>(world() + 0x18);
    if (tree == nullptr) {
        return;
    }
    for (auto& entry : g_tracked) {
        entry.second.seen = false;
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
        float* block = side != 0 ? healthBlock(object) : nullptr;
        if (block == nullptr || block[0x18 / 4] <= 0.0f || block[0x14 / 4] <= 0.0f) {
            continue;
        }
        const int handle = *reinterpret_cast<const std::int32_t*>(node + 0x14);
        auto found = g_tracked.find(handle);
        if (found == g_tracked.end()) {
            found = g_tracked.emplace(handle, Tracked{side, kindOf(object, team, block[0x18 / 4]), block[0x14 / 4], block, false}).first;
        }
        Tracked& t = found->second;
        t.seen = true;
        t.block = block;
        // Last stand: give back part of the damage the trailing side's turrets took since the last scan.
        const int behind = held[3 - side] - held[side];
        if (g_lastStand > 0.0f && t.kind == kTurret && behind > 0 && block[0x14 / 4] < t.health) {
            const float refund = (t.health - block[0x14 / 4]) * std::min(0.75f, g_lastStand * static_cast<float>(behind));
            block[0x14 / 4] = std::min(block[0x18 / 4], block[0x14 / 4] + refund);
            g_refunded += refund;
        }
        t.health = block[0x14 / 4];
    }
    for (auto it = g_tracked.begin(); it != g_tracked.end();) {
        if (it->second.seen) {
            ++it;
            continue;
        }
        // Gone: a loss for its side. Bounty when the leader loses a fighting unit or turret.
        const int side = it->second.side;
        const Kind kind = it->second.kind;
        it = g_tracked.erase(it);
        if (g_bounty > 0.0f && (kind == kTurret || kind == kTroop || kind == kTank) && held[side] > held[3 - side]) {
            payBounty(3 - side);
        }
    }
}

void __cdecl tick(CwRegisters*) {
    if (!g_conquest || world() == nullptr) {
        return;
    }
    const double now = *reinterpret_cast<const float*>(world() + 0x28);
    int held[3] = {};
    heldBySide(held);

    // Respawn: speed up the countdown of dead players on the trailing side, and tell them.
    for (int side = 1; side <= 2 && g_respawn > 0.0f; ++side) {
        const int behind = held[3 - side] - held[side];
        if (behind > 0 && behind > g_lastBehind[side]) {
            for (int slot = 0; slot < 8; ++slot) {
                const std::uint8_t* object = player(slot);
                if (object != nullptr && sideOf(*reinterpret_cast<const std::int32_t*>(object + 0xC0)) == side) {
                    showMessage(slot, L"BEHIND: REINFORCEMENTS FASTER", 3000);
                }
            }
        }
        g_lastBehind[side] = std::max(behind, 0);
    }
    for (int slot = 0; slot < 8; ++slot) {
        auto& timer = *reinterpret_cast<float*>(kPlayerSlots + slot * 0x80 + 0x04);
        const std::uint8_t* object = player(slot);
        const int side = object != nullptr ? sideOf(*reinterpret_cast<const std::int32_t*>(object + 0xC0)) : 0;
        const float moved = timer - g_lastRespawnTimer[slot];
        const int behind = side != 0 ? held[3 - side] - held[side] : 0;
        if (g_respawn > 0.0f && timer != 0.0f && behind > 0 && std::fabs(moved) < 0.5f) {
            const float extra = moved * g_respawn * static_cast<float>(std::min(behind, 3));
            timer += extra;
            g_respawnSaved += std::fabs(extra);
            wchar_t text[32];
            std::swprintf(text, 32, L"BEHIND - FAST RESPAWN x%d", 1 + static_cast<int>(std::lround(g_respawn * std::min(behind, 3))));
            showMessage(slot, text, 500);
        }
        g_lastRespawnTimer[slot] = timer;
    }

    if (now >= g_nextReport) {
        g_nextReport = now + 60.0;
        g_api->log("conquest_mechanics: t=%.0f respawn saved %.1f s, turret damage refunded %.0f, bounties %d", now, g_respawnSaved,
            g_refunded, g_bounties);
    }
    if (now < g_nextScan) {
        return;
    }
    g_nextScan = now + kScanPeriod;
    if (g_lastStand > 0.0f || g_bounty > 0.0f || (g_sudden > 0.0f && !g_suddenDone)) {
        scan(held);
    }
    // Sudden death: HQ shields down for good.
    if (g_sudden > 0.0f && now >= g_sudden * 60.0) {
        for (auto& entry : g_tracked) {
            if (entry.second.kind == kHq && entry.second.seen) {
                entry.second.block[0x1C / 4] = 0.0f;
                entry.second.block[0x24 / 4] = 0.0f;
            }
        }
        if (!g_suddenDone) {
            g_suddenDone = true;
            g_api->log("conquest_mechanics: sudden death at t=%.0f: HQ shields down", now);
        }
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
    g_tracked.clear();
    g_nextScan = 0.0;
    g_suddenDone = false;
    g_respawnSaved = g_refunded = 0.0;
    g_bounties = 0;
    g_nextReport = 60.0;
    std::memset(g_lastRespawnTimer, 0, sizeof(g_lastRespawnTimer));
    std::memset(g_lastBehind, 0, sizeof(g_lastBehind));
    if (g_conquest) {
        g_api->log("conquest_mechanics: respawn %.2f, last stand %.2f, bounty %.2f, sudden death %.1f min on %s", g_respawn, g_lastStand,
            g_bounty, g_sudden, mission);
    }
}

float setting(const char* name, float fallback = 0.0f) {
    const char* value = std::getenv(name);
    return value != nullptr ? static_cast<float>(std::atof(value)) : fallback;
}

} // namespace

extern "C" __declspec(dllexport) int __cdecl CwModInit(const CwModApi* api) {
    if (api->version != CW_MOD_API_VERSION) {
        return 0;
    }
    g_api = api;
    g_respawn = setting("CW_MECH_RESPAWN", 2.0f);
    g_lastStand = setting("CW_MECH_LASTSTAND");
    g_bounty = setting("CW_MECH_BOUNTY");
    g_sudden = setting("CW_MECH_SUDDEN");
    return api->detour(kBeginMission, reinterpret_cast<const void*>(&beginMission), reinterpret_cast<void**>(&g_originalBeginMission),
               "Batch_BeginMission (conquest_mechanics)")
        && api->midHook(kPowerupUpdate, &tick, "Powerup_Update (conquest_mechanics)");
}
