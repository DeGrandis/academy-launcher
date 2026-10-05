// Drives computer players' tanks through their gamepads (Conquest).
//
// Each computer player is a real player: its tank is [[0x3A2F0C]+0x14][slot], and it claims outposts, builds and
// commands troops exactly like a person, because all it does is press buttons. Per player it:
// - presses d-pad up (squad command "attack HQ") as the match starts and whenever its outposts change,
// - picks an outpost: first one of its own with something left to build (rebuild), then the nearest unclaimed one,
//   then the nearest enemy one,
// - drives the map's route between its base and that outpost (Fac<id>_<team>), waypoint by waypoint, then into the
//   claim circle, going round the outpost building when it gets stuck,
// - stops once the outpost counts it as the claimer and waits there until everything is built,
// - pushes the enemy HQ along the map's paths when none of its outposts needs rebuilding and it holds kPushLead more
//   outposts than the enemy (1 more after 8 minutes, none after 14: escalation) or there is nothing left to take; it
//   shoots the HQ first,
// - fights: every 0.5 s it picks the nearest enemy within range (troops, vehicles, turrets; not the invulnerable HQs
//   and generators). It turns to face one that is close (or any, while holding an outpost) and otherwise keeps
//   driving; whenever one is lined up it fires its primary weapon (A), and its secondary (X) every 2.5 s at range.
//   Object +0xC0 team: 1 and 2 for players and buildings, 11 and 12 for the troops each side's outposts build.
// Steering: the left stick points where the tank should go relative to its heading (right = clockwise from above).
//
// Object: +0x40 matrix (rows right, up, front, position), +0xC0 team; world position from 0x22F5A0.
// Conquest zones: 0x58-byte entries at 0x43E5B8 (count 0x43E550): +0x00 position, +0x0C radius, +0x10 type
// (2 = outpost), +0x14 owner team (-1 none), +0x18 player slot claiming it, +0x20 id, +0x28 build index (-1 = all
// built), +0x2C claimed. Paths: [[0x3A2F0C]+0x1C] path manager, Find at 0xAD320; points listed from +0x18 (node
// +0x08 next, +0x10 position). Spawn points: 0x20-byte records at 0x41BA50 (count 0x41B870): rotation, position, team.
// Objects: a search tree of handles at [[0x3A2F0C]+0x18] (root +0x08; node +0x08 lower, +0x0C higher, +0x14 handle,
// +0x18 object), looked up by 0x14340 (__thiscall(tree, handle), null once the object is gone).

#include "AiDriver.h"

#include <windows.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace ai {

