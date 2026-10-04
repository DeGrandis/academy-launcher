// Powerup Academy: a second Thule Moon Academy (multi18.wld, a copy of multi5.wld with a 60 s powerup respawn)
// whose powerup changes every time it respawns: Super Blasters -> Invincibility -> Overcharge -> Double Tap.
//
// Overcharge (ammo crate, pink glow): unlimited secondary weapon for 30 s.
// Double Tap (gold pellet, orange glow): primary cannons fire twice as fast for 60 s.
//
// Map powerups live in a table of 0x3C-byte entries at 0x43AE78 (count at 0x43AE64): +0x00 type (itemdesc.cfg
// Type), +0x04 position, +0x14 respawn time, +0x1C respawn countdown, +0x24 handle of the pickup object (0 while
// taken). Powerup_Update (0x7D520) creates the pickup at 0x7D858 when the countdown has run out; the hook there
// changes the entry's type just before that. Pickup_Apply (0x7CD20) gives a picked-up powerup to a vehicle.
//
// Online, only the host runs Powerup_Update and Pickup_Apply; every player's powerup timers (0x5EC380.., 0x80 bytes
// per player) are sent from the host to the clients, and every machine counts them down (0x7C410). The two custom
// powerups therefore live in the timers of the two built-in ones Thule Power no longer uses: Overcharge in the Total
// Offense ("Disintegration Field") timer, Double Tap in the Invisibility (cloak) timer. Their effects are applied on
// every machine from those timers, and in Thule Power the built-in effects are kept off (the disintegration
// collision check reads 0, and the cloak is never switched on since only its own pickup does that).

#include "cw_mod.h"

#include <cstdint>
#include <cstring>
#include <cwchar>

namespace {

constexpr std::uint32_t kBatchBeginMission = 0x00062750;  // void __cdecl(const char* mission, const char* directory)
constexpr std::uint32_t kPowerupCreatePickup = 0x0007D858; // esi = entry + 0x0C, edi = entry index
constexpr std::uint32_t kPickupApply = 0x0007CD20;         // bool __cdecl(int, Powerup* entry, GameObject* vehicle)
constexpr std::uint32_t kPowerupTick = 0x0007C410;         // void __cdecl(float dt, int player): counts timers down
constexpr std::uint32_t kPowerupHud = 0x0007C780;          // void __cdecl(View*, GameObject* vehicle): powerup countdowns
constexpr std::uint32_t kTotalOffenseTimer = 0x00073BF0;   // float __cdecl(int player): read by the collision check
constexpr std::uint32_t kCannonUpdate = 0x00049540;        // void __thiscall CannonPhysics::Update(float dt)
constexpr std::uint32_t kLoadEffect = 0x000A0870;          // void* __cdecl(const char* pse)
constexpr std::uint32_t kAttachEffect = 0x000A5400;        // void __cdecl(quat*, vec3*, effect, GameObject*, int)
constexpr std::uint32_t kHudTotalOffenseKey = 0x0007C905;  // push "multiplayer.misc.totaloffense" (imm32)
constexpr std::uint32_t kHudInvisibleKey = 0x0007C9A7;     // push "multiplayer.misc.invisible" (imm32)
constexpr std::uint32_t kPowerupTable = 0x0043AE78;
constexpr std::uint32_t kPowerupStride = 0x3C;
constexpr std::uint32_t kPlayerTimers = 0x005EC380;        // per player 0x80 bytes
constexpr std::uint32_t kTotalOffense = 0x04;              // timer offsets in a player's block
constexpr std::uint32_t kInvisible = 0x08;
constexpr std::uint32_t kWorld = 0x003A2F0C;               // [world]+0x14: player objects (8)
constexpr std::uint32_t kHealthComponent = 0xFBCD164A;
constexpr int kMaxPowerups = 100;
constexpr int kMaxPlayers = 8;
constexpr char kMission[] = "multi18";

constexpr float kOverchargeSeconds = 30.0f;
constexpr float kDoubleTapSeconds = 60.0f;

// The rotation. A pickup shows the model of its itemdesc.cfg type (1 Quad Damage = Super Blasters, 6 Ultimate
// Health = Invincibility, 2 Ammo crate, 16 Boost = gold pellet); the two custom kinds borrow the ammo crate and pellet.
// Left out: 7 Total Offense ("Disintegration Field"), which only destroys vehicles that belong to players (collision
// handler 0x332B0), and 8 Invisibility (cloak); both do little against the Academy's AI waves.
enum Kind { kStock, kOvercharge, kDoubleTap };
struct Slot {
    int type;
    Kind kind;
    const char* name;
};
constexpr Slot kCycle[] = {
    {1, kStock, "Super Blasters"},
    {6, kStock, "Invincibility"},
    {2, kOvercharge, "Overcharge"},
    {16, kDoubleTap, "Double Tap"},
};
constexpr int kCycleLength = static_cast<int>(sizeof(kCycle) / sizeof(kCycle[0]));

// HUD countdown labels (localize.cfg, multiplayer.misc.*, added by edits.json).
const char kOverchargeKey[] = "multiplayer.misc.overcharge";
const char kDoubleTapKey[] = "multiplayer.misc.doubletap";

const CwModApi* g_api = nullptr;
void(__cdecl* g_originalBeginMission)(const char*, const char*) = nullptr;
bool(__cdecl* g_originalPickupApply)(int, std::uint8_t*, std::uint8_t*) = nullptr;
void(__cdecl* g_originalPowerupTick)(float, int) = nullptr;
void(__cdecl* g_originalPowerupHud)(std::uint8_t*, std::uint8_t*) = nullptr;
float(__cdecl* g_originalTotalOffenseTimer)(int) = nullptr;
void(__fastcall* g_originalCannonUpdate)(std::uint8_t*, void*, float) = nullptr;

constexpr std::uint32_t kPickupFinish = 0x0007D35A;  // Pickup_Apply: common ending (message, effect, remove pickup)
constexpr std::uint32_t kPickupTypes = 0x0043A888;   // per itemdesc type, 0x50 bytes (see setPickupLabels)
constexpr int kNoEffectType = 4;                       // "Tag": Pickup_Apply's switch has no case for it

bool g_active = false;
int* g_finishEntry = nullptr;  // entry whose type pickupApply swapped, restored by pickupFinish
int g_finishType = 0;
bool g_labelsSet = false;
int g_position[kMaxPowerups];     // rotation position of each map powerup, -1 = not rotating
void* g_overchargeEffect = nullptr;
void* g_doubleTapEffect = nullptr;
std::uint32_t g_hudKeys[2] = {};  // the HUD's original label pointers, restored outside Thule Power

float& timer(int player, std::uint32_t which) {
    return *reinterpret_cast<float*>(kPlayerTimers + player * 0x80 + which);
}

std::uint8_t* playerObject(int player) {
    auto* world = *reinterpret_cast<std::uint8_t**>(kWorld);
    if (world == nullptr) {
        return nullptr;
    }
    return reinterpret_cast<std::uint8_t**>(*reinterpret_cast<std::uintptr_t*>(world + 0x14))[player];
}

// The HUD shows a countdown for each running powerup timer; in Thule Power the two reused timers get our labels.
void setHudLabels(bool custom) {
    const std::uint32_t overcharge = static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(kOverchargeKey));
    const std::uint32_t doubleTap = static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(kDoubleTapKey));
    g_api->patchBytes(kHudTotalOffenseKey, custom ? &overcharge : &g_hudKeys[0], 4, "HUD Total Offense label (academy_powerups)");
    g_api->patchBytes(kHudInvisibleKey, custom ? &doubleTap : &g_hudKeys[1], 4, "HUD Invisibility label (academy_powerups)");
}

