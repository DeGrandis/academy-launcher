#pragma once

#include <cstdarg>

namespace cw {

void logInit(const wchar_t* path);
void logf(const char* format, ...);
void logv(const char* format, va_list args);
[[noreturn]] void fatal(const char* format, ...);

// Game clock. CW_TIME_SCALE=n (tests) runs missions n times faster than real time, and the menus at
// CW_MENU_TIME_SCALE (default min(n, 4), where the menus reliably keep up with scripted input). The game's own timer,
// the kernel clocks, the frame-rate cap and log timestamps follow the current speed; input/hotkey scripts and
// CW_EXIT_MS count rendered frames (d3d::scriptMilliseconds) whenever CW_TIME_SCALE is set. Default 1.
double timeScale();        // current speed
double targetTimeScale();  // CW_TIME_SCALE
double menuTimeScale();    // CW_MENU_TIME_SCALE
void setTimeScale(double scale);  // changes speed without a jump in game time (options switch it per mission)
double gameSeconds();      // game time since the process started
unsigned long long gameMilliseconds();

} // namespace cw
