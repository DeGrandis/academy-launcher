#include "GameOptions.h"

#include "Hooks.h"
#include "Log.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <intrin.h>
#include <iterator>

#include <windows.h>

namespace cw::options {

namespace {

// Each map's .sky file sets a visibility range (far clip plane, object culling, sky dome size) and a fog range;
// missions can change both. CW_VIEW_DISTANCE=<multiplier> scales them; F7 / F8 step it down / up in game.
constexpr std::uint32_t kSkySetVisibilityRange = 0x000BBB20;  // void __cdecl(float range)
constexpr std::uint32_t kSkySetFogRange = 0x000BBB70;         // void __cdecl(float start, float end)
constexpr float kSteps[] = {1.0f, 1.5f, 2.0f, 3.0f, 4.0f, 6.0f, 8.0f, 12.0f, 16.0f};

constexpr std::uint32_t kConfigGetInt = 0x00226B80;
constexpr std::uint32_t kLicenseScreenSeconds = 0x00348C54;  // float 5.5          // int __cdecl(const char* name, int fallback)

void(__cdecl* g_setVisibilityRange)(float) = nullptr;
int(__cdecl* g_configGetInt)(const char*, int) = nullptr;
void(__cdecl* g_setFogRange)(float, float) = nullptr;
float g_multiplier = 1.0f;
bool g_pending = false;
// The game's own (unscaled) values from the last calls, re-applied when the multiplier changes.
float g_visibility = 0.0f;
float g_fogStart = 0.0f;
float g_fogEnd = 0.0f;
bool g_haveVisibility = false;
bool g_haveFog = false;

void __cdecl setVisibilityRange(float range) {
    g_visibility = range;
    g_haveVisibility = true;
    g_setVisibilityRange(range * g_multiplier);
}

void __cdecl setFogRange(float start, float end) {
    g_fogStart = start;
    g_fogEnd = end;
    g_haveFog = true;
    g_setFogRange(start * g_multiplier, end * g_multiplier);
}

// The tiled sky plane (sky flag 0x4, e.g. dm1) is a grid sized from the camera's far distance (+0x2D4), written each
// frame into a fixed-size dynamic buffer. With a scaled visibility range the grid outgrows the buffer and the game
// writes past its end (access violation at 0xBA1D6). Draw that plane with the map's own range; it lies beyond the fog.
constexpr std::uint32_t kSkyDrawPlane = 0x000B9CE0;  // void __cdecl(Camera*)
constexpr std::uint32_t kCameraFarDistance = 0x2D4;
void(__cdecl* g_skyDrawPlane)(std::uint8_t*) = nullptr;
} // namespace
extern bool g_remixSkyPlane;  // d3d/Device.cpp: CW_REMIX_DUMP marks the sky plane's draws "sky"
extern char g_missionName[32];  // d3d/Device.cpp: RTX Remix picks each map's lighting by mission
namespace {

void __cdecl skyDrawPlane(std::uint8_t* camera) {
    // CW_NO_SKY_PLANE=1 (RTX Remix): skip the flat cloud plane drawn high over the map. A path tracer sees it as a solid
    // lid that blocks the sun; Remix draws its own sky from the tagged sky textures.
    // While dumping textures for Remix the plane is drawn anyway, so its textures can be found and tagged.
    static const bool skip = std::getenv("CW_NO_SKY_PLANE") != nullptr && std::getenv("CW_REMIX_DUMP") == nullptr;
    if (skip) {
        return;
    }
    auto* farDistance = reinterpret_cast<float*>(camera + kCameraFarDistance);
    const float saved = *farDistance;
    static bool logged = false;
    if (!logged) {
        logged = true;
        logf("options: sky plane drawn with far distance %.0f (scaled x%g, drawn at %.0f)", saved, g_multiplier, saved / g_multiplier);
    }
    if (g_multiplier > 1.0f) {
        *farDistance = saved / g_multiplier;
    }
    g_remixSkyPlane = true;
    g_skyDrawPlane(camera);
    g_remixSkyPlane = false;
    *farDistance = saved;
}

// With a mod overlay, level caches (Bins/<map>odf.bin, anm.bin) would be recorded on the first load and replayed in
// the same order afterwards; any change in what loads (another vehicle, a changed mod) desynchronizes the replay and
// crashes. config.ini's doBatch turns the caches on; report it as off so every file loads from data.zwp.
// Chase camera: each vehicle class reads [Camera] distance from its ODF at 0x304C7 into class +0x2180 (ebx = the
// class); right after, CW_CAMERA_DISTANCE (default 1.2) scales it.
constexpr std::uint32_t kCameraDistanceRead = 0x000304CC;
float g_cameraDistance = 1.2f;

void __cdecl cameraDistanceRead(hooks::Registers* registers) {
    *reinterpret_cast<float*>(static_cast<std::uintptr_t>(registers->ebx + 0x2180)) *= g_cameraDistance;
}

int __cdecl configGetInt(const char* name, int fallback) {
    if (name != nullptr && std::strcmp(name, "doBatch") == 0 && std::getenv("CW_MOD_ROOT") != nullptr) {
        return 0;
    }
    return g_configGetInt(name, fallback);
}

// Multiplayer name: System Link names each player after their save profile. Two places read it:
// - 0xDFEB0 returns the network player name (Xbox Live gamertag when [0x5A2858] == 2, else the current profile record,
//   whose first field is the wide-string name). The host copies it into its player table (0x542400, 0x40 bytes) when
//   it starts a game, and a client sends 0x40 bytes of it in its join request.
// - The Create Game screen (0x139F80) calls the profile getter at 0x13A06F for "Session by <name>" in System Link mode.
// CW_PLAYER_NAME replaces both; the profile and its save keep their own name.
constexpr std::uint32_t kNetPlayerName = 0x000DFEB0;     // const wchar_t* __cdecl()
constexpr std::uint32_t kLiveGamertag = 0x000DF160;      // const wchar_t* __cdecl()
constexpr std::uint32_t kNetMode = 0x005A2858;           // 1 = System Link, 2 = Xbox Live
constexpr std::uint32_t kSessionTitleNameCall = 0x0013A06F;  // call Profile_Current
constexpr std::size_t kMaxPlayerName = 15;
wchar_t g_playerName[0x20] = {};  // 0x40 bytes, as the join request sends

const wchar_t* __cdecl netPlayerName() {
    if (*reinterpret_cast<const int*>(kNetMode) == 2) {
        return reinterpret_cast<const wchar_t*(__cdecl*)()>(kLiveGamertag)();
    }
    return g_playerName;
}

const wchar_t* __cdecl sessionTitleName() {
    return g_playerName;
}

void installPlayerName() {
    const char* value = std::getenv("CW_PLAYER_NAME");
    if (value == nullptr || *value == '\0') {
        return;
    }
    wchar_t name[64] = {};
    MultiByteToWideChar(CP_UTF8, 0, value, -1, name, static_cast<int>(std::size(name)) - 1);
    std::size_t length = 0;
    for (const wchar_t* c = name; *c != 0 && length < kMaxPlayerName; ++c) {
        if (*c >= 0x20 && *c < 0x7F) {  // the game's fonts only have ASCII
            g_playerName[length++] = *c;
        }
    }
    if (length == 0) {
        return;
    }
    hooks::detour(kNetPlayerName, reinterpret_cast<const void*>(&netPlayerName), nullptr, "network player name (CW_PLAYER_NAME)");
    std::uint8_t call[5] = {0xE8};
    const std::int32_t relative = static_cast<std::int32_t>(reinterpret_cast<std::uintptr_t>(&sessionTitleName) - (kSessionTitleNameCall + 5));
    std::memcpy(call + 1, &relative, 4);
    hooks::patchBytes(kSessionTitleNameCall, call, sizeof(call), "session title name (CW_PLAYER_NAME)");
    logf("options: multiplayer name '%ls'", g_playerName);
}

// CW_TIME_SCALE: missions run at targetTimeScale(), the menus at menuTimeScale(). The game's timer is
// (rdtsc - base) * secondsPerTick, base at 0x608DD8 and secondsPerTick (double) at 0x608DE0, both set once at startup
// from QueryPerformanceFrequency (which reports the menu speed); changing speed rescales secondsPerTick and moves base
// so the game time read right now stays the same.
constexpr std::uint32_t kBatchBeginMission = 0x00062750;  // void __cdecl(const char* mission, const char* directory)
constexpr std::uint32_t kTimerBase = 0x00608DD8;
constexpr std::uint32_t kTimerSecondsPerTick = 0x00608DE0;
void(__cdecl* g_beginMission)(const char*, const char*) = nullptr;

void switchTimeScale(double scale) {
    const double current = timeScale();
    auto& base = *reinterpret_cast<unsigned long long*>(kTimerBase);
    auto& secondsPerTick = *reinterpret_cast<double*>(kTimerSecondsPerTick);
    if (scale == current || secondsPerTick == 0.0) {
        return;
    }
    const unsigned long long now = __rdtsc();
    const double gameNow = static_cast<double>(static_cast<long long>(now - base)) * secondsPerTick;
    secondsPerTick *= scale / current;
    base = now - static_cast<unsigned long long>(gameNow / secondsPerTick);
    setTimeScale(scale);
    logf("timing: game speed x%g", scale);
}

void __cdecl beginMission(const char* mission, const char* directory) {
    const bool menus = mission == nullptr || _strnicmp(mission, "shell", 5) == 0;
    logf("options: mission '%s'", mission != nullptr ? mission : "(none)");  // automated runs check where the menus led
    switchTimeScale(menus ? menuTimeScale() : targetTimeScale());
    strncpy_s(g_missionName, mission != nullptr ? mission : "", _TRUNCATE);
    g_beginMission(mission, directory);
}

} // namespace

void install() {
    installPlayerName();
    hooks::detour(kBatchBeginMission, reinterpret_cast<const void*>(&beginMission), reinterpret_cast<void**>(&g_beginMission),
        "Batch_BeginMission (mission log, CW_TIME_SCALE per mission)");
    if (const char* value = std::getenv("CW_VIEW_DISTANCE")) {
        g_multiplier = std::clamp(static_cast<float>(std::atof(value)), 0.25f, 64.0f);
    }
    hooks::detour(kSkySetVisibilityRange, reinterpret_cast<const void*>(&setVisibilityRange), reinterpret_cast<void**>(&g_setVisibilityRange),
        "Sky_SetVisibilityRange (view distance)");
    hooks::detour(kSkySetFogRange, reinterpret_cast<const void*>(&setFogRange), reinterpret_cast<void**>(&g_setFogRange),
        "Sky_SetFogRange (view distance)");
    hooks::detour(kSkyDrawPlane, reinterpret_cast<const void*>(&skyDrawPlane), reinterpret_cast<void**>(&g_skyDrawPlane),
        "Sky_DrawPlane (unscaled range)");
    hooks::detour(kConfigGetInt, reinterpret_cast<const void*>(&configGetInt), reinterpret_cast<void**>(&g_configGetInt), "Config_GetInt (no level caches with mods)");
    // The license/copyright screen ("LIC") stays up for a fixed 5.5 s before the logo movies; the constant is only used
    // by that screen. CW_SKIP_INTRO=0 keeps it.
    const char* skipIntro = std::getenv("CW_SKIP_INTRO");
    if (skipIntro == nullptr || std::strcmp(skipIntro, "0") != 0) {
        const float zero = 0.0f;
        hooks::patchBytes(kLicenseScreenSeconds, &zero, sizeof(zero), "license screen duration (skip intro)");
    }
    if (const char* value = std::getenv("CW_CAMERA_DISTANCE")) {
        g_cameraDistance = std::clamp(static_cast<float>(std::atof(value)), 0.25f, 8.0f);
    }
    if (g_cameraDistance != 1.0f) {
        hooks::midHook(kCameraDistanceRead, &cameraDistanceRead, "camera distance (CW_CAMERA_DISTANCE)");
    }
    logf("options: view distance x%g, camera distance x%g", g_multiplier, g_cameraDistance);
}

void stepViewDistance(int direction) {
    const float* current = std::lower_bound(std::begin(kSteps), std::end(kSteps), g_multiplier - 0.001f);
    std::ptrdiff_t index = current - std::begin(kSteps);
    if (direction > 0 && current != std::end(kSteps) && *current <= g_multiplier + 0.001f) {
        ++index;
    } else if (direction < 0) {
        --index;
    }
    index = std::clamp<std::ptrdiff_t>(index, 0, std::size(kSteps) - 1);
    g_multiplier = kSteps[index];
    g_pending = true;
    logf("options: view distance -> x%g", g_multiplier);
}

void applyPending() {
    if (!g_pending) {
        return;
    }
    g_pending = false;
    if (g_haveVisibility) {
        g_setVisibilityRange(g_visibility * g_multiplier);
    }
    if (g_haveFog) {
        g_setFogRange(g_fogStart * g_multiplier, g_fogEnd * g_multiplier);
    }
}

float viewDistance() {
    return g_multiplier;
}

} // namespace cw::options
