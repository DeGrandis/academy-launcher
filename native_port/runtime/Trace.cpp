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

// Reads a pointer and adds an offset; 0 when the pointer cannot be read.
std::uintptr_t followPointer(std::uintptr_t address, std::uintptr_t offset) {
    __try {
        const std::uintptr_t pointer = *reinterpret_cast<const std::uint32_t*>(address);
        return pointer == 0 ? 0 : pointer + offset;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

void formatWatch(char* value, std::size_t size, std::uintptr_t address, char type) {
    __try {
        switch (type) {
        case 'x': snprintf(value, size, " %X=%08X", static_cast<unsigned>(address), *reinterpret_cast<const std::uint32_t*>(address)); break;
        case 'd': {
            const auto* words = reinterpret_cast<const std::uint32_t*>(address);
            snprintf(value, size, " %X=[%08X %08X %08X %08X %08X %08X %08X %08X]", static_cast<unsigned>(address), words[0], words[1], words[2],
                words[3], words[4], words[5], words[6], words[7]);
            break;
        }
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
// "addr*off1*off2..." follows pointers: read the pointer at addr, add off1, read that pointer, add off2, ... (hex).
// Types: i (int32, default), f (float), b (byte), h (int16), s (string), x (hex), d (8 hex dwords).
void startWatch() {
    const char* list = std::getenv("CW_WATCH");
    if (list == nullptr) {
        return;
    }
    struct Watch {
        std::string label;
        std::uintptr_t address;
        char type;
        std::vector<std::uintptr_t> offsets;  // each: dereference, then add
    };
    std::vector<Watch> watches;
    std::string text = list;
    std::size_t start = 0;
    while (start < text.size()) {
        const std::size_t end = std::min(text.find(',', start), text.size());
        const std::string item = text.substr(start, end - start);
        const std::size_t colon = item.find(':');
        char* rest = nullptr;
        const auto address = static_cast<std::uintptr_t>(std::strtoul(item.c_str(), &rest, 16));
        std::vector<std::uintptr_t> offsets;
        while (rest != nullptr && *rest == '*') {
            offsets.push_back(static_cast<std::uintptr_t>(std::strtoul(rest + 1, &rest, 16)));
        }
        if (address != 0) {
            watches.push_back({item.substr(0, colon), address, colon == std::string::npos ? 'i' : item[colon + 1], offsets});
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
                char value[160];
                std::uintptr_t address = watch.address;
                for (std::uintptr_t offset : watch.offsets) {
                    address = address == 0 ? 0 : followPointer(address, offset);
                }
                if (address == 0) {
                    snprintf(value, sizeof(value), " %s=null", watch.label.c_str());
                } else {
                    formatWatch(value, sizeof(value), address, watch.type);
                }
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
    // CW_TRACE_EVERY=n logs only every n-th hit (so long traces sample over time);
    // CW_TRACE_DUMP=<reg>:<bytes> also dumps the memory a register (eax..edi) points to.
    static const int every = [] {
        const char* value = std::getenv("CW_TRACE_EVERY");
        return value == nullptr ? 1 : std::max(1, std::atoi(value));
    }();
    if (point.hits % every == 0) {
        logf("trace %08X #%d: eax=%08lX ebx=%08lX ecx=%08lX edx=%08lX esi=%08lX edi=%08lX ebp=%08lX esp=%08lX | [esp] %08lX %08lX %08lX %08lX %08lX %08lX",
            static_cast<unsigned>(address), point.hits, context->Eax, context->Ebx, context->Ecx, context->Edx, context->Esi, context->Edi,
            context->Ebp, context->Esp, stack[0], stack[1], stack[2], stack[3], stack[4], stack[5]);
        if (const char* dump = std::getenv("CW_TRACE_DUMP")) {
            const std::string spec = dump;
            const std::string reg = spec.substr(0, 3);
            const DWORD bytes = static_cast<DWORD>(std::strtoul(spec.substr(4).c_str(), nullptr, 0));
            const DWORD base = reg == "eax" ? context->Eax : reg == "ebx" ? context->Ebx : reg == "ecx" ? context->Ecx : reg == "edx" ? context->Edx
                : reg == "esi" ? context->Esi : reg == "edi" ? context->Edi : 0;
            for (DWORD offset = 0; base != 0 && offset < bytes; offset += 32) {
                char line[160];
                formatWatch(line, sizeof(line), base + offset, 'd');
                logf("  +%03lX%s", offset, line);
            }
        }
    }

    writeByte(address, point.original);
    if (++point.hits < maxHitsPerTracepoint()) {
        t_rearmAddress = address;
        context->EFlags |= 0x100;
    }
    return true;
}

} // namespace cw::trace
