#include "Threads.h"

#include "Log.h"

#include <cstring>
#include <mutex>
#include <vector>

namespace cw::threads {

namespace {

DWORD g_kpcrSlot = TLS_OUT_OF_INDEXES;
std::mutex g_threadsMutex;
std::vector<std::pair<DWORD, HANDLE>> g_xboxThreads;
constexpr std::uintptr_t kImageStart = 0x10000;
constexpr std::uintptr_t kImageEnd = 0x6434C8;

// Every non-FS:[0] segment access in default.xbe, exported from Ghidra (analysis_exports/fs_accesses.csv).
constexpr std::uint32_t kSegmentAccessSites[] = {
    0x0015e2f4, 0x0015e48a, 0x00160e50, 0x00160e6c, 0x00160e72, 0x00160f2c, 0x00160f38, 0x00160f43,
    0x00160f54, 0x00160f60, 0x00160f66, 0x00164168, 0x001641df, 0x00164a76, 0x00164aab, 0x001655a7,
    0x001655f3, 0x001656c0, 0x001656cc, 0x001656e9, 0x002686c9, 0x002686df, 0x002686f0, 0x00268757,
    0x0026876b, 0x0026877c, 0x00268888, 0x002707ed, 0x002709d2, 0x002ab1fe, 0x002b60f3, 0x002da4a5,
    0x002f4857, 0x002f489a, 0x002f48bf, 0x002f4930, 0x002f9c97,
};

struct DecodedAccess {
    std::uint8_t reg;
    std::uint32_t offset;
    std::uint32_t length;
    bool zeroExtendByte;
};

bool decode(const std::uint8_t* code, DecodedAccess& out) {
    if (code[0] != 0x64) {
        return false;
    }
    if (code[1] == 0xA1) {
        out = {0, *reinterpret_cast<const std::uint32_t*>(code + 2), 6, false};
        return true;
    }
    if (code[1] == 0x8B && (code[2] & 0xC7) == 0x05) {
        out = {static_cast<std::uint8_t>((code[2] >> 3) & 7), *reinterpret_cast<const std::uint32_t*>(code + 3), 7, false};
        return true;
    }
    if (code[1] == 0x0F && code[2] == 0xB6 && (code[3] & 0xC7) == 0x05) {
        out = {static_cast<std::uint8_t>((code[3] >> 3) & 7), *reinterpret_cast<const std::uint32_t*>(code + 4), 8, true};
        return true;
    }
    return false;
}

void emit32(std::vector<std::uint8_t>& code, std::uint32_t value) {
    for (int shift = 0; shift < 32; shift += 8) {
        code.push_back(static_cast<std::uint8_t>(value >> shift));
    }
}

} // namespace

void initializeProcess() {
    g_kpcrSlot = TlsAlloc();
    if (g_kpcrSlot == TLS_OUT_OF_INDEXES || g_kpcrSlot >= 64) {
        fatal("could not allocate a direct TEB TLS slot for the Xbox KPCR");
    }
}

void attachCurrentThread(std::uint32_t tlsDataSize, UCHAR irql) {
    auto* kpcr = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, kKpcrSize + kKthreadSize, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    const std::uint32_t tlsSize = tlsDataSize == 0 ? 0x1000 : tlsDataSize;
    auto* tls = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, tlsSize, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (kpcr == nullptr || tls == nullptr) {
        fatal("out of memory creating Xbox thread state");
    }
    std::uint8_t* kthread = kpcr + kKpcrSize;

    // XAPI locates the TLS block at StackBase - TlsDataSize, so the fake StackBase sits just above it.
    *reinterpret_cast<std::uint8_t**>(kpcr + kKpcrStackBase) = tls + tlsSize;
    *reinterpret_cast<std::uint8_t**>(kpcr + kKpcrSelf) = kpcr;
    *reinterpret_cast<std::uint8_t**>(kpcr + 0x1C) = kpcr;
    *reinterpret_cast<std::uint8_t**>(kpcr + kKpcrPrcb) = kpcr + 0x28;
    kpcr[kKpcrIrql] = irql;
    *reinterpret_cast<std::uint8_t**>(kpcr + kKpcrCurrentThread) = kthread;
    *reinterpret_cast<std::uint8_t**>(kthread + kKthreadTlsData) = tls;
    *reinterpret_cast<DWORD*>(kthread + kEthreadUniqueThread) = GetCurrentThreadId();

    TlsSetValue(g_kpcrSlot, kpcr);

    HANDLE self = nullptr;
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &self, 0, FALSE, DUPLICATE_SAME_ACCESS);
    std::lock_guard lock(g_threadsMutex);
    g_xboxThreads.emplace_back(GetCurrentThreadId(), self);
}

