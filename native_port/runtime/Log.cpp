#include "Log.h"

#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <share.h>
#include <mutex>

namespace cw {

namespace {

std::mutex g_logMutex;
FILE* g_logFile = nullptr;

} // namespace

void logInit(const wchar_t* path) {
    std::lock_guard lock(g_logMutex);
    // Allow other processes to read the log while the game is running.
    g_logFile = _wfsopen(path, L"w", _SH_DENYWR);
}

namespace {

double scaleSetting(const char* name, double fallback) {
    const char* value = std::getenv(name);
    const double parsed = value != nullptr ? std::strtod(value, nullptr) : fallback;
    return parsed > 0.0 ? parsed : fallback;
}

// The game clock is piecewise linear in real time: since `wallStart` (QPC ticks) it has run at `scale` from
// `gameStart` (seconds). setTimeScale starts a new piece, so game time never jumps.
struct ClockPiece {
    LONGLONG wallStart;
    double gameStart;
    double scale;
};
std::mutex g_clockMutex;
ClockPiece g_clock{0, 0.0, 0.0};

LONGLONG qpcNow() {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return now.QuadPart;
}

double qpcFrequency() {
    static const double frequency = [] {
        LARGE_INTEGER value;
        QueryPerformanceFrequency(&value);
        return static_cast<double>(value.QuadPart);
    }();
    return frequency;
}

ClockPiece& clockPiece() {
    if (g_clock.scale == 0.0) {
        g_clock = {qpcNow(), 0.0, menuTimeScale()};
    }
    return g_clock;
}

} // namespace

double targetTimeScale() {
    static const double scale = scaleSetting("CW_TIME_SCALE", 1.0);
    return scale;
}

double menuTimeScale() {
    static const double scale = scaleSetting("CW_MENU_TIME_SCALE", targetTimeScale() < 4.0 ? targetTimeScale() : 4.0);
    return scale;
}

double timeScale() {
    std::lock_guard lock(g_clockMutex);
    return clockPiece().scale;
}

double gameSeconds() {
    std::lock_guard lock(g_clockMutex);
    const ClockPiece& piece = clockPiece();
    return piece.gameStart + static_cast<double>(qpcNow() - piece.wallStart) / qpcFrequency() * piece.scale;
}

void setTimeScale(double scale) {
    std::lock_guard lock(g_clockMutex);
    ClockPiece& piece = clockPiece();
    if (scale <= 0.0 || scale == piece.scale) {
        return;
    }
    const LONGLONG now = qpcNow();
    piece = {now, piece.gameStart + static_cast<double>(now - piece.wallStart) / qpcFrequency() * piece.scale, scale};
}

unsigned long long gameMilliseconds() {
    return static_cast<unsigned long long>(gameSeconds() * 1000.0);
}

void logv(const char* format, va_list args) {
    char buffer[2048];
    vsnprintf(buffer, sizeof(buffer), format, args);

    static const ULONGLONG start = gameMilliseconds();
    const auto elapsed = static_cast<unsigned long>(gameMilliseconds() - start);
    std::lock_guard lock(g_logMutex);
    // [thread] seconds.milliseconds since the first log line
    std::fprintf(stdout, "[%5lu] %4lu.%03lu %s\n", GetCurrentThreadId(), elapsed / 1000, elapsed % 1000, buffer);
    std::fflush(stdout);
    if (g_logFile != nullptr) {
        std::fprintf(g_logFile, "[%5lu] %4lu.%03lu %s\n", GetCurrentThreadId(), elapsed / 1000, elapsed % 1000, buffer);
        std::fflush(g_logFile);
    }
}

void logf(const char* format, ...) {
    va_list args;
    va_start(args, format);
    logv(format, args);
    va_end(args);
}

void fatal(const char* format, ...) {
    va_list args;
    va_start(args, format);
    logv(format, args);
    va_end(args);
    logf("fatal error; terminating");
    ExitProcess(1);
}

} // namespace cw
