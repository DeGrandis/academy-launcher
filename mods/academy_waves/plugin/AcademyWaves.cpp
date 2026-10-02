// Rewrites the single-player Thule Moon Academy waves (see docs/academy_waves.md) from academy_waves.ini:
// - bonus-point waves become a copy of the enemy wave before them (and raise the Academy level like one),
// - groups can be set to "keep going" (no waiting until all enemies are dead), optionally except each wave's last
//   enemy group, so units stream in during a wave but the next wave waits for a clear,
// - pauses between groups are scaled,
// - wave N spawns floor(original enemies * (1 + growth) ^ (N - 1)); extra units repeat the wave's own units.
//
// ThuleAcademyScript_SetupWaves1 (0x17E72D) points the script's wave entries at tables in the game image; after it
// runs, this builds new tables in plugin memory and points the entries at them. Units beyond the script's 30 tracked
// enemies still spawn; they are just not counted (only matters for groups that wait for a clear).

#include "cw_mod.h"
#include "game/GameObjects.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

using cw::game::ThuleAcademyScript;

constexpr std::uint32_t kSetupWaves1 = 0x0017E72D;  // void __thiscall(ThuleAcademyScript*)
constexpr std::uint32_t kUnitNames = 0x003968F8;    // const char*[], index 0 (null) ends a group
constexpr int kUnitNameCount = 16;
constexpr int kRecordSize = 0x14;                   // path effect record per group

struct Settings {
    bool replaceBonusWaves = true;
    int keepGoing = 2;  // 0 = the game's own flags, 1 = every group, 2 = every group but each wave's last enemy group
    float delayScale = 1.0f;
    float growth = 0.10f;
};

struct Unit {
    std::int8_t unit;
    std::int8_t spawn;
};

struct Group {
    std::vector<Unit> units;
    float delay = 0.0f;
    const char* sound = nullptr;
    std::uint8_t record[kRecordSize] = {};
    std::uint8_t keepGoing = 0;
};

struct OriginalWave {
    ThuleAcademyScript::Wave entry{};
    std::vector<Group> groups;
    int enemies = 0;
    std::uint8_t levelStep = 0;
};

// Storage the rewritten wave entries point into; rebuilt on every setup.
struct BuiltWave {
    std::vector<std::int8_t> spawnList;
    std::vector<float> delays;
    std::vector<const char*> sounds;
    std::vector<std::uint8_t> records;
    std::vector<std::uint8_t> keepGoing;
};

const CwModApi* g_api = nullptr;
void(__fastcall* g_originalSetup)(std::uint8_t*, void*) = nullptr;
BuiltWave g_built[ThuleAcademyScript::kMaxWaves];
std::uint8_t g_levelSteps[ThuleAcademyScript::kMaxWaves + 2] = {};

const char* unitName(int unit) {
    if (unit < 0 || unit >= kUnitNameCount) {
        return nullptr;
    }
    return reinterpret_cast<const char* const*>(kUnitNames)[unit];
}

Settings readSettings() {
    Settings settings;
    const std::string path = std::string(g_api->modRoot) + "\\academy_waves.ini";
    char value[64];
    auto read = [&](const char* key, const char* fallback) {
        GetPrivateProfileStringA("waves", key, fallback, value, sizeof(value), path.c_str());
        return std::atof(value);
    };
    settings.replaceBonusWaves = read("replace_bonus_waves", "1") != 0.0;
    settings.keepGoing = static_cast<int>(read("keep_going", "2"));
    settings.delayScale = static_cast<float>(read("delay_scale", "1.0"));
    settings.growth = static_cast<float>(read("growth_per_wave", "0.10"));
    return settings;
}

OriginalWave readWave(const ThuleAcademyScript& script, int index, const std::uint8_t* levelSteps) {
    OriginalWave wave;
    wave.entry = script.wave(index);
    wave.levelStep = levelSteps != nullptr ? levelSteps[index - 1] : 0;
    const std::int8_t* position = wave.entry.spawnList;
    for (int group = 0; group < wave.entry.groupCount; ++group) {
        Group parsed;
        while (position != nullptr) {
            const Unit unit{position[0], position[1]};
            position += 2;
            if (unitName(unit.unit) == nullptr) {
                break;
            }
            parsed.units.push_back(unit);
        }
        parsed.delay = wave.entry.delays != nullptr ? wave.entry.delays[group] : 0.0f;
        parsed.sound = wave.entry.sounds != nullptr ? wave.entry.sounds[group] : nullptr;
        if (wave.entry.pathEffects != nullptr) {
            std::memcpy(parsed.record, wave.entry.pathEffects + group * kRecordSize, kRecordSize);
        }
        parsed.keepGoing = wave.entry.keepGoing != nullptr ? wave.entry.keepGoing[group] : 0;
        wave.enemies += static_cast<int>(parsed.units.size());
        wave.groups.push_back(std::move(parsed));
    }
    return wave;
}

