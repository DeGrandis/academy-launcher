#include "Hle.h"

#include "Kernel.h"
#include "Log.h"
#include "game/GameSymbols.h"

#include <cstring>
#include <intrin.h>
#include <windows.h>

namespace cw::hle {

namespace {

// XMountUtilityDrive normally partitions and formats a cache slice of the raw disk; map Z: to a host folder instead.
BOOL __stdcall xXMountUtilityDrive(BOOL formatClusters) {
    kernel::createSymbolicLink("\\??\\Z:", "\\Device\\Harddisk0\\Partition5");
    logf("XMountUtilityDrive(%d) -> Z: mapped to host cache folder", formatClusters);
    return TRUE;
}

// The game's movie player (thiscall, 2 stack args) treats -1 as "movie unavailable" and moves on.
int __fastcall xMovieOpen(void* self, void* unusedEdx, const char* name, DWORD flags) {
    logf("movie: skipping '%s' (XMV playback not implemented)", name != nullptr ? name : "?");
    return -1;
}

// The voice codec refuses to start unless CPUID reports family 6 (the Xbox Pentium III); modern CPUs report other families.
int __cdecl xVoiceCodecCpuCheck() {
    return 0;
}

// The main menu reads a cached Xbox Live record (XONLINES wrapper over the global XOnline object at 0x5F7548). With the
// network offline that object is never created, so the real function fails with 0x80150005 (not initialized); the menu
// then posts an "XLive error" that is never cleared, and the multiplayer pre-game countdown waits on it forever
// (Academy stuck on "GET READY!"). An offline console finds no cached record: clear the 0x9C-byte output, return S_FALSE.
int __stdcall xXOnlineReadCachedRecord(void* record) {
    std::memset(record, 0, 0x9C);
    return 1;
}

// XAPI and the game's timer read RDTSC directly but take its rate from QueryPerformanceFrequency, hard-coded to the 733 MHz Xbox CPU.
BOOL __stdcall xQueryPerformanceFrequency(LARGE_INTEGER* frequency) {
    static const LONGLONG tscFrequency = [] {
        LARGE_INTEGER hostFrequency, start, end;
        QueryPerformanceFrequency(&hostFrequency);
        QueryPerformanceCounter(&start);
        const unsigned long long tscStart = __rdtsc();
        Sleep(100);
        QueryPerformanceCounter(&end);
        const unsigned long long tscEnd = __rdtsc();
        const double seconds = static_cast<double>(end.QuadPart - start.QuadPart) / static_cast<double>(hostFrequency.QuadPart);
        const auto measured = static_cast<LONGLONG>(static_cast<double>(tscEnd - tscStart) / seconds);
        logf("timing: host TSC runs at %.1f MHz; game speed x%g (menus) / x%g (missions)", measured / 1e6, menuTimeScale(), targetTimeScale());
        // CW_TIME_SCALE: a slower reported rate makes every TSC interval count for more game time. The game reads this
        // once, at the menu speed; options::switchTimeScale changes it per mission.
        return static_cast<LONGLONG>(static_cast<double>(measured) / menuTimeScale());
    }();
    frequency->QuadPart = tscFrequency;
    return TRUE;
}

} // namespace

void installXapiHooks() {
    hookFunction(0x001608AE, reinterpret_cast<const void*>(&xXMountUtilityDrive), "XMountUtilityDrive");
    hookFunction(0x00262860, reinterpret_cast<const void*>(&xMovieOpen), "MoviePlayer::Open");
    hookFunction(0x00273A60, reinterpret_cast<const void*>(&xVoiceCodecCpuCheck), "VoiceCodecCpuCheck");
    hookFunction(reinterpret_cast<std::uint32_t>(game::XOnline_ReadCachedRecord), reinterpret_cast<const void*>(&xXOnlineReadCachedRecord), "XOnlineReadCachedRecord");
    hookFunction(0x00160DE9, reinterpret_cast<const void*>(&xQueryPerformanceFrequency), "QueryPerformanceFrequency");
}

} // namespace cw::hle
