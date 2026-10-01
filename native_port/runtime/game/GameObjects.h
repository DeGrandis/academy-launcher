// Typed views of game objects, for the runtime and plugins. Hand-written from reverse engineering (see
// symbols/manual.csv); offsets verified at runtime in Thule Moon Academy.
#pragma once

#include "game/GameSymbols.h"

#include <cstdint>

namespace cw::game {

constexpr std::uint32_t kHashHealthComponent = 0xFBCD164A;  // GetComponent() key of the health/ammo block

// An in-game object (GameObject, vtable 0x3353F8 for the player's craft).
//   +0x04 flags (bit 8: alive)   +0x20 Physics component   +0x24 orientation quaternion (x, y, z, w)
//   +0x40 world matrix: right (+0x40), up (+0x50), front (+0x60), position (+0x70)   +0xC0 team
struct GameObject {
    std::uint8_t* self = nullptr;

    explicit operator bool() const { return self != nullptr; }
    template <typename T> T& at(std::uint32_t offset) const { return *reinterpret_cast<T*>(self + offset); }

    static GameObject fromHandle(int handle) { return {static_cast<std::uint8_t*>(reinterpret_cast<void*(__cdecl*)(int)>(0x00014340)(handle))}; }

    // The object a player slot (0..7) controls, or null.
    static GameObject player(int slot) {
        auto* world = *reinterpret_cast<std::uint8_t**>(0x003A2F0C);
        if (world == nullptr || slot < 0 || slot > 7) {
            return {};
        }
        auto* players = *reinterpret_cast<std::uint8_t***>(world + 0x14);
        return {players != nullptr ? players[slot] : nullptr};
    }

    bool alive() const { return (at<std::uint32_t>(0x04) >> 8) & 1; }
    float* quaternion() const { return &at<float>(0x24); }
    float* matrix() const { return &at<float>(0x40); }   // 4x4, row-major
    float* position() const { return &at<float>(0x70); } // x, y, z
    float* front() const { return &at<float>(0x60); }
    std::int32_t& team() const { return at<std::int32_t>(0xC0); }

    // Virtual GetComponent(hash) (vtable slot 16, __thiscall).
    std::uint8_t* component(std::uint32_t hash) const {
        auto function = reinterpret_cast<std::uint8_t*(__fastcall*)(void*, void*, std::uint32_t)>((*reinterpret_cast<void***>(self))[16]);
        return function(self, nullptr, hash);
    }
    // Health block: +0x14 current, +0x18 maximum.
    float* healthBlock() const {
        auto* block = component(kHashHealthComponent);
        return block != nullptr ? reinterpret_cast<float*>(block) : nullptr;
    }
    float health() const { auto* block = healthBlock(); return block != nullptr ? block[0x14 / 4] : 0.0f; }
    float maxHealth() const { auto* block = healthBlock(); return block != nullptr ? block[0x18 / 4] : 0.0f; }
    void setHealth(float current, float maximum) const {
        if (auto* block = healthBlock()) {
            block[0x18 / 4] = maximum;
            block[0x14 / 4] = current < maximum ? current : maximum;
        }
    }
};

// The Thule Moon Academy mode script (g_missionScript while multi5 Academy runs; vtable 0x360220, 0x4D8 bytes).
struct ThuleAcademyScript {
    static constexpr std::uint32_t kVtable = 0x00360220;
    static constexpr int kMaxWaves = 30;
    static constexpr int kMaxEnemies = 30;

    std::uint8_t* self = nullptr;

    // The running Academy script, or null when another mode/mission is active.
    static ThuleAcademyScript current() {
        auto* script = *reinterpret_cast<std::uint8_t**>(0x006066B8);
        return {script != nullptr && *reinterpret_cast<std::uint32_t*>(script) == kVtable ? script : nullptr};
    }
    explicit operator bool() const { return self != nullptr; }
    template <typename T> T& at(std::uint32_t offset) const { return *reinterpret_cast<T*>(self + offset); }

    // Wave table entry (0x20 bytes at +0x24 + wave * 0x20; wave 0 is empty). See tools/re/academy_waves.py.
    struct Wave {
        std::int32_t groupCount;
        const std::int8_t* spawnList;   // (unit index, spawn index) pairs; a null unit ends a group
        const float* delays;            // seconds before each group
        const char* const* sounds;      // voice/sound cue per group (may be null)
        const std::uint8_t* pathEffects;// 0x14-byte records per group
        const std::uint8_t* waitForClear;
        const void* startEffects;
        const void* stopEffects;
    };
    static_assert(sizeof(Wave) == 0x20);

    Wave& wave(int index) const { return at<Wave>(0x24 + index * 0x20); }
    std::int32_t& currentWave() const { return at<std::int32_t>(0x498); }
    std::int32_t& currentGroup() const { return at<std::int32_t>(0x494); }
    std::int32_t& loops() const { return at<std::int32_t>(0x4B0); }        // times all waves were cleared
    std::int32_t& waveSet() const { return at<std::int32_t>(0x4A4); }      // player count the tables were built for
    float& nextGroupTime() const { return at<float>(0x3FC); }
    std::int32_t* enemies() const { return &at<std::int32_t>(0x410); }      // handles of live enemies (0 = free)
    const std::uint8_t*& levelSteps() const { return at<const std::uint8_t*>(0x3E4); }  // added to g_academyLevel per wave
};

inline std::int32_t& academyLevel() { return *reinterpret_cast<std::int32_t*>(0x0041B87C); }
inline std::int32_t& mpPlayerCount() { return *reinterpret_cast<std::int32_t*>(0x005EC72C); }

} // namespace cw::game