void __cdecl beginMission(const char* mission, const char* directory) {
    g_active = mission != nullptr && _strnicmp(mission, kMission, sizeof(kMission) - 1) == 0;
    std::memset(g_position, 0xFF, sizeof(g_position));
    g_labelsSet = false;
    g_api->log("academy_powerups: mission '%s'%s", mission != nullptr ? mission : "(null)", g_active ? " (powerup cycle on)" : "");
    g_originalBeginMission(mission, directory);
    if (g_active) {
        g_overchargeEffect = reinterpret_cast<void*(__cdecl*)(const char*)>(kLoadEffect)("overcharge.pse");
        g_doubleTapEffect = reinterpret_cast<void*(__cdecl*)(const char*)>(kLoadEffect)("doubletap.pse");
    }
    setHudLabels(g_active);
}

void setPickupLabels();

void __cdecl createPickup(CwRegisters* registers) {
    const int index = static_cast<int>(registers->edi);
    if (!g_active || index < 0 || index >= kMaxPowerups) {
        return;
    }
    auto* type = reinterpret_cast<int*>(static_cast<std::uintptr_t>(registers->esi - 0x0C));
    if (!g_labelsSet) {
        g_labelsSet = true;  // the pickup tables are loaded by the time the first pickup appears
        setPickupLabels();
    }
    if (g_position[index] < 0) {
        // First appearance: keep the map's own powerup, and rotate it from then on if it is one of ours.
        for (int i = 0; i < kCycleLength; ++i) {
            if (kCycle[i].kind == kStock && *type == kCycle[i].type) {
                g_position[index] = i;
            }
        }
        if (g_position[index] < 0) {
            g_position[index] = kCycleLength;  // not rotating
            return;
        }
    } else if (g_position[index] < kCycleLength) {
        g_position[index] = (g_position[index] + 1) % kCycleLength;
        *type = kCycle[g_position[index]].type;
    } else {
        return;
    }
    g_api->log("academy_powerups: powerup %d spawns as %s", index, kCycle[g_position[index]].name);
}

