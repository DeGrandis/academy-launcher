#include <windows.h>

#include "Trace.h"

#include "Log.h"

#include <cstdlib>
#include <algorithm>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace cw::trace {

namespace {

struct Tracepoint {
    std::uint8_t original = 0;
    int hits = 0;
};

// CW_TRACE_HITS overrides how many hits of each tracepoint are logged.
int maxHitsPerTracepoint() {
    static const int hits = [] {
        const char* value = std::getenv("CW_TRACE_HITS");
        return value == nullptr ? 8 : std::atoi(value);
    }();
    return hits;
}
std::mutex g_mutex;
std::map<std::uintptr_t, Tracepoint> g_tracepoints;
thread_local std::uintptr_t t_rearmAddress = 0;

void writeByte(std::uintptr_t address, std::uint8_t value) {
    DWORD oldProtect;
    VirtualProtect(reinterpret_cast<void*>(address), 1, PAGE_EXECUTE_READWRITE, &oldProtect);
    *reinterpret_cast<volatile std::uint8_t*>(address) = value;
    VirtualProtect(reinterpret_cast<void*>(address), 1, oldProtect, &oldProtect);
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(address), 1);
}

void formatWatch(char* value, std::size_t size, std::uintptr_t address, char type) {
    __try {
        switch (type) {
        case 'f': snprintf(value, size, " %X=%g", static_cast<unsigned>(address), *reinterpret_cast<const float*>(address)); break;
        case 'b': snprintf(value, size, " %X=%u", static_cast<unsigned>(address), *reinterpret_cast<const std::uint8_t*>(address)); break;
        case 's': snprintf(value, size, " %X='%.48s'", static_cast<unsigned>(address), reinterpret_cast<const char*>(address)); break;
        case 'h': snprintf(value, size, " %X=%d", static_cast<unsigned>(address), *reinterpret_cast<const std::int16_t*>(address)); break;
        default: snprintf(value, size, " %X=%d", static_cast<unsigned>(address), *reinterpret_cast<const std::int32_t*>(address)); break;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        snprintf(value, size, " %X=?", static_cast<unsigned>(address));
    }
}

// CW_WATCH="addr[:type],..." logs the values at the addresses every CW_WATCH_MS (default 1000) milliseconds.
// Types: i (int32, default), f (float), b (byte), h (int16), s (string).
void startWatch() {
    const char* list = std::getenv("CW_WATCH");
    if (list == nullptr) {
        return;
    }
    struct Watch {
        std::uintptr_t address;
        char type;
    };
    std::vector<Watch> watches;
    std::string text = list;
    std::size_t start = 0;
    while (start < text.size()) {
        const std::size_t end = std::min(text.find(',', start), text.size());
        const std::string item = text.substr(start, end - start);
        const std::size_t colon = item.find(':');
        const auto address = static_cast<std::uintptr_t>(std::strtoul(item.c_str(), nullptr, 16));
        if (address != 0) {
            watches.push_back({address, colon == std::string::npos ? 'i' : item[colon + 1]});
        }
        start = end + 1;
    }
    const char* periodSetting = std::getenv("CW_WATCH_MS");
    const DWORD period = periodSetting == nullptr ? 1000 : static_cast<DWORD>(std::strtoul(periodSetting, nullptr, 10));
    std::thread([watches, period] {
        for (;;) {
            Sleep(period);
            std::string line = "watch:";
            for (const Watch& watch : watches) {
                char value[64];
                formatWatch(value, sizeof(value), watch.address, watch.type);
                line += value;
            }
            logf("%s", line.c_str());
        }
    }).detach();
}

} // namespace

void installFromEnvironment() {
    startWatch();
    const char* list = std::getenv("CW_TRACE");
    if (list == nullptr) {
        return;
    }
    std::string text = list;
    std::size_t start = 0;
    while (start < text.size()) {
        const std::size_t end = text.find(',', start);
        const std::string item = text.substr(start, end == std::string::npos ? std::string::npos : end - start);
        const auto address = static_cast<std::uintptr_t>(std::strtoul(item.c_str(), nullptr, 16));
        if (address != 0) {
            Tracepoint point;
            point.original = *reinterpret_cast<std::uint8_t*>(address);
            g_tracepoints[address] = point;
            writeByte(address, 0xCC);
            logf("trace: tracepoint at %08X", static_cast<unsigned>(address));
        }
        if (end == std::string::npos) {
            break;
        }
        start = end + 1;
    }
}

bool handle(EXCEPTION_POINTERS* info) {
    CONTEXT* context = info->ContextRecord;
    const DWORD code = info->ExceptionRecord->ExceptionCode;

    if (code == EXCEPTION_SINGLE_STEP && t_rearmAddress != 0) {
        writeByte(t_rearmAddress, 0xCC);
        t_rearmAddress = 0;
        return true;
    }
    if (code != EXCEPTION_BREAKPOINT) {
        return false;
    }

    const auto address = static_cast<std::uintptr_t>(context->Eip);
    std::lock_guard lock(g_mutex);
    auto entry = g_tracepoints.find(address);
    if (entry == g_tracepoints.end()) {
        return false;
    }
    Tracepoint& point = entry->second;
    const auto* stack = reinterpret_cast<const DWORD*>(static_cast<std::uintptr_t>(context->Esp));
    logf("trace %08X #%d: eax=%08lX ebx=%08lX ecx=%08lX edx=%08lX esi=%08lX edi=%08lX ebp=%08lX esp=%08lX | [esp] %08lX %08lX %08lX %08lX %08lX %08lX",
        static_cast<unsigned>(address), point.hits, context->Eax, context->Ebx, context->Ecx, context->Edx, context->Esi, context->Edi,
        context->Ebp, context->Esp, stack[0], stack[1], stack[2], stack[3], stack[4], stack[5]);

    writeByte(address, point.original);
    if (++point.hits < maxHitsPerTracepoint()) {
        t_rearmAddress = address;
        context->EFlags |= 0x100;
    }
    return true;
}

} // namespace cw::trace
