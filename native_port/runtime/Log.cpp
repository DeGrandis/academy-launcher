#include "Log.h"

#include <windows.h>

#include <cstdio>
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

void logv(const char* format, va_list args) {
    char buffer[2048];
    vsnprintf(buffer, sizeof(buffer), format, args);

    static const ULONGLONG start = GetTickCount64();
    const auto elapsed = static_cast<unsigned long>(GetTickCount64() - start);
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