std::uint8_t* currentKpcr() {
    return static_cast<std::uint8_t*>(TlsGetValue(g_kpcrSlot));
}

UCHAR currentIrql() {
    std::uint8_t* kpcr = currentKpcr();
    return kpcr == nullptr ? 0 : kpcr[kKpcrIrql];
}

void setCurrentIrql(UCHAR irql) {
    if (std::uint8_t* kpcr = currentKpcr()) {
        kpcr[kKpcrIrql] = irql;
    }
}

void patchSegmentAccesses() {
    auto* thunks = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, 0x1000, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE));
    std::uint8_t* cursor = thunks;
    const std::uint32_t slotOffset = 0xE10 + g_kpcrSlot * 4;

    for (std::uint32_t site : kSegmentAccessSites) {
        auto* code = reinterpret_cast<std::uint8_t*>(static_cast<std::uintptr_t>(site));
        DecodedAccess access{};
        if (!decode(code, access)) {
            fatal("unexpected instruction bytes at FS access site 0x%08X", site);
        }

        // Thunk: mov reg, fs:[kpcr slot]; mov/movzx reg, [reg + offset]; ret
        std::vector<std::uint8_t> thunk = {0x64, 0x8B, static_cast<std::uint8_t>(0x05 | (access.reg << 3))};
        emit32(thunk, slotOffset);
        if (access.zeroExtendByte) {
            thunk.push_back(0x0F);
            thunk.push_back(0xB6);
        } else {
            thunk.push_back(0x8B);
        }
        thunk.push_back(static_cast<std::uint8_t>(0x80 | (access.reg << 3) | access.reg));
        emit32(thunk, access.offset);
        thunk.push_back(0xC3);

        std::memcpy(cursor, thunk.data(), thunk.size());
        const std::int32_t relative = static_cast<std::int32_t>(cursor - (code + 5));
        code[0] = 0xE8;
        std::memcpy(code + 1, &relative, 4);
        for (std::uint32_t index = 5; index < access.length; ++index) {
            code[index] = 0x90;
        }
        cursor += thunk.size();
    }

    FlushInstructionCache(GetCurrentProcess(), nullptr, 0);
    logf("patched %zu Xbox FS segment accesses", std::size(kSegmentAccessSites));
}

