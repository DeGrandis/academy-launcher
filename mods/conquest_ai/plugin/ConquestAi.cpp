// A computer-driven tank for team 2 in Conquest, so the mode can be played alone (with mp_solo).
//
// It spawns at team 2's base and boosts to the next outpost (factory zone): first any team 2 outpost with something
// left to build (for example a destroyed turret), then the nearest unclaimed one, then team 1's. It keeps moving
// closer until the zone counts it as the claimer, stays until team 2 owns it and everything there is built, then moves
// on. Like a player pressing d-pad up, it orders the units its outposts build to attack: the player controller's
// squad command (0xB0C80) calls Conquest_SquadCommand (0x91740, __cdecl(int playerSlot, int team, bool attack,
// bool breakAndAttack, bool regroup, bool hold)), which puts every outpost the team owns into that mode (zone +0x48,
// 2 = attack) and orders its units; the tank calls it as player 2 whenever one of its outposts is not attacking.
// It respawns at the base 10 s after it is destroyed.
//
// Conquest zones are a table of 0x58-byte entries at 0x43E5B8 (count at 0x43E550): +0x00 position, +0x0C radius,
// +0x10 type (2 = factory zone), +0x14 owning team (-1 = nobody), +0x20 id, +0x24 build timer, +0x28 index of the
// turret/unit being built (-1 when all are built), +0x2C claimed (the factory exists).
// Zone_UpdateClaim (0x8F6C0, int __cdecl(int zone, bool playersOnly)) is called in players-only mode: it looks at
// the 8 player slots ([[0x3A2F0C]+0x14][slot]) and counts objects whose owning player (+0xC8) is set. While it
// checks a factory zone, the tank is put in the empty player 2 slot (and given owner 1), then taken out again.
// The tank spawns at one of the map's team 2 spawn points (table of 0x20-byte records at 0x41BA50, count at 0x41B870:
// rotation, position, team). Movement uses the game's own "go to position" order (0x713F0, behind Script_Goto);
// a move order counts as done some way short of its point, so inside the circle the tank is sent past the centre
// until the zone registers it. Zone +0x18 is the player slot currently claiming it.
// Routes: driving straight at an outpost takes the tank off bridges, so it follows the map's own path between team
// 2's base and that outpost (Fac<id>_2, the route the game's units use), from the waypoint nearest to it, until it
// reaches the circle. Paths come from the path manager ([[0x3A2F0C]+0x1C], Find at 0xAD320); a path's points are a
// list from +0x18, each node: +0x08 next, +0x10 position.
// Boost: hover physics control 7 sets +0x142; HoverPhysics' update reads the queued controls and then (0x3B3B8)
// applies them, so a mid hook there turns boost on for the tank while it travels.

#include "cw_mod.h"
#include "game/GameSymbols.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iterator>

