#pragma once

#include <cstdarg>

namespace cw {

void logInit(const wchar_t* path);
void logf(const char* format, ...);
void logv(const char* format, va_list args);
[[noreturn]] void fatal(const char* format, ...);

} // namespace cw
