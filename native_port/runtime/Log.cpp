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

    std::lock_guard lock(g_logMutex);
    std::fprintf(stdout, "[%5lu] %s\n", GetCurrentThreadId(), buffer);
    std::fflush(stdout);
    if (g_logFile != nullptr) {
        std::fprintf(g_logFile, "[%5lu] %s\n", GetCurrentThreadId(), buffer);
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
