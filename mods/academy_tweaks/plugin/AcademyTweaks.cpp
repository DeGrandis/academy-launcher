// Example plugin: reads and changes Academy state through game/GameObjects.h.
//
// - Logs every new wave (wave, Academy level, live enemies) with the player's position and health.
// - Doubles the player's maximum health when the match starts.

#include "cw_mod.h"
#include "game/GameObjects.h"

namespace {

const CwModApi* g_api = nullptr;
void(__cdecl* g_originalUpdate)(float) = nullptr;
int g_loggedWave = -1;
bool g_boosted = false;

void __cdecl update(float dt) {
    g_originalUpdate(dt);
    using namespace cw::game;
    const ThuleAcademyScript script = ThuleAcademyScript::current();
    const GameObject player = GameObject::player(0);
    if (!script || !player || !player.alive()) {
        return;
    }
    if (!g_boosted && player.maxHealth() > 0.0f) {
        g_boosted = true;
        const float before = player.maxHealth();
        player.setHealth(before * 2.0f, before * 2.0f);
        g_api->log("academy_tweaks: player health %.0f -> %.0f", before, player.maxHealth());
    }
    if (script.currentWave() != g_loggedWave) {
        g_loggedWave = script.currentWave();
        int enemies = 0;
        for (int index = 0; index < ThuleAcademyScript::kMaxEnemies; ++index) {
            enemies += script.enemies()[index] != 0;
        }
        const float* position = player.position();
        g_api->log("academy_tweaks: wave %d (groups %d, level %d, loops %d, live enemies %d) player at (%.1f, %.1f, %.1f) health %.0f/%.0f",
            g_loggedWave, script.wave(g_loggedWave).groupCount, academyLevel(), script.loops(), enemies, position[0], position[1], position[2],
            player.health(), player.maxHealth());
    }
}

} // namespace

extern "C" __declspec(dllexport) int __cdecl CwModInit(const CwModApi* api) {
    if (api->version != CW_MOD_API_VERSION) {
        return 0;
    }
    g_api = api;
    return api->detour(reinterpret_cast<std::uint32_t>(cw::game::Mp_Update), reinterpret_cast<const void*>(&update),
        reinterpret_cast<void**>(&g_originalUpdate), "Mp_Update (academy_tweaks)");
}