void attachEffect(void* effect, std::uint8_t* vehicle) {
    if (effect == nullptr) {
        return;
    }
    float rotation[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    float offset[3] = {0.0f, 0.0f, 0.0f};
    reinterpret_cast<void(__cdecl*)(float*, float*, void*, std::uint8_t*, int)>(kAttachEffect)(rotation, offset, effect, vehicle, 1);
}

// Host: a vehicle collected a powerup. The custom kinds start their timer and glow instead of the borrowed type's
// own effect (an ammo refill, a boost).
bool __cdecl pickupApply(int unknown, std::uint8_t* entry, std::uint8_t* vehicle) {
    const std::uintptr_t offset = reinterpret_cast<std::uintptr_t>(entry) - kPowerupTable;
    const int index = static_cast<int>(offset / kPowerupStride);
    if (g_active && offset % kPowerupStride == 0 && index >= 0 && index < kMaxPowerups && g_position[index] >= 0 &&
        g_position[index] < kCycleLength && kCycle[g_position[index]].kind != kStock && vehicle != nullptr) {
        const int player = *reinterpret_cast<int*>(vehicle + 0xC8);
        if (player >= 0 && player < kMaxPlayers) {
            const Slot& slot = kCycle[g_position[index]];
            if (slot.kind == kOvercharge) {
                timer(player, kTotalOffense) += kOverchargeSeconds;
                attachEffect(g_overchargeEffect, vehicle);
            } else {
                timer(player, kInvisible) += kDoubleTapSeconds;
                attachEffect(g_doubleTapEffect, vehicle);
            }
            g_api->log("academy_powerups: player %d picked up %s", player, slot.name);
            // Let the game finish the pickup (message, sound, effect, remove the pickup so it can respawn) with no
            // effect of its own: its switch does nothing for type 4, and pickupFinish restores the real type.
            auto* type = reinterpret_cast<int*>(entry);
            g_finishType = *type;
            g_finishEntry = type;
            *type = kNoEffectType;
            const bool taken = g_originalPickupApply(unknown, entry, vehicle);
            if (g_finishEntry != nullptr) {
                *g_finishEntry = g_finishType;
                g_finishEntry = nullptr;
            }
            return taken;
        }
    }
    return g_originalPickupApply(unknown, entry, vehicle);
}

// Start of Pickup_Apply's common ending, after the per-type effects: put the borrowed type back so the message,
// sound and pickup effect are the ammo crate's / pellet's own (with the labels set in setPickupLabels).
void __cdecl pickupFinish(CwRegisters*) {
    if (g_finishEntry != nullptr) {
        *g_finishEntry = g_finishType;
        g_finishEntry = nullptr;
    }
}

// The pickup message shows a per-type label (0x43A888 + type * 0x50: +0x04 pickup effect, +0x08 idle effect,
// +0x0C sound, +0x10 label, wide string); in Thule Power the borrowed types announce the custom powerups. The table is
// rebuilt for every mission.
void setPickupLabels() {
    for (const Slot& slot : kCycle) {
        if (slot.kind == kStock) {
            continue;
        }
        auto* label = reinterpret_cast<wchar_t*>(kPickupTypes + slot.type * 0x50 + 0x10);
        const wchar_t* text = slot.kind == kOvercharge ? L"OVERCHARGE" : L"DOUBLE TAP";
        std::wcsncpy(label, text, 31);
        label[31] = 0;
    }
}

// Every machine, every frame, for each player: while Overcharge runs, keep the secondary weapon full (the same
// refill the ammo pickup does: GameObject::AddAmmo(slot 1 = secondary, fraction)).
bool g_overchargeLogged[kMaxPlayers] = {};
bool g_doubleTapLogged[kMaxPlayers] = {};

void __cdecl powerupTick(float dt, int player) {
    g_originalPowerupTick(dt, player);
    if (!g_active || player < 0 || player >= kMaxPlayers) {
        return;
    }
    if (timer(player, kInvisible) <= 0.0f) {
        g_doubleTapLogged[player] = false;
    }
    if (timer(player, kTotalOffense) <= 0.0f) {
        g_overchargeLogged[player] = false;
        return;
    }
    std::uint8_t* vehicle = playerObject(player);
    if (vehicle != nullptr) {
        auto addAmmo = reinterpret_cast<bool(__thiscall*)(std::uint8_t*, int, float)>((*reinterpret_cast<void***>(vehicle))[0x84 / 4]);
        const bool refilled = addAmmo(vehicle, 1, 1.0f);
        if (!g_overchargeLogged[player]) {
            g_overchargeLogged[player] = true;
            g_api->log("academy_powerups: Overcharge on player %d (%.0f s left, secondary %s)", player, timer(player, kTotalOffense),
                refilled ? "refilled" : "already full");
        }
    }
}

// Online, a joining machine fires its own player's weapons from its own ammo, but it never runs Powerup_Tick for
// that player (the host does, and sends the timers). The powerup HUD (0x7C780, every frame, for each local player on
// every machine) is where the local player's Overcharge refill happens on every machine.
void __cdecl powerupHud(std::uint8_t* view, std::uint8_t* vehicle) {
    g_originalPowerupHud(view, vehicle);
    if (!g_active || vehicle == nullptr) {
        return;
    }
    const int player = *reinterpret_cast<int*>(vehicle + 0xC8);
    if (player >= 0 && player < kMaxPlayers && timer(player, kTotalOffense) > 0.0f) {
        auto addAmmo = reinterpret_cast<bool(__thiscall*)(std::uint8_t*, int, float)>((*reinterpret_cast<void***>(vehicle))[0x84 / 4]);
        addAmmo(vehicle, 1, 1.0f);
        if (!g_overchargeLogged[player]) {
            g_overchargeLogged[player] = true;
            g_api->log("academy_powerups: Overcharge on player %d (local, %.0f s left)", player, timer(player, kTotalOffense));
        }
    }
}

// The Disintegration Field's collision kill reads this timer; in Thule Power it holds Overcharge instead.
float __cdecl totalOffenseTimer(int player) {
    return g_active ? 0.0f : g_originalTotalOffenseTimer(player);
}

// Every machine: a primary cannon (+0xF0 weapon group 0) on a vehicle whose player has Double Tap counts the time
// since its last shot (+0x1FC) twice as fast, so it fires twice as often.
void __fastcall cannonUpdate(std::uint8_t* cannon, void* edx, float dt) {
    if (g_active && *reinterpret_cast<int*>(cannon + 0xF0) == 0) {
        std::uint8_t* owner = *reinterpret_cast<std::uint8_t**>(cannon + 0x1C);
        if (owner != nullptr) {
            const int player = *reinterpret_cast<int*>(owner + 0xC8);
            if (player >= 0 && player < kMaxPlayers && timer(player, kInvisible) > 0.0f) {
                *reinterpret_cast<float*>(cannon + 0x1FC) += dt;
                if (!g_doubleTapLogged[player]) {
                    g_doubleTapLogged[player] = true;
                    g_api->log("academy_powerups: Double Tap on player %d (%.0f s left)", player, timer(player, kInvisible));
                }
            }
        }
    }
    g_originalCannonUpdate(cannon, edx, dt);
}

} // namespace