namespace {

namespace game = cw::game;

constexpr std::uint32_t kZoneCount = 0x0043E550;
constexpr std::uint32_t kZones = 0x0043E5B8;
constexpr std::uint32_t kZoneSize = 0x58;
constexpr int kMaxZones = 20;
constexpr std::int32_t kFactoryZone = 2;
constexpr std::uint32_t kGotoPosition = 0x000713F0;  // edi = const float position[3]; __cdecl(int handle, 0, 0, 0, 0)
constexpr std::uint32_t kSpawnPointCount = 0x0041B870;
constexpr std::uint32_t kSpawnPoints = 0x0041BA50;
constexpr std::uint32_t kObjectOwnerPlayer = 0xC8;
constexpr std::int32_t kOwnerPlayer = 1;  // player 2's slot

// Script_BuildObject: odf, rotation quaternion (x, y, z, w), position, label, team, -1, -1, -1; returns a handle.
const auto BuildObject = reinterpret_cast<int(__cdecl*)(const char*, const float*, const float*, const char*, int, int, int, int)>(0x0006FE10);

struct SpawnPoint {
    float rotation[4];
    float position[3];
    std::int32_t team;
};

constexpr int kTeam = 2;
constexpr char kUnitOdf[] = "CIS_tank_fighter_multi";
constexpr float kFirstSpawnDelay = 5.0f;
constexpr float kRespawnDelay = 10.0f;
constexpr float kReorderInterval = 3.0f;   // repeat the move order until the zone counts the tank as its claimer
constexpr float kOvershootFraction = 0.6f;  // move target: this fraction of the radius past the centre
// The outpost building blocks most sides of the circle. When the tank gets no closer for kStuckSeconds it drives
// round to the next side (kSideStep further round, kSideRing x the radius out) and tries through the centre from there.
constexpr float kStuckSeconds = 6.0f;
constexpr float kStuckProgress = 5.0f;
constexpr float kSideStep = 1.0471976f;  // 60 degrees
constexpr float kSideRing = 1.4f;
constexpr float kTroopOrderInterval = 2.0f;  // how often to check team 2's outposts for changes
constexpr float kTroopRepeat = 30.0f;        // repeat the attack command this often anyway
constexpr int kMaxHandle = 4096;             // object handles are small sequential numbers
constexpr std::uint32_t kObjectTeam = 0xC0;
// Physics vtables of units that drive (troops); turrets and buildings are left alone.
constexpr std::uint32_t kMobilePhysics[] = {0x00338A40 /*hover*/, 0x003395C8 /*tread*/, 0x00339BF8 /*walker*/,
    0x00338E00 /*jedi*/, 0x003383D0 /*flyer*/};
constexpr float kWaypointReached = 20.0f;    // distance at which a route waypoint counts as reached
constexpr float kBoostMinimumLeg = 100.0f;   // only boost on long legs, not near bends and bridge ends
constexpr int kMaxRoutePoints = 64;
constexpr std::uint32_t kPathFind = 0x000AD320;  // void* __thiscall(PathManager*, const char* name)

constexpr std::uint32_t kHoverControlsApplied = 0x0003B3B8;  // esi = HoverPhysics
constexpr std::uint32_t kHoverBoost = 0x142;
constexpr std::uint32_t kObjectPhysics = 0x20;
const auto ConquestSquadCommand = reinterpret_cast<void(__cdecl*)(int, int, int, int, int, int)>(0x00091740);

const char* const kConquestMaps[] = {"multi6", "multi8", "multi10", "multi12"};

struct Zone {
    float position[3];
    float radius;
    std::int32_t type;
    std::int32_t owner;
    std::int32_t claimer;  // player slot standing in the zone, -1 = none
    std::uint8_t unknown1C[4];
    std::int32_t id;
    float buildTimer;
    std::int32_t building;  // turret/unit being built; -1 when everything is built
    std::uint8_t claimed;
};

constexpr std::uint32_t kZoneUpdateClaim = 0x0008F6C0;
constexpr int kPlayerSlot = 1;

const CwModApi* g_api = nullptr;
void(__cdecl* g_originalBeginMission)(const char*, const char*) = nullptr;
int(__cdecl* g_originalUpdateClaim)(int, int) = nullptr;
void(__cdecl* g_originalUpdate)(float) = nullptr;

bool g_active = false;
int g_unit = 0;
int g_target = -1;          // zone index the unit is heading to or holding
float g_spawnAt = 0.0f;     // game time to (re)spawn the unit
float g_orderedAt = -1.0f;  // game time of the last move order
int g_lastBuilding = -2;    // target zone's build index last logged
bool g_inPlace = false;     // the zone counts the tank as its claimer
bool g_boost = false;       // hold boost (while travelling)
std::uint8_t* g_physics = nullptr;  // the tank's physics, for the boost hook
float g_troopsOrderedAt = -1.0f;
bool g_troopsOrderedThisMatch = false;
float g_route[kMaxRoutePoints][3];  // waypoints to the target zone, outpost end last
int g_routeLength = 0;
int g_routeIndex = 0;
float g_bestDistance = 0.0f;  // closest the tank has got to the zone centre since the last change of approach
float g_progressAt = 0.0f;
bool g_sideMove = false;      // driving round the outpost to another side
float g_side[3];
float g_sideAngle = 0.0f;
int g_sideTries = 0;

Zone* zone(int index) {
    return reinterpret_cast<Zone*>(kZones + index * kZoneSize);
}

int zoneCount() {
    const int count = *reinterpret_cast<const std::int32_t*>(kZoneCount);
    return count < 0 ? 0 : (count > kMaxZones ? kMaxZones : count);
}

// World position: +0x70 is relative to the object's parent node, so use the game's own getter (0x22F5A0,
// float* __thiscall(Node*, float out[3], Node* relativeTo), which the zone claim check uses too).
const float* unitPosition() {
    static float position[3];
    auto* object = game::Script_GetPhysicsObject(g_unit);
    if (object == nullptr) {
        return nullptr;
    }
    const auto worldPosition = reinterpret_cast<float*(__fastcall*)(void*, void*, float*, void*)>(0x0022F5A0);
    worldPosition(object, nullptr, position, nullptr);
    return position;
}

void gotoPosition(int handle, const float* position) {
    __asm {
        mov edi, position
        push 0
        push 0
        push 0
        push 0
        push handle
        mov eax, kGotoPosition
        call eax
        add esp, 0x14
    }
}

// Team 2 owns it and has finished building there.
bool held(const Zone* target) {
    return target->owner == kTeam && target->claimed != 0 && target->building < 0;
}

float distance2d(const float* a, const float* b) {
    const float dx = a[0] - b[0];
    const float dz = a[2] - b[2];
    return std::sqrt(dx * dx + dz * dz);
}

// 0 = team 2's but something is left to build (rebuild), 1 = nobody's, 2 = team 1's, -1 = done.
int priority(const Zone* candidate) {
    if (candidate->type != kFactoryZone || held(candidate)) {
        return -1;
    }
    return candidate->owner == kTeam ? 0 : (candidate->owner < 0 ? 1 : 2);
}

// The most urgent factory zone, nearest first; -1 when team 2 holds them all.
int chooseTarget(const float* from) {
    int best = -1;
    int bestPriority = 0;
    float bestDistance = 0.0f;
    for (int index = 0; index < zoneCount(); ++index) {
        const int rank = priority(zone(index));
        if (rank < 0) {
            continue;
        }
        const float distance = distance2d(zone(index)->position, from);
        if (best < 0 || rank < bestPriority || (rank == bestPriority && distance < bestDistance)) {
            best = index;
            bestPriority = rank;
            bestDistance = distance;
        }
    }
    return best;
}

const std::uint8_t* findPath(const char* name) {
    auto* world = *reinterpret_cast<std::uint8_t**>(0x003A2F0C);
    auto* manager = world != nullptr ? *reinterpret_cast<void**>(world + 0x1C) : nullptr;
    if (manager == nullptr) {
        return nullptr;
    }
    const auto find = reinterpret_cast<const std::uint8_t*(__fastcall*)(void*, void*, const char*)>(kPathFind);
    return find(manager, nullptr, name);
}

// Loads Fac<id>_2 for the target zone into g_route (outpost end last) and starts at the waypoint nearest the tank.
void planRoute(const Zone* target, const float* from) {
    g_routeLength = 0;
    g_routeIndex = 0;
    char name[16];
    std::snprintf(name, sizeof(name), "Fac%d_2", target->id);
    const std::uint8_t* path = findPath(name);
    if (path == nullptr) {
        g_api->log("conquest_ai: no path %s; driving straight", name);
        return;
    }
    for (auto* node = *reinterpret_cast<const std::uint8_t* const*>(path + 0x18); node != nullptr && g_routeLength < kMaxRoutePoints;
         node = *reinterpret_cast<const std::uint8_t* const*>(node + 0x08)) {
        std::memcpy(g_route[g_routeLength++], node + 0x10, sizeof(float) * 3);
    }
    if (g_routeLength == 0) {
        return;
    }
    if (distance2d(g_route[0], target->position) < distance2d(g_route[g_routeLength - 1], target->position)) {
        for (int low = 0, high = g_routeLength - 1; low < high; ++low, --high) {
            float swap[3];
            std::memcpy(swap, g_route[low], sizeof(swap));
            std::memcpy(g_route[low], g_route[high], sizeof(swap));
            std::memcpy(g_route[high], swap, sizeof(swap));
        }
    }
    float nearest = -1.0f;
    for (int index = 0; index < g_routeLength; ++index) {
        const float distance = distance2d(g_route[index], from);
        if (nearest < 0.0f || distance < nearest) {
            nearest = distance;
            g_routeIndex = index;
        }
    }
    g_api->log("conquest_ai: following %s (%d points, joining at %d)", name, g_routeLength, g_routeIndex);
}

bool isMobile(const std::uint8_t* object) {
    const auto* physics = *reinterpret_cast<const std::uint8_t* const*>(object + kObjectPhysics);
    if (physics == nullptr) {
        return false;
    }
    const std::uint32_t vtable = *reinterpret_cast<const std::uint32_t*>(physics);
    for (const std::uint32_t mobile : kMobilePhysics) {
        if (vtable == mobile) {
            return true;
        }
    }
    return false;
}

// Moves every team 2 unit that can drive (not turrets or buildings, not the tank) towards team 1's HQ
// (a team 1 spawn point); returns how many were ordered.
int sendTroopsToEnemyHq() {
    const float* hq = nullptr;
    const int spawnCount = *reinterpret_cast<const std::int32_t*>(kSpawnPointCount);
    for (int index = 0; index < spawnCount && hq == nullptr; ++index) {
        const auto* spawn = reinterpret_cast<const SpawnPoint*>(kSpawnPoints) + index;
        if (spawn->team == 1) {
            hq = spawn->position;
        }
    }
    if (hq == nullptr) {
        return 0;
    }
    int sent = 0;
    for (int handle = 1; handle < kMaxHandle; ++handle) {
        if (handle == g_unit || !game::Script_IsAlive(handle)) {
            continue;
        }
        const auto* object = static_cast<const std::uint8_t*>(game::Script_GetPhysicsObject(handle));
        if (object != nullptr && *reinterpret_cast<const std::int32_t*>(object + kObjectTeam) == kTeam && isMobile(object)) {
            gotoPosition(handle, hq);
            ++sent;
        }
    }
    return sent;
}

// D-pad up for player 2. Zone +0x48 already reads 2 (attack) from the start, but the units only move once the
// command is given, so it is given whenever team 2's outposts change (one claimed, a turret or unit built) and every
// kTroopRepeat seconds.
void orderTroops(float now) {
    static int lastSignature = 0;
    static float lastOrderAt = -1.0f;
    int signature = 0;
    for (int index = 0; index < zoneCount(); ++index) {
        const Zone* candidate = zone(index);
        if (candidate->type == kFactoryZone && candidate->owner == kTeam && candidate->claimed != 0) {
            signature = signature * 31 + index * 100 + candidate->building + 2;
        }
    }
    // The first call of a match always gives the command, like a player pressing d-pad up as the match starts.
    if (g_troopsOrderedThisMatch &&
        (signature == 0 || (signature == lastSignature && lastOrderAt >= 0.0f && now - lastOrderAt < kTroopRepeat))) {
        return;
    }
    g_troopsOrderedThisMatch = true;
    lastSignature = signature;
    lastOrderAt = now;
    // The command reaches team 2's outpost squads (zone +0x30/+0x34, team 12) and gives them a path order and an
    // attack order, but without a real player 2 their units stay put; so the units are also sent at team 1's HQ.
    ConquestSquadCommand(kPlayerSlot, kTeam, 1, 0, 0, 0);
    const int sent = sendTroopsToEnemyHq();
    g_api->log("conquest_ai: squad command attack (d-pad up); %d team %d units sent at team 1's HQ", sent, kTeam);
}

void __cdecl hoverControlsApplied(CwRegisters* registers) {
    auto* physics = reinterpret_cast<std::uint8_t*>(static_cast<std::uintptr_t>(registers->esi));
    if (g_boost && physics == g_physics) {
        physics[kHoverBoost] = 1;
    }
}

void spawnUnit() {
    const int count = *reinterpret_cast<const std::int32_t*>(kSpawnPointCount);
    static int next = 0;
    for (int tries = 0; tries < count; ++tries) {
        const auto* spawn = reinterpret_cast<const SpawnPoint*>(kSpawnPoints) + (next++ % count);
        if (spawn->team != kTeam) {
            continue;
        }
        g_unit = BuildObject(kUnitOdf, spawn->rotation, spawn->position, "conquest_ai", kTeam, -1, -1, -1);
        if (auto* object = static_cast<std::uint8_t*>(game::Script_GetPhysicsObject(g_unit))) {
            *reinterpret_cast<std::int32_t*>(object + kObjectOwnerPlayer) = kOwnerPlayer;
        }
        g_target = -1;
        g_orderedAt = -1.0f;
        g_api->log("conquest_ai: spawned %s at (%.0f, %.0f, %.0f) (handle %d)", kUnitOdf, spawn->position[0], spawn->position[1],
            spawn->position[2], g_unit);
        return;
    }
    g_api->log("conquest_ai: no team %d spawn point on this map", kTeam);
    g_spawnAt = game::Script_GetTime() + kRespawnDelay;
}

void __cdecl update(float dt) {
    g_originalUpdate(dt);
    if (!g_active || !game::Mp_IsRunning()) {
        return;
    }
    const float now = game::Script_GetTime();
    if (g_unit == 0 || !game::Script_IsAlive(g_unit)) {
        g_boost = false;
        g_physics = nullptr;
        g_inPlace = false;
        if (g_unit != 0) {
            g_api->log("conquest_ai: unit destroyed; respawning in %.0f s", kRespawnDelay);
            g_unit = 0;
            g_spawnAt = now + kRespawnDelay;
        }
        if (now >= g_spawnAt && zoneCount() > 0) {
            spawnUnit();
        }
        return;
    }
    auto* object = static_cast<std::uint8_t*>(game::Script_GetPhysicsObject(g_unit));
    const float* position = unitPosition();
    if (object == nullptr || position == nullptr) {
        return;
    }
    auto& owner = *reinterpret_cast<std::int32_t*>(object + kObjectOwnerPlayer);
    if (owner != kOwnerPlayer) {
        owner = kOwnerPlayer;
    }
    g_physics = *reinterpret_cast<std::uint8_t**>(object + kObjectPhysics);
    if (g_troopsOrderedAt < 0.0f || now - g_troopsOrderedAt >= kTroopOrderInterval) {
        g_troopsOrderedAt = now;
        orderTroops(now);
    }
    if (g_target >= 0 && held(zone(g_target))) {
        g_api->log("conquest_ai: zone %d claimed and fully built", zone(g_target)->id);
        g_target = -1;
    }
    if (g_target >= 0 && !g_inPlace) {
        // While travelling, switch to a more urgent zone (for example one of ours losing a turret).
        const int better = chooseTarget(position);
        if (better >= 0 && better != g_target && priority(zone(better)) < priority(zone(g_target))) {
            g_target = -1;
        }
    }
    if (g_target < 0) {
        g_target = chooseTarget(position);
        g_boost = false;
        if (g_target < 0) {
            return;  // team 2 holds every outpost
        }
        static const char* const kReasons[] = {"rebuild", "unclaimed", "team 1's"};
        g_api->log("conquest_ai: heading for zone %d (%s)", zone(g_target)->id, kReasons[priority(zone(g_target))]);
        g_orderedAt = -1.0f;
        g_lastBuilding = -2;
        g_inPlace = false;
        planRoute(zone(g_target), position);
    }
    const Zone* target = zone(g_target);
    if (target->owner == kTeam && target->claimed != 0 && target->building != g_lastBuilding) {
        g_lastBuilding = target->building;
        g_api->log("conquest_ai: zone %d building item %d", target->id, target->building);
    }
    // In place once the zone counts the tank as its claimer; keep moving in until then.
    const bool counted = target->claimer == kPlayerSlot;
    if (counted != g_inPlace) {
        g_inPlace = counted;
        g_api->log("conquest_ai: %s zone %d", counted ? "holding" : "lost the claim on", target->id);
        if (counted) {
            gotoPosition(g_unit, position);  // stop here
        }
    }
    const float distance = distance2d(position, target->position);
    if (!g_inPlace && distance > target->radius && g_routeIndex < g_routeLength) {
        // On the way: drive the map's route waypoint by waypoint.
        const int previous = g_routeIndex;
        while (g_routeIndex < g_routeLength && distance2d(position, g_route[g_routeIndex]) < kWaypointReached) {
            ++g_routeIndex;
        }
        if (g_routeIndex < g_routeLength) {
            float leg = distance2d(position, g_route[g_routeIndex]);
            if (g_routeIndex != previous || g_orderedAt < 0.0f) {
                g_bestDistance = leg;
                g_progressAt = now;
            } else if (leg < g_bestDistance - kStuckProgress) {
                g_bestDistance = leg;
                g_progressAt = now;
            } else if (now - g_progressAt > kStuckSeconds) {
                g_api->log("conquest_ai: stuck %.0f short of route point %d of %d; skipping it", leg, g_routeIndex + 1, g_routeLength);
                ++g_routeIndex;
                g_progressAt = now;
                g_orderedAt = -1.0f;
                return;
            }
            if (g_routeIndex != previous) {
                g_api->log("conquest_ai: route point %d of %d (%.0f away)", g_routeIndex + 1, g_routeLength, leg);
            }
            g_boost = leg > kBoostMinimumLeg;
            if (g_routeIndex != previous || g_orderedAt < 0.0f || now - g_orderedAt >= kReorderInterval) {
                gotoPosition(g_unit, g_route[g_routeIndex]);
                g_orderedAt = now;
            }
            return;
        }
        g_orderedAt = -1.0f;  // route done: move in right away
    }
    g_boost = false;
    if (g_inPlace) {
        return;
    }
    if (g_orderedAt < 0.0f) {  // just finished the route (or a new target): start measuring progress
        g_bestDistance = distance;
        g_progressAt = now;
        g_sideMove = false;
        g_sideTries = 0;
    }
    if (g_sideMove) {
        const bool arrived = distance2d(position, g_side) < kWaypointReached;
        if (arrived || now - g_progressAt > kStuckSeconds) {
            g_sideMove = false;
            g_bestDistance = distance;
            g_progressAt = now;
            g_orderedAt = 0.0f;  // drive through the centre right away
        } else {
            if (distance2d(position, g_side) < g_bestDistance - kStuckProgress) {
                g_bestDistance = distance2d(position, g_side);
                g_progressAt = now;
            }
            if (now - g_orderedAt >= kReorderInterval) {
                gotoPosition(g_unit, g_side);
                g_orderedAt = now;
            }
            return;
        }
    } else if (distance < g_bestDistance - kStuckProgress) {
        g_bestDistance = distance;
        g_progressAt = now;
    } else if (now - g_progressAt > kStuckSeconds) {
        // Blocked by the building: go round to the next side of the outpost.
        if (g_sideTries++ == 0) {
            g_sideAngle = std::atan2(position[2] - target->position[2], position[0] - target->position[0]);
        }
        const float angle = g_sideAngle + kSideStep * g_sideTries;
        g_side[0] = target->position[0] + std::cos(angle) * target->radius * kSideRing;
        g_side[1] = target->position[1];
        g_side[2] = target->position[2] + std::sin(angle) * target->radius * kSideRing;
        g_sideMove = true;
        g_bestDistance = distance2d(position, g_side);
        g_progressAt = now;
        g_api->log("conquest_ai: tank stuck %s zone %d's claim circle (%.0f from the centre, radius %.0f); driving round to side %d",
            distance > target->radius ? "OUTSIDE" : "inside", target->id, distance, target->radius, g_sideTries);
        gotoPosition(g_unit, g_side);
        g_orderedAt = now;
        return;
    }
    if (g_orderedAt <= 0.0f || now - g_orderedAt >= kReorderInterval) {
        // Aim past the centre (the far side of the outpost from the tank): a move order counts as done some way short
        // of its point, so this carries the tank through the middle. It stops as soon as the zone counts it.
        const float scale = distance > 1.0f ? -target->radius * kOvershootFraction / distance : 0.0f;
        const float destination[3] = {target->position[0] + (position[0] - target->position[0]) * scale, target->position[1],
            target->position[2] + (position[2] - target->position[2]) * scale};
        g_api->log("conquest_ai: tank is %s zone %d's claim circle (%.0f from the centre, radius %.0f, owner %d); ordering it "
                   "through the centre", distance > target->radius ? "OUTSIDE" : "inside", target->id, distance, target->radius,
            target->owner);
        gotoPosition(g_unit, destination);
        g_orderedAt = now;
    }
}

int __cdecl updateClaim(int index, int playersOnly) {
    if (!g_active || g_unit == 0 || index < 0 || index >= zoneCount() || zone(index)->type != kFactoryZone || !game::Script_IsAlive(g_unit)) {
        return g_originalUpdateClaim(index, playersOnly);
    }
    auto* object = static_cast<std::uint8_t*>(game::Script_GetPhysicsObject(g_unit));
    auto* world = *reinterpret_cast<std::uint8_t**>(0x003A2F0C);
    auto** players = world != nullptr ? *reinterpret_cast<std::uint8_t***>(world + 0x14) : nullptr;
    if (object == nullptr || players == nullptr || players[kPlayerSlot] != nullptr) {
        return g_originalUpdateClaim(index, playersOnly);
    }
    *reinterpret_cast<std::int32_t*>(object + kObjectOwnerPlayer) = kOwnerPlayer;
    players[kPlayerSlot] = object;
    const int result = g_originalUpdateClaim(index, playersOnly);
    players[kPlayerSlot] = nullptr;
    return result;
}

void __cdecl beginMission(const char* mission, const char* directory) {
    g_active = false;
    for (const char* map : kConquestMaps) {
        const std::size_t length = std::strlen(map);
        if (mission != nullptr && _strnicmp(mission, map, length) == 0 && mission[length] == '.') {
            g_active = true;
        }
    }
    g_unit = 0;
    g_target = -1;
    g_spawnAt = kFirstSpawnDelay;
    g_boost = false;
    g_physics = nullptr;
    g_inPlace = false;
    g_troopsOrderedAt = -1.0f;
    g_troopsOrderedThisMatch = false;
    g_routeLength = 0;
    g_routeIndex = 0;
    if (g_active) {
        g_api->log("conquest_ai: %s is a Conquest map; team 2 gets a computer tank", mission);
    }
    g_originalBeginMission(mission, directory);
}

} // namespace

extern "C" __declspec(dllexport) int __cdecl CwModInit(const CwModApi* api) {
    if (api->version != CW_MOD_API_VERSION) {
        return 0;
    }
    g_api = api;
    return api->detour(reinterpret_cast<std::uint32_t>(game::Batch_BeginMission), reinterpret_cast<const void*>(&beginMission),
               reinterpret_cast<void**>(&g_originalBeginMission), "Batch_BeginMission (conquest_ai)")
        && api->detour(reinterpret_cast<std::uint32_t>(game::UpdateList_Run), reinterpret_cast<const void*>(&update),
               reinterpret_cast<void**>(&g_originalUpdate), "UpdateList_Run (conquest_ai)")
        && api->detour(kZoneUpdateClaim, reinterpret_cast<const void*>(&updateClaim), reinterpret_cast<void**>(&g_originalUpdateClaim),
               "Zone_UpdateClaim (conquest_ai)")
        && api->midHook(kHoverControlsApplied, &hoverControlsApplied, "HoverPhysics controls applied (conquest_ai boost)");
}