void writeWave(const ThuleAcademyScript& script, int index, const ThuleAcademyScript::Wave& source, const std::vector<Group>& groups) {
    BuiltWave& built = g_built[index];
    built = {};
    for (const Group& group : groups) {
        for (const Unit& unit : group.units) {
            built.spawnList.push_back(unit.unit);
            built.spawnList.push_back(unit.spawn);
        }
        built.spawnList.push_back(0);  // unit 0 has no name: end of group
        built.spawnList.push_back(0);
        built.delays.push_back(group.delay);
        built.sounds.push_back(group.sound);
        built.records.insert(built.records.end(), group.record, group.record + kRecordSize);
        built.keepGoing.push_back(group.keepGoing);
    }
    ThuleAcademyScript::Wave& entry = script.wave(index);
    entry = source;  // keeps the effect start/stop lists of the wave the groups came from
    entry.groupCount = static_cast<std::int32_t>(groups.size());
    entry.spawnList = built.spawnList.data();
    entry.delays = built.delays.data();
    entry.sounds = built.sounds.data();
    entry.pathEffects = built.records.data();
    entry.keepGoing = built.keepGoing.data();
}

void __fastcall setupWaves(std::uint8_t* self, void* edx) {
    g_originalSetup(self, edx);
    const ThuleAcademyScript script{self};
    const Settings settings = readSettings();
    const std::uint8_t* levelSteps = script.levelSteps();

    std::vector<OriginalWave> waves(ThuleAcademyScript::kMaxWaves);
    int lastWave = 0;
    for (int index = 1; index < ThuleAcademyScript::kMaxWaves; ++index) {
        waves[index] = readWave(script, index, levelSteps);
        if (waves[index].entry.groupCount > 0) {
            lastWave = index;
        }
    }
    if (levelSteps != nullptr) {
        std::memcpy(g_levelSteps, levelSteps, lastWave);
    }

    int previousEnemyWave = 0;
    for (int index = 1; index <= lastWave; ++index) {
        const OriginalWave& original = waves[index];
        if (original.entry.groupCount == 0) {
            continue;
        }
        const OriginalWave* source = &original;
        // Bonus waves: no enemies and no level step. The finale (last wave) has no enemies either and stays.
        const bool bonus = original.enemies == 0 && original.levelStep == 0 && index != lastWave;
        if (bonus && settings.replaceBonusWaves && previousEnemyWave != 0) {
            source = &waves[previousEnemyWave];
            g_levelSteps[index - 1] = 1;
        }

        std::vector<Group> groups = source->groups;
        if (source != &original) {
            for (Group& group : groups) {
                group.sound = nullptr;  // the copied wave's intro/outro lines would announce the wrong wave
                std::memset(group.record, 0, kRecordSize);
            }
        }
        // The script runs a group, then waits for a clear unless that group keeps going, then waits its delay. The wave
        // number goes up when the last group (usually the outro line) runs, so waiting after the last enemy group
        // holds the outro and the next wave until the field is clear.
        int lastEnemyGroup = -1;
        for (int group = 0; group < static_cast<int>(groups.size()); ++group) {
            if (!groups[group].units.empty()) {
                lastEnemyGroup = group;
            }
        }
        for (int group = 0; group < static_cast<int>(groups.size()); ++group) {
            groups[group].delay *= settings.delayScale;
            if (settings.keepGoing == 1 || (settings.keepGoing == 2 && group != lastEnemyGroup)) {
                groups[group].keepGoing = 1;
            } else if (settings.keepGoing == 2) {
                groups[group].keepGoing = 0;
            }
        }

        // Extra units repeat the wave's own units in order, each in the group it came from.
        const int enemies = source->enemies;
        const int target = static_cast<int>(std::floor(enemies * std::pow(1.0 + settings.growth, index - 1) + 1e-6));
        std::vector<std::pair<int, Unit>> pool;
        for (int group = 0; group < static_cast<int>(groups.size()); ++group) {
            for (const Unit& unit : groups[group].units) {
                if (std::strcmp(unitName(unit.unit), "GLADIATOR") != 0) {
                    pool.push_back({group, unit});
                }
            }
        }
        for (int extra = 0; !pool.empty() && extra < target - enemies; ++extra) {
            const auto& [group, unit] = pool[extra % pool.size()];
            groups[group].units.push_back(unit);
        }

        writeWave(script, index, source->entry, groups);
        g_api->log("academy_waves: wave %d%s: %d groups, %d -> %d enemies", index,
            source != &original ? " (bonus wave replaced)" : "", static_cast<int>(groups.size()), enemies, enemies > 0 ? std::max(target, enemies) : 0);
        if (original.enemies > 0) {
            previousEnemyWave = index;
        }
    }
    script.levelSteps() = g_levelSteps;
    static const char* const kKeepGoing[] = {"game default", "every group", "every group, waves wait for a clear"};
    g_api->log("academy_waves: keep going: %s, delays x%.2f, +%.0f%% enemies per wave", kKeepGoing[std::clamp(settings.keepGoing, 0, 2)],
        settings.delayScale, settings.growth * 100.0f);
}

} // namespace

extern "C" __declspec(dllexport) int __cdecl CwModInit(const CwModApi* api) {
    if (api->version != CW_MOD_API_VERSION) {
        return 0;
    }
    g_api = api;
    return api->detour(kSetupWaves1, reinterpret_cast<const void*>(&setupWaves), reinterpret_cast<void**>(&g_originalSetup),
        "ThuleAcademyScript_SetupWaves1 (academy_waves)");
}