extern "C" __declspec(dllexport) int __cdecl CwModInit(const CwModApi* api) {
    if (api->version != CW_MOD_API_VERSION) {
        return 0;
    }
    g_api = api;
    std::memcpy(&g_hudKeys[0], reinterpret_cast<const void*>(kHudTotalOffenseKey), 4);
    std::memcpy(&g_hudKeys[1], reinterpret_cast<const void*>(kHudInvisibleKey), 4);
    return api->detour(kBatchBeginMission, reinterpret_cast<const void*>(&beginMission), reinterpret_cast<void**>(&g_originalBeginMission),
               "Batch_BeginMission (academy_powerups)")
        && api->midHook(kPowerupCreatePickup, &createPickup, "Powerup_CreatePickup (academy_powerups)")
        && api->detour(kPickupApply, reinterpret_cast<const void*>(&pickupApply), reinterpret_cast<void**>(&g_originalPickupApply),
               "Pickup_Apply (academy_powerups)")
        && api->midHook(kPickupFinish, &pickupFinish, "Pickup_Apply finish (academy_powerups)")
        && api->detour(kPowerupTick, reinterpret_cast<const void*>(&powerupTick), reinterpret_cast<void**>(&g_originalPowerupTick),
               "Powerup_Tick (academy_powerups)")
        && api->detour(kPowerupHud, reinterpret_cast<const void*>(&powerupHud), reinterpret_cast<void**>(&g_originalPowerupHud),
               "Powerup_DrawHud (academy_powerups)")
        && api->detour(kTotalOffenseTimer, reinterpret_cast<const void*>(&totalOffenseTimer), reinterpret_cast<void**>(&g_originalTotalOffenseTimer),
               "TotalOffense_GetTimer (academy_powerups)")
        && api->detour(kCannonUpdate, reinterpret_cast<const void*>(&cannonUpdate), reinterpret_cast<void**>(&g_originalCannonUpdate),
               "CannonPhysics::Update (academy_powerups)");
}