namespace {

constexpr int kSlots = 4;
constexpr std::uint32_t kZoneCount = 0x0043E550;
constexpr std::uint32_t kZones = 0x0043E5B8;
constexpr std::uint32_t kZoneSize = 0x58;
constexpr int kMaxZones = 20;
constexpr std::int32_t kFactoryZone = 2;
constexpr std::uint32_t kPathFind = 0x000AD320;
constexpr std::uint32_t kSpawnPointCount = 0x0041B870;
constexpr std::uint32_t kSpawnPoints = 0x0041BA50;
const char* const kConquestMaps[] = {"multi6", "multi8", "multi10", "multi12", "multi19",  // multi19: flat_conquest test map
    "night6", "night8", "night10", "night12",  // mods/night_maps
                                     "cq1", "cq2", "cq3", "cq4", "cq5"};  // online (System Link) Conquest maps

constexpr float kWaypointReached = 20.0f;
constexpr float kStuckSeconds = 6.0f;
constexpr float kStuckProgress = 5.0f;
constexpr float kSideStep = 1.0471976f;  // 60 degrees round the outpost per try
constexpr float kSideRing = 1.4f;        // x the claim radius
constexpr float kOvershootFraction = 0.6f;
constexpr float kTroopRepeat = 30.0f;
constexpr int kMaxRoutePoints = 64;
constexpr float kPi = 3.14159265f;
constexpr float kLookahead = 25.0f;       // steer at the route this far ahead of the tank's closest point on it
constexpr float kPivotAngle = 1.0f;       // radians off course beyond which the tank pivots slowly
constexpr float kPivotThrottle = 0.12f;
constexpr float kRespawnJump = 150.0f;    // a move this big between two frames means the tank respawned
constexpr std::uint32_t kObjectFromHandle = 0x00014340;
constexpr std::uint32_t kScreen = 0x0038F7A8;  // current screen; 4 = playing, 18 = Conquest end screen
constexpr std::int32_t kInGameScreen = 4;
constexpr std::uint32_t kHealthComponent = 0xFBCD164A;
constexpr float kEngageRange = 250.0f;     // enemies this close are shot at
constexpr float kStandRange = 150.0f;      // and this close, the tank stops to face them on its way
constexpr float kFireAngle = 0.2f;         // radians off the hull's heading the target may be to fire
constexpr float kSecondaryRange = 40.0f;   // the secondary weapon only beyond this (splash, missiles)
constexpr float kSecondaryEvery = 2.5f;
constexpr float kRetarget = 0.5f;
constexpr float kInvulnerable = 20000.0f;  // invulnerable buildings have huge health (1,000,000)
constexpr float kHqMinHealth = 2400.0f;    // the HQ is the only destructible building above this (turrets 750-800)
constexpr int kPushLead = 2;               // outposts ahead before pushing the enemy HQ
constexpr float kEscalate1 = 480.0f;       // match seconds after which a lead of 1 is enough
constexpr float kEscalate2 = 840.0f;       // and after which a tie is enough
constexpr float kPushCheck = 5.0f;

struct Zone {
    float position[3];
    float radius;
    std::int32_t type;
    std::int32_t owner;
    std::int32_t claimer;
    std::uint8_t unknown1C[4];
    std::int32_t id;
    float buildTimer;
    std::int32_t building;
    std::uint8_t claimed;
};

struct SpawnPoint {
    float rotation[4];
    float position[3];
    std::int32_t team;
};

struct Brain {
    bool started = false;
    const std::uint8_t* object = nullptr;  // the tank last frame
    float lastPosition[3] = {};
    int target = -1;
    float route[kMaxRoutePoints][3];
    int routeLength = 0;
    int routeIndex = 0;       // waypoint the tank is driving towards
    float segmentStart[3];    // where the current leg started (the previous waypoint, or where it joined)
    bool inPlace = false;
    int lastBuilding = -2;
    // progress / stuck detection
    float best = 0.0f;
    double progressAt = 0.0;
    bool sideMove = false;
    float side[3];
    float sideAngle = 0.0f;
    int sideTries = 0;
    bool approachStarted = false;
    // d-pad up
    int troopSignature = 0;
    double troopOrderedAt = -1.0;
    double pressUpUntil = 0.0;
    // fighting
    int enemy = 0;            // handle of the target, 0 = none
    double retargetAt = 0.0;
    double secondaryAt = 0.0;
    double secondaryUntil = 0.0;
    // pushing the enemy HQ
    bool pushing = false;
    double pushCheckAt = 0.0;
    float hq[3] = {};
    int hqHandle = 0;
};

const CwModApi* g_api = nullptr;
bool g_conquest = false;
Brain g_brains[kSlots];
double g_now = 0.0;  // seconds, game time

std::uint8_t* playerObject(int slot) {
    auto* world = *reinterpret_cast<std::uint8_t**>(0x003A2F0C);
    auto** players = world != nullptr ? *reinterpret_cast<std::uint8_t***>(world + 0x14) : nullptr;
    return players != nullptr ? players[slot] : nullptr;
}

void worldPosition(std::uint8_t* object, float* out) {
    const auto getter = reinterpret_cast<float*(__fastcall*)(void*, void*, float*, void*)>(0x0022F5A0);
    getter(object, nullptr, out, nullptr);
}

Zone* zone(int index) {
    return reinterpret_cast<Zone*>(kZones + index * kZoneSize);
}

int zoneCount() {
    const int count = *reinterpret_cast<const std::int32_t*>(kZoneCount);
    return count < 0 ? 0 : (count > kMaxZones ? kMaxZones : count);
}

float distance2d(const float* a, const float* b) {
    const float dx = a[0] - b[0];
    const float dz = a[2] - b[2];
    return std::sqrt(dx * dx + dz * dz);
}

bool held(const Zone* candidate, int team) {
    return candidate->owner == team && candidate->claimed != 0 && candidate->building < 0;
}

// 0 = ours with something left to build, 1 = nobody's, 2 = the enemy's, -1 = done.
int priority(const Zone* candidate, int team) {
    if (candidate->type != kFactoryZone || held(candidate, team)) {
        return -1;
    }
    return candidate->owner == team ? 0 : (candidate->owner < 0 ? 1 : 2);
}

// Another player on the same team has this outpost covered: a computer teammate is heading there, or an ally
// (a person) is standing in it or claiming it.
// yieldOnly: for the outpost this player is already heading to, only give way to people and lower-numbered computer
// players, so two computer teammates never both back off the same one.
bool takenByAlly(int index, int slot, int team, bool yieldOnly = false) {
    const Zone* candidate = zone(index);
    for (int other = 0; other < kSlots; ++other) {
        std::uint8_t* object = other != slot ? playerObject(other) : nullptr;
        if (object == nullptr || *reinterpret_cast<const std::int32_t*>(object + 0xC0) != team) {
            continue;
        }
        if (g_brains[other].started) {
            if (g_brains[other].target == index && (!yieldOnly || other < slot)) {
                return true;
            }
            continue;
        }
        float position[3];
        worldPosition(object, position);
        if (candidate->claimer == other || distance2d(position, candidate->position) < candidate->radius) {
            return true;
        }
    }
    return false;
}

int chooseTarget(const float* from, int team, int slot, bool shareWithAllies = false) {
    int best = -1;
    int bestPriority = 0;
    float bestDistance = 0.0f;
    for (int index = 0; index < zoneCount(); ++index) {
        const int rank = priority(zone(index), team);
        if (rank < 0 || (!shareWithAllies && takenByAlly(index, slot, team))) {
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

// Loads the map path `name` as the route, ordered to end nearest `goal`, joined at its point nearest `from`.
void planPath(Brain& brain, int slot, const char* name, const float* goal, const float* from) {
    brain.routeLength = 0;
    brain.routeIndex = 0;
    const std::uint8_t* path = findPath(name);
    if (path == nullptr) {
        g_api->log("ai_players: player %d: no path %s; driving straight", slot + 1, name);
        return;
    }
    for (auto* node = *reinterpret_cast<const std::uint8_t* const*>(path + 0x18); node != nullptr && brain.routeLength < kMaxRoutePoints;
         node = *reinterpret_cast<const std::uint8_t* const*>(node + 0x08)) {
        std::memcpy(brain.route[brain.routeLength++], node + 0x10, sizeof(float) * 3);
    }
    if (brain.routeLength == 0) {
        return;
    }
    if (distance2d(brain.route[0], goal) < distance2d(brain.route[brain.routeLength - 1], goal)) {
        for (int low = 0, high = brain.routeLength - 1; low < high; ++low, --high) {
            float swap[3];
            std::memcpy(swap, brain.route[low], sizeof(swap));
            std::memcpy(brain.route[low], brain.route[high], sizeof(swap));
            std::memcpy(brain.route[high], swap, sizeof(swap));
        }
    }
    float nearest = -1.0f;
    for (int index = 0; index < brain.routeLength; ++index) {
        const float distance = distance2d(brain.route[index], from);
        if (nearest < 0.0f || distance < nearest) {
            nearest = distance;
            brain.routeIndex = index;
        }
    }
    std::memcpy(brain.segmentStart, from, sizeof(brain.segmentStart));
    g_api->log("ai_players: player %d following %s (%d points, joining at %d)", slot + 1, name, brain.routeLength, brain.routeIndex + 1);
}

void planRoute(Brain& brain, int slot, const Zone* target, const float* from, int team) {
    char name[16];
    std::snprintf(name, sizeof(name), "Fac%d_%d", target->id, team);
    planPath(brain, slot, name, target->position, from);
}

// Pure pursuit along the route: the point kLookahead further along the route than the tank's closest point on the
// current leg, so it follows the line between waypoints (along bridges) instead of cutting corners. Moves on to the
// next leg once the tank is past the end of this one.
void routeLookahead(Brain& brain, const float* position, float* out) {
    for (;;) {
        const float* a = brain.segmentStart;
        const float* b = brain.route[brain.routeIndex];
        const float dx = b[0] - a[0];
        const float dz = b[2] - a[2];
        const float length = std::sqrt(dx * dx + dz * dz);
        float along = length > 0.01f ? ((position[0] - a[0]) * dx + (position[2] - a[2]) * dz) / length : length;
        along = along < 0.0f ? 0.0f : along;
        if ((along >= length || distance2d(position, b) < kWaypointReached) && brain.routeIndex + 1 < brain.routeLength) {
            std::memcpy(brain.segmentStart, b, sizeof(brain.segmentStart));
            ++brain.routeIndex;
            continue;
        }
        // Walk kLookahead along the route from the closest point, onto later legs if needed.
        float remaining = kLookahead;
        float from = along;
        const float* start = a;
        int index = brain.routeIndex;
        for (;;) {
            const float* end = brain.route[index];
            const float legX = end[0] - start[0];
            const float legZ = end[2] - start[2];
            const float legLength = std::sqrt(legX * legX + legZ * legZ);
            if (from + remaining <= legLength || index + 1 >= brain.routeLength) {
                const float t = legLength > 0.01f ? (from + remaining < legLength ? from + remaining : legLength) / legLength : 1.0f;
                out[0] = start[0] + legX * t;
                out[1] = start[1] + (end[1] - start[1]) * t;
                out[2] = start[2] + legZ * t;
                return;
            }
            remaining -= legLength - from;
            from = 0.0f;
            start = end;
            ++index;
        }
    }
}

// How far `point` is off the tank's heading, in radians (positive = clockwise from above).
float headingError(const std::uint8_t* object, const float* position, const float* point) {
    const float* front = reinterpret_cast<const float*>(object + 0x60);
    const float heading = std::atan2(front[0], front[2]);
    const float wanted = std::atan2(point[0] - position[0], point[2] - position[2]);
    float error = wanted - heading;
    while (error > kPi) error -= 2 * kPi;
    while (error < -kPi) error += 2 * kPi;
    return error;
}

// Left stick towards `point`, relative to the tank's heading.
void steer(CwPad* pad, const std::uint8_t* object, const float* position, const float* point) {
    const float error = headingError(object, position, point);
    // Slow down to turn: full throttle only when lined up, a crawl while pivoting, so it doesn't arc off bridges.
    const float x = std::fabs(error) > kPi / 2 ? (error > 0 ? 1.0f : -1.0f) : std::sin(error);
    const float lined = std::cos(error) > 0.0f ? std::cos(error) : 0.0f;
    const float y = std::fabs(error) > kPivotAngle ? kPivotThrottle : lined * lined;
    pad->thumbLX = static_cast<std::int16_t>(x * 32767.0f);
    pad->thumbLY = static_cast<std::int16_t>(y * 32767.0f);
}

// D-pad up whenever this team's outposts change (one claimed, a turret or unit built), at the start, and every 30 s.
void troopCommands(Brain& brain, int slot, int team, CwPad* pad) {
    int signature = 0;
    for (int index = 0; index < zoneCount(); ++index) {
        const Zone* candidate = zone(index);
        if (candidate->type == kFactoryZone && candidate->owner == team && candidate->claimed != 0) {
            signature = signature * 31 + index * 100 + candidate->building + 2;
        }
    }
    if (brain.troopOrderedAt < 0.0 || signature != brain.troopSignature || g_now - brain.troopOrderedAt >= kTroopRepeat) {
        brain.troopSignature = signature;
        brain.troopOrderedAt = g_now;
        brain.pressUpUntil = g_now + 0.25;
        g_api->log("ai_players: player %d presses d-pad up (attack)", slot + 1);
    }
    if (g_now < brain.pressUpUntil) {
        pad->buttons |= CW_PAD_DPAD_UP;
    }
}

void* objectTree() {
    auto* world = *reinterpret_cast<std::uint8_t**>(0x003A2F0C);
    return world != nullptr ? *reinterpret_cast<void**>(world + 0x18) : nullptr;
}

std::uint8_t* objectFromHandle(int handle) {
    void* tree = objectTree();
    return tree != nullptr ? reinterpret_cast<std::uint8_t*(__fastcall*)(void*, void*, int)>(kObjectFromHandle)(tree, nullptr, handle) : nullptr;
}

// Something worth shooting: alive, on another team, with health left and not invulnerable (ordnance and props have
// no health; HQs and generators can't be destroyed).
bool isEnemy(const std::uint8_t* candidate, int team) {
    if (candidate == nullptr || ((*reinterpret_cast<const std::uint32_t*>(candidate + 0x04) >> 8) & 1) == 0) {
        return false;
    }
    // Teams 1 and 2 are the players' (and their buildings'); the troops the outposts build are 11 and 12.
    const std::int32_t other = *reinterpret_cast<const std::int32_t*>(candidate + 0xC0);
    const std::int32_t side = other > 10 ? other - 10 : other;
    if ((side != 1 && side != 2) || side == (team > 10 ? team - 10 : team)) {
        return false;
    }
    auto getComponent = reinterpret_cast<const float*(__fastcall*)(const void*, void*, std::uint32_t)>(
        (*reinterpret_cast<void* const* const*>(candidate))[16]);
    const float* health = getComponent(candidate, nullptr, kHealthComponent);
    return health != nullptr && health[0x14 / 4] > 0.0f && health[0x18 / 4] < kInvulnerable;
}

// The nearest enemy within kEngageRange, or 0.
int nearestEnemy(const std::uint8_t* self, const float* position, int team) {
    auto* tree = static_cast<std::uint8_t*>(objectTree());
    if (tree == nullptr) {
        return 0;
    }
    std::vector<const std::uint8_t*> pending;
    if (auto* root = *reinterpret_cast<const std::uint8_t* const*>(tree + 0x08)) {
        pending.push_back(root);
    }
    int best = 0;
    float bestDistance = kEngageRange;
    while (!pending.empty() && pending.size() < 100000) {
        const std::uint8_t* node = pending.back();
        pending.pop_back();
        constexpr std::uint32_t kChildren[] = {0x08, 0x0C};
        for (std::uint32_t child : kChildren) {
            if (auto* next = *reinterpret_cast<const std::uint8_t* const*>(node + child)) {
                pending.push_back(next);
            }
        }
        auto* candidate = *reinterpret_cast<std::uint8_t* const*>(node + 0x18);
        if (candidate == self || !isEnemy(candidate, team)) {
            continue;
        }
        float at[3];
        worldPosition(candidate, at);
        const float distance = distance2d(at, position);
        if (distance < bestDistance) {
            best = *reinterpret_cast<const std::int32_t*>(node + 0x14);
            bestDistance = distance;
        }
    }
    return best;
}

// Shoots at the nearest enemy; returns true when it took over the left stick (turning to face it).
bool fight(Brain& brain, int slot, const std::uint8_t* object, const float* position, int team, CwPad* pad) {
    if (g_now >= brain.retargetAt) {
        brain.retargetAt = g_now + kRetarget;
        int enemy = nearestEnemy(object, position, team);
        // Pushing: the HQ comes first once it is in range (its shield regenerates, so spread fire never breaks it).
        if (brain.pushing && brain.hqHandle != 0 && distance2d(position, brain.hq) < kEngageRange && isEnemy(objectFromHandle(brain.hqHandle), team)) {
            enemy = brain.hqHandle;
        }
        if (enemy != brain.enemy && enemy != 0) {
            float at[3];
            worldPosition(objectFromHandle(enemy), at);
            g_api->log("ai_players: player %d engages enemy %d (team %d, %.0f away)", slot + 1, enemy,
                *reinterpret_cast<const std::int32_t*>(objectFromHandle(enemy) + 0xC0), distance2d(at, position));
        }
        brain.enemy = enemy;
    }
    std::uint8_t* enemy = brain.enemy != 0 ? objectFromHandle(brain.enemy) : nullptr;
    if (!isEnemy(enemy, team)) {
        brain.enemy = 0;
        return false;
    }
    float at[3];
    worldPosition(enemy, at);
    const float distance = distance2d(at, position);
    if (distance > kEngageRange) {
        return false;
    }
    const float error = headingError(object, position, at);
    if (std::fabs(error) < kFireAngle) {
        pad->analog[CW_PAD_A] = 255;
        if (distance > kSecondaryRange && g_now >= brain.secondaryAt) {
            brain.secondaryAt = g_now + kSecondaryEvery;
            brain.secondaryUntil = g_now + 0.2;
        }
    }
    if (g_now < brain.secondaryUntil) {
        pad->analog[CW_PAD_X] = 255;
    }
    if (!brain.inPlace && distance > kStandRange) {
        return false;  // keep driving; shoot whatever comes into line
    }
    const float turn = error * 2.5f;
    pad->thumbLX = static_cast<std::int16_t>((turn > 1.0f ? 1.0f : (turn < -1.0f ? -1.0f : turn)) * 32767.0f);
    pad->thumbLY = 0;
    return true;
}

int heldBy(int team) {
    int count = 0;
    for (int index = 0; index < zoneCount(); ++index) {
        const Zone* candidate = zone(index);
        count += candidate->type == kFactoryZone && candidate->owner == team && candidate->claimed != 0;
    }
    return count;
}

// Push the enemy HQ once none of our outposts needs rebuilding (outposts only build while a player stands in them,
// and their troops are what bring HQs down) and we are far enough ahead on outposts, or nothing is left to take. The
// lead needed shrinks as the match goes on, like a round timer forcing a decision: kPushLead, then 1 after
// kEscalate1 s, then 0 (tied is enough) after kEscalate2 s. Once pushing, it goes on while at most 1 below that.
bool wantPush(const float* position, int team, int slot, bool pushing) {
    for (int index = 0; index < zoneCount(); ++index) {
        if (priority(zone(index), team) == 0) {
            return false;
        }
    }
    const int lead = heldBy(team) - heldBy(3 - team);
    const int needed = g_now < kEscalate1 ? kPushLead : (g_now < kEscalate2 ? 1 : 0);
    return lead >= (pushing ? needed - 1 : needed) || chooseTarget(position, team, slot, true) < 0;
}

// The enemy HQ: a destructible building of the other team with the most health.
bool findEnemyHq(int team, float* out, int* handle) {
    auto* tree = static_cast<std::uint8_t*>(objectTree());
    if (tree == nullptr) {
        return false;
    }
    std::vector<const std::uint8_t*> pending;
    if (auto* root = *reinterpret_cast<const std::uint8_t* const*>(tree + 0x08)) {
        pending.push_back(root);
    }
    float best = kHqMinHealth;
    bool found = false;
    while (!pending.empty() && pending.size() < 100000) {
        const std::uint8_t* node = pending.back();
        pending.pop_back();
        constexpr std::uint32_t kChildren[] = {0x08, 0x0C};
        for (std::uint32_t child : kChildren) {
            if (auto* next = *reinterpret_cast<const std::uint8_t* const*>(node + child)) {
                pending.push_back(next);
            }
        }
        auto* candidate = *reinterpret_cast<std::uint8_t* const*>(node + 0x18);
        if (candidate == nullptr || *reinterpret_cast<const std::int32_t*>(candidate + 0xC0) != 3 - team || !isEnemy(candidate, team)) {
            continue;
        }
        auto getComponent = reinterpret_cast<const float*(__fastcall*)(const void*, void*, std::uint32_t)>(
            (*reinterpret_cast<void* const* const*>(candidate))[16]);
        const float maxHealth = getComponent(candidate, nullptr, kHealthComponent)[0x18 / 4];
        if (maxHealth > best) {
            best = maxHealth;
            worldPosition(candidate, out);
            *handle = *reinterpret_cast<const std::int32_t*>(node + 0x14);
            found = true;
        }
    }
    return found;
}

// Starts a push: the route from the outpost nearest to us along that outpost's path to the enemy base.
void startPush(Brain& brain, int slot, const float* position, int team) {
    int nearest = -1;
    for (int index = 0; index < zoneCount(); ++index) {
        if (zone(index)->type == kFactoryZone &&
            (nearest < 0 || distance2d(zone(index)->position, position) < distance2d(zone(nearest)->position, position))) {
            nearest = index;
        }
    }
    brain.routeLength = 0;
    if (nearest >= 0) {
        char name[16];
        std::snprintf(name, sizeof(name), "Fac%d_%d", zone(nearest)->id, 3 - team);
        planPath(brain, slot, name, brain.hq, position);
    }
    std::memcpy(brain.segmentStart, position, sizeof(brain.segmentStart));
    brain.best = 1e9f;
    brain.progressAt = g_now;
}

void drivePush(Brain& brain, int slot, const std::uint8_t* object, const float* position, CwPad* pad) {
    if (brain.routeIndex < brain.routeLength && distance2d(position, brain.hq) > kStandRange) {
        const int previous = brain.routeIndex;
        float lookahead[3];
        routeLookahead(brain, position, lookahead);
        const float leg = distance2d(position, brain.route[brain.routeIndex]);
        if (brain.routeIndex != previous || leg < brain.best - kStuckProgress) {
            brain.best = leg;
            brain.progressAt = g_now;
        } else if (g_now - brain.progressAt > kStuckSeconds) {
            std::memcpy(brain.segmentStart, position, sizeof(brain.segmentStart));
            ++brain.routeIndex;
            brain.best = 1e9f;
            brain.progressAt = g_now;
        }
        if (brain.routeIndex + 1 >= brain.routeLength && distance2d(position, brain.route[brain.routeIndex]) < kWaypointReached) {
            brain.routeIndex = brain.routeLength;  // route done: straight at the HQ
        }
        steer(pad, object, position, lookahead);
        return;
    }
    steer(pad, object, position, brain.hq);
}

void resetApproach(Brain& brain) {
    brain.approachStarted = false;
    brain.sideMove = false;
    brain.sideTries = 0;
}

void navigate(Brain& brain, int slot, std::uint8_t* object, const float* position, int team, CwPad* pad);

void think(Brain& brain, int slot, std::uint8_t* object, CwPad* pad) {
    const int team = *reinterpret_cast<const std::int32_t*>(object + 0xC0);
    float position[3];
    worldPosition(object, position);
    troopCommands(brain, slot, team, pad);

    // A new tank (respawn) or a jump across the map: start over from here.
    if (object != brain.object || distance2d(position, brain.lastPosition) > kRespawnJump) {
        if (brain.object != nullptr) {
            g_api->log("ai_players: player %d respawned; picking a new outpost", slot + 1);
        }
        brain.object = object;
        brain.target = -1;
        brain.inPlace = false;
        brain.enemy = 0;
        brain.pushing = false;
        brain.pushCheckAt = 0.0;
    }
    std::memcpy(brain.lastPosition, position, sizeof(brain.lastPosition));

    navigate(brain, slot, object, position, team, pad);
    if (fight(brain, slot, object, position, team, pad)) {
        brain.progressAt = g_now;  // standing to fight is not being stuck
    }
}

void navigate(Brain& brain, int slot, std::uint8_t* object, const float* position, int team, CwPad* pad) {
    if (g_now >= brain.pushCheckAt) {
        brain.pushCheckAt = g_now + kPushCheck;
        const bool push = wantPush(position, team, slot, brain.pushing) && findEnemyHq(team, brain.hq, &brain.hqHandle);
        if (push != brain.pushing) {
            brain.pushing = push;
            g_api->log("ai_players: player %d %s (outposts %d vs %d)", slot + 1, push ? "pushes the enemy HQ" : "stops pushing",
                heldBy(team), heldBy(3 - team));
            brain.target = -1;
            brain.inPlace = false;
            if (push) {
                startPush(brain, slot, position, team);
            }
        }
    }
    if (brain.pushing) {
        drivePush(brain, slot, object, position, pad);
        return;
    }
    if (brain.target >= 0 && held(zone(brain.target), team)) {
        g_api->log("ai_players: player %d: zone %d claimed and fully built", slot + 1, zone(brain.target)->id);
        brain.target = -1;
    }
    if (brain.target >= 0 && !brain.inPlace) {
        // While travelling: let an ally have it if they got there first, and switch to something more urgent.
        const int better = chooseTarget(position, team, slot);
        if (takenByAlly(brain.target, slot, team, true) && better >= 0) {
            g_api->log("ai_players: player %d leaves zone %d to an ally", slot + 1, zone(brain.target)->id);
            brain.target = -1;
        } else if (better >= 0 && better != brain.target && priority(zone(better), team) < priority(zone(brain.target), team)) {
            brain.target = -1;
        }
    }
    if (brain.target < 0) {
        brain.target = chooseTarget(position, team, slot);
        if (brain.target < 0) {
            brain.target = chooseTarget(position, team, slot, true);  // everything left is covered: help out anyway
        }
        if (brain.target < 0) {
            return;  // every outpost is ours and built
        }
        static const char* const kReasons[] = {"rebuild", "unclaimed", "enemy"};
        g_api->log("ai_players: player %d heading for zone %d (%s)", slot + 1, zone(brain.target)->id,
            kReasons[priority(zone(brain.target), team)]);
        brain.inPlace = false;
        brain.lastBuilding = -2;
        resetApproach(brain);
        planRoute(brain, slot, zone(brain.target), position, team);
        brain.best = 1e9f;
        brain.progressAt = g_now;
    }
    const Zone* target = zone(brain.target);
    if (target->owner == team && target->claimed != 0 && target->building != brain.lastBuilding) {
        brain.lastBuilding = target->building;
        g_api->log("ai_players: player %d: zone %d building item %d", slot + 1, target->id, target->building);
    }
    const bool counted = target->claimer == slot;
    if (counted != brain.inPlace) {
        brain.inPlace = counted;
        g_api->log("ai_players: player %d %s zone %d", slot + 1, counted ? "holding" : "lost the claim on", target->id);
        if (!counted) {
            resetApproach(brain);
        }
    }
    if (brain.inPlace) {
        return;  // stand still while it builds
    }
    const float distance = distance2d(position, target->position);

    // On the way: the map's route.
    if (distance > target->radius && brain.routeIndex < brain.routeLength) {
        const int previous = brain.routeIndex;
        float lookahead[3];
        routeLookahead(brain, position, lookahead);
        const bool lastReached = brain.routeIndex + 1 >= brain.routeLength && distance2d(position, brain.route[brain.routeIndex]) < kWaypointReached;
        if (!lastReached) {
            const float leg = distance2d(position, brain.route[brain.routeIndex]);
            if (brain.routeIndex != previous || leg < brain.best - kStuckProgress) {
                brain.best = leg;
                brain.progressAt = g_now;
            } else if (g_now - brain.progressAt > kStuckSeconds) {
                g_api->log("ai_players: player %d stuck %.0f short of route point %d of %d; skipping it", slot + 1, leg, brain.routeIndex + 1,
                    brain.routeLength);
                std::memcpy(brain.segmentStart, position, sizeof(brain.segmentStart));
                ++brain.routeIndex;
                brain.best = 1e9f;
                brain.progressAt = g_now;
                return;
            }
            steer(pad, object, position, lookahead);
            return;
        }
        brain.routeIndex = brain.routeLength;  // route done
    }

    // Into the claim circle: through the centre, and round the outpost when blocked.
    if (!brain.approachStarted) {
        brain.approachStarted = true;
        brain.best = distance;
        brain.progressAt = g_now;
        g_api->log("ai_players: player %d moving into zone %d (%.0f from the centre, radius %.0f)", slot + 1, target->id, distance,
            target->radius);
    }
    if (brain.sideMove) {
        const float toSide = distance2d(position, brain.side);
        if (toSide < kWaypointReached || g_now - brain.progressAt > kStuckSeconds) {
            brain.sideMove = false;
            brain.best = distance;
            brain.progressAt = g_now;
        } else {
            if (toSide < brain.best - kStuckProgress) {
                brain.best = toSide;
                brain.progressAt = g_now;
            }
            steer(pad, object, position, brain.side);
            return;
        }
    } else if (distance < brain.best - kStuckProgress) {
        brain.best = distance;
        brain.progressAt = g_now;
    } else if (g_now - brain.progressAt > kStuckSeconds) {
        if (brain.sideTries++ == 0) {
            brain.sideAngle = std::atan2(position[2] - target->position[2], position[0] - target->position[0]);
        }
        const float angle = brain.sideAngle + kSideStep * brain.sideTries;
        brain.side[0] = target->position[0] + std::cos(angle) * target->radius * kSideRing;
        brain.side[1] = target->position[1];
        brain.side[2] = target->position[2] + std::sin(angle) * target->radius * kSideRing;
        brain.sideMove = true;
        brain.best = distance2d(position, brain.side);
        brain.progressAt = g_now;
        g_api->log("ai_players: player %d stuck %s zone %d's circle (%.0f from the centre); driving round to side %d", slot + 1,
            distance > target->radius ? "outside" : "inside", target->id, distance, brain.sideTries);
        steer(pad, object, position, brain.side);
        return;
    }
    // Aim past the centre so the tank drives through the middle.
    const float scale = distance > 1.0f ? -target->radius * kOvershootFraction / distance : 0.0f;
    const float destination[3] = {target->position[0] + (position[0] - target->position[0]) * scale, target->position[1],
        target->position[2] + (position[2] - target->position[2]) * scale};
    steer(pad, object, position, destination);
}

} // namespace

void install(const CwModApi* api) {
    g_api = api;
}

void missionStart(const char* mission) {
    g_conquest = false;
    for (const char* map : kConquestMaps) {
        const std::size_t length = std::strlen(map);
        if (mission != nullptr && _strnicmp(mission, map, length) == 0 && mission[length] == '.') {
            g_conquest = true;
        }
    }
    for (Brain& brain : g_brains) {
        brain = Brain{};
    }
}

void drive(int slot, CwPad* pad) {
    *pad = CwPad{};
    // Only in the match itself (screen 4): on the end screen ("Conquest battle is over") pressing A would restart it.
    if (!g_conquest || slot <= 0 || slot >= kSlots || *reinterpret_cast<const std::int32_t*>(kScreen) != kInGameScreen) {
        return;
    }
    std::uint8_t* object = playerObject(slot);
    if (object == nullptr) {
        return;
    }
    auto* world = *reinterpret_cast<std::uint8_t**>(0x003A2F0C);
    g_now = world != nullptr ? *reinterpret_cast<const float*>(world + 0x28) : 0.0;  // game time (follows pause, CW_TIME_SCALE)
    Brain& brain = g_brains[slot];
    if (!brain.started) {
        brain.started = true;
        g_api->log("ai_players: player %d (team %d) takes control", slot + 1, *reinterpret_cast<const std::int32_t*>(object + 0xC0));
    }
    think(brain, slot, object, pad);
}

} // namespace ai
