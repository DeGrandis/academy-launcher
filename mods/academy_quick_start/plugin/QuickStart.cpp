// Example plugin: skips the "GET READY" wait before an Academy match and counts multiplayer HUD draws.
//
// Shows the three ways to hook game code: a detour that calls the original (Mp_Update), a mid-function hook with
// register access (Mp_DrawPlayerHud), and typed globals from the generated symbol header.

#include "cw_mod.h"
#include "game/GameSymbols.h"

namespace {

const CwModApi* g_api = nullptr;
void(__cdecl* g_originalUpdate)(float) = nullptr;
int g_hudDraws = 0;

constexpr std::uint8_t kGameTypeAcademy = 3;
// The pre-game timer counts up from -5; "GET READY" shows until -3, then a 3-2-1 countdown runs to 0.
constexpr float kCountdownStart = -3.0f;

void __cdecl quickUpdate(float dt) {
    g_originalUpdate(dt);
    float& timer = *cw::game::g_mpPregameTimer;
    if (*cw::game::g_mpGameType == kGameTypeAcademy && timer < kCountdownStart) {
        g_api->log("academy_quick_start: skipping GET READY (timer %.2f -> %.2f)", timer, kCountdownStart);
        timer = kCountdownStart;
    }
}

void __cdecl onHudDraw(CwRegisters* registers) {
    if (++g_hudDraws == 1) {
        g_api->log("academy_quick_start: first multiplayer HUD draw (esp=%08X)", registers->esp);
    }
}

} // namespace

extern "C" __declspec(dllexport) int __cdecl CwModInit(const CwModApi* api) {
    if (api->version != CW_MOD_API_VERSION) {
        return 0;
    }
    g_api = api;
    const int detoured = api->detour(reinterpret_cast<std::uint32_t>(cw::game::Mp_Update), reinterpret_cast<const void*>(&quickUpdate),
        reinterpret_cast<void**>(&g_originalUpdate), "Mp_Update");
    const int midHooked = api->midHook(cw::game::Mp_DrawPlayerHud, &onHudDraw, "Mp_DrawPlayerHud");
    return detoured && midHooked;
}
