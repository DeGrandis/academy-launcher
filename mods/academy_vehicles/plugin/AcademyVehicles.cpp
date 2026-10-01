// Lets the player fly the gunship in the Academy.
//
// A flyer that touches the ground marks itself landed (FlyerPhysics +0x143 "land request" = 1, state +0x340 = 2).
// Only a control message clears it, and the game sends that one after a gunship drops off its troops; there is no
// takeoff button. The Academy spawns vehicles on the ground, so the player's gunship would sit there for good.
// Before each physics update this clears the land request on the player's flyer, so the state machine takes off.

#include "cw_mod.h"
#include "game/GameObjects.h"

#include <cstdint>

namespace {

constexpr std::uint32_t kFlyerPhysicsVtable = 0x003383D0;
constexpr int kFlyerPhysicsUpdateSlot = 22;        // 0x00035310, void __thiscall(float dt)
constexpr std::uint32_t kLandRequest = 0x143;      // byte, set by control message 8 and by touching the ground
constexpr std::uint32_t kGameObjectPhysics = 0x20;

const CwModApi* g_api = nullptr;
void(__fastcall* g_originalUpdate)(std::uint8_t*, void*, float) = nullptr;
bool g_logged = false;

bool isPlayerPhysics(const std::uint8_t* physics) {
    for (int slot = 0; slot < 4; ++slot) {
        const cw::game::GameObject player = cw::game::GameObject::player(slot);
        if (player && *reinterpret_cast<std::uint8_t* const*>(player.self + kGameObjectPhysics) == physics) {
            return true;
        }
    }
    return false;
}

void __fastcall update(std::uint8_t* physics, void* edx, float dt) {
    if (physics[kLandRequest] != 0 && isPlayerPhysics(physics)) {
        physics[kLandRequest] = 0;
        if (!g_logged) {
            g_logged = true;
            g_api->log("academy_vehicles: player flyer taking off");
        }
    }
    g_originalUpdate(physics, edx, dt);
}

} // namespace

extern "C" __declspec(dllexport) int __cdecl CwModInit(const CwModApi* api) {
    if (api->version != CW_MOD_API_VERSION) {
        return 0;
    }
    g_api = api;
    return api->hookVirtual(kFlyerPhysicsVtable, kFlyerPhysicsUpdateSlot, reinterpret_cast<const void*>(&update),
        reinterpret_cast<void**>(&g_originalUpdate), "FlyerPhysics::Update (academy_vehicles)");
}