namespace {

bool readable(std::uintptr_t address) {
    MEMORY_BASIC_INFORMATION info{};
    return VirtualQuery(reinterpret_cast<void*>(address), &info, sizeof(info)) != 0 && info.State == MEM_COMMIT &&
        (info.Protect & (PAGE_NOACCESS | PAGE_GUARD)) == 0;
}

DWORD WINAPI watchdogThread(void* parameter) {
    const DWORD intervalMs = static_cast<DWORD>(reinterpret_cast<std::uintptr_t>(parameter));
    while (true) {
        Sleep(intervalMs);
        std::lock_guard lock(g_threadsMutex);
        for (const auto& [threadId, handle] : g_xboxThreads) {
            if (WaitForSingleObject(handle, 0) == WAIT_OBJECT_0 || SuspendThread(handle) == static_cast<DWORD>(-1)) {
                continue;
            }
            CONTEXT context{};
            context.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
            if (GetThreadContext(handle, &context)) {
                char frames[512] = {};
                int length = 0;
                std::uintptr_t ebp = context.Ebp;
                for (int depth = 0; depth < 16 && readable(ebp) && readable(ebp + 4); ++depth) {
                    const std::uintptr_t returnAddress = *reinterpret_cast<std::uintptr_t*>(ebp + 4);
                    if (returnAddress >= kImageStart && returnAddress < kImageEnd) {
                        length += snprintf(frames + length, sizeof(frames) - length, " %08X", static_cast<unsigned>(returnAddress));
                    }
                    const std::uintptr_t next = *reinterpret_cast<std::uintptr_t*>(ebp);
                    if (next <= ebp) {
                        break;
                    }
                    ebp = next;
                }
                logf("watchdog: thread %lu eip=%08lX esp=%08lX eax=%08lX ecx=%08lX frames:%s", threadId, context.Eip, context.Esp,
                    context.Eax, context.Ecx, frames);
                if (context.Eip < kImageStart || context.Eip >= kImageEnd) {
                    HMODULE module = nullptr;
                    wchar_t moduleName[MAX_PATH] = L"?";
                    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(static_cast<std::uintptr_t>(context.Eip)), &module)) {
                        GetModuleFileNameW(module, moduleName, MAX_PATH);
                    }
                    char scan[512] = {};
                    int scanLength = 0;
                    int found = 0;
                    for (std::uintptr_t slot = context.Esp; found < 24 && readable(slot) && slot < context.Esp + 0x10000; slot += 4) {
                        const std::uintptr_t value = *reinterpret_cast<std::uintptr_t*>(slot);
                        if (value >= 0x11000 && value < 0x2A2E00 && (value & 0xFFFF0000) != 0x00010000 && value > 0x20000) {
                            scanLength += snprintf(scan + scanLength, sizeof(scan) - scanLength, " %08X", static_cast<unsigned>(value));
                            ++found;
                        }
                    }
                    logf("  in %ls+0x%lX; game code addresses on stack:%s", moduleName,
                        static_cast<unsigned long>(context.Eip - reinterpret_cast<std::uintptr_t>(module)), scan);
                    std::wstring text;
                    for (std::uintptr_t slot = context.Esp; readable(slot) && slot < context.Esp + 0x10000; slot += 2) {
                        const wchar_t value = *reinterpret_cast<wchar_t*>(slot);
                        if ((value >= 0x20 && value < 0x7F) || value == L'\n') {
                            text.push_back(value);
                        } else if (text.size() >= 24) {
                            logf("  stack text: %ls", text.c_str());
                            text.clear();
                        } else {
                            text.clear();
                        }
                    }
                }
            }
            ResumeThread(handle);
        }
    }
}

} // namespace

void logCallChain(const char* reason) {
    CONTEXT context{};
    RtlCaptureContext(&context);
    logCallChainFrom(reason, context.Ebp);
}

void logCallChainFrom(const char* reason, std::uintptr_t ebp) {
    char frames[512] = {};
    int length = 0;
    for (int depth = 0; depth < 32 && readable(ebp) && readable(ebp + 4); ++depth) {
        const std::uintptr_t returnAddress = *reinterpret_cast<std::uintptr_t*>(ebp + 4);
        if (returnAddress >= kImageStart && returnAddress < kImageEnd) {
            length += snprintf(frames + length, sizeof(frames) - length, " %08X", static_cast<unsigned>(returnAddress));
        }
        const std::uintptr_t next = *reinterpret_cast<std::uintptr_t*>(ebp);
        if (next <= ebp) {
            break;
        }
        ebp = next;
    }
    logf("%s: xbe call chain:%s", reason, frames);
}

void startWatchdog(DWORD intervalMs) {
    CloseHandle(CreateThread(nullptr, 0, &watchdogThread, reinterpret_cast<void*>(static_cast<std::uintptr_t>(intervalMs)), 0, nullptr));
}

} // namespace cw::threads
