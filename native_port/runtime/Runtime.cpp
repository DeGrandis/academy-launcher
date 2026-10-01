#include "GameOptions.h"
#include "Hle.h"
#include "Kernel.h"
#include "Log.h"
#include "ModLoader.h"
#include "Threads.h"
#include "Trace.h"

#include <windows.h>

#include <filesystem>
#include <fstream>
#include <string>

namespace {

constexpr std::uintptr_t kXbeBase = 0x10000;
constexpr std::uint32_t kEntryRetailKey = 0xA8FC57AB;
constexpr std::uint32_t kEntryDebugKey = 0x94859D4B;
constexpr std::uint32_t kThunkRetailKey = 0x5B6D40B6;
constexpr std::uint32_t kThunkDebugKey = 0xEFB1F152;

std::uint32_t readHeader32(std::uint32_t offset) {
    return *reinterpret_cast<const std::uint32_t*>(kXbeBase + offset);
}

std::filesystem::path executableDirectory() {
    wchar_t buffer[MAX_PATH];
    GetModuleFileNameW(nullptr, buffer, MAX_PATH);
    return std::filesystem::path(buffer).parent_path();
}

std::filesystem::path findGameRoot(const std::filesystem::path& exeDirectory) {
    if (const char* environment = std::getenv("CW_GAME_ROOT")) {
        return environment;
    }
    std::ifstream config(exeDirectory / "game_root.txt");
    std::string line;
    if (config && std::getline(config, line) && !line.empty()) {
        return std::filesystem::path(std::u8string(line.begin(), line.end()));
    }
    return exeDirectory / "game";
}

LONG CALLBACK logExceptions(EXCEPTION_POINTERS* info) {
    const DWORD code = info->ExceptionRecord->ExceptionCode;
    if (cw::trace::handle(info)) {
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (code == DBG_PRINTEXCEPTION_C || code == 0x406D1388 || code == DBG_CONTROL_C) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    const CONTEXT* context = info->ContextRecord;
    cw::logf("exception 0x%08lX at %p (eax=%08lX ebx=%08lX ecx=%08lX edx=%08lX esi=%08lX edi=%08lX ebp=%08lX esp=%08lX)",
        code, info->ExceptionRecord->ExceptionAddress, context->Eax, context->Ebx, context->Ecx, context->Edx,
        context->Esi, context->Edi, context->Ebp, context->Esp);
    if (code == EXCEPTION_ACCESS_VIOLATION && info->ExceptionRecord->NumberParameters >= 2) {
        const auto faultAddress = info->ExceptionRecord->ExceptionInformation[1];
        MEMORY_BASIC_INFORMATION region{};
        VirtualQuery(reinterpret_cast<void*>(faultAddress), &region, sizeof(region));
        cw::logf("  access violation %s address 0x%08lX (region %p+0x%lX state 0x%lX protect 0x%lX type 0x%lX)",
            info->ExceptionRecord->ExceptionInformation[0] ? "writing" : "reading", static_cast<unsigned long>(faultAddress),
            region.AllocationBase, static_cast<unsigned long>(region.RegionSize), region.State, region.Protect, region.Type);
    }
    if (code == 0xE06D7363 && info->ExceptionRecord->NumberParameters >= 3) {
        // MSVC x86 ThrowInfo -> CatchableTypeArray -> CatchableType -> TypeDescriptor.name (absolute pointers).
        __try {
            const auto* throwInfo = reinterpret_cast<const DWORD*>(info->ExceptionRecord->ExceptionInformation[2]);
            const auto* catchableTypes = reinterpret_cast<const DWORD*>(static_cast<std::uintptr_t>(throwInfo[3]));
            const auto* catchableType = reinterpret_cast<const DWORD*>(static_cast<std::uintptr_t>(catchableTypes[1]));
            const auto* typeDescriptor = reinterpret_cast<const char*>(static_cast<std::uintptr_t>(catchableType[1]));
            cw::logf("  C++ exception type %s thrown with object %p", typeDescriptor + 8,
                reinterpret_cast<void*>(info->ExceptionRecord->ExceptionInformation[1]));
            const auto* record = reinterpret_cast<const DWORD*>(__readfsdword(0));
            for (int depth = 0; depth < 12 && record != nullptr && reinterpret_cast<std::uintptr_t>(record) != 0xFFFFFFFF; ++depth) {
                cw::logf("    SEH frame %p handler %08lX", record, record[1]);
                record = reinterpret_cast<const DWORD*>(static_cast<std::uintptr_t>(record[0]));
            }
            const auto* stack = reinterpret_cast<const DWORD*>(static_cast<std::uintptr_t>(context->Esp));
            for (int index = 0; index < 512; ++index) {
                const auto* text = reinterpret_cast<const char*>(static_cast<std::uintptr_t>(stack[index]));
                MEMORY_BASIC_INFORMATION memory{};
                if (VirtualQuery(text, &memory, sizeof(memory)) == 0 || memory.State != MEM_COMMIT || (memory.Protect & (PAGE_NOACCESS | PAGE_GUARD))) {
                    continue;
                }
                int length = 0;
                while (length < 96 && text[length] >= 0x20 && text[length] < 0x7F) {
                    ++length;
                }
                if (length >= 5 && text[length] == '\0' && std::memchr(text, '.', length) != nullptr) {
                    cw::logf("    stack string: %.*s", length, text);
                }
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            cw::logf("  C++ exception of unknown type");
        }
    }
    cw::threads::logCallChainFrom("  exception", context->Ebp);
    char scan[400] = {};
    int scanLength = 0;
    int found = 0;
    for (std::uintptr_t slot = context->Esp; found < 14 && slot < context->Esp + 0x2000; slot += 4) {
        MEMORY_BASIC_INFORMATION memory{};
        if (VirtualQuery(reinterpret_cast<void*>(slot), &memory, sizeof(memory)) == 0 || memory.State != MEM_COMMIT) {
            break;
        }
        const DWORD value = *reinterpret_cast<const DWORD*>(slot);
        if (value > 0x20000 && value < 0x2A2E00) {
            scanLength += snprintf(scan + scanLength, sizeof(scan) - scanLength, " %08lX", value);
            ++found;
        }
    }
    cw::logf("  stack scan:%s", scan);
    return EXCEPTION_CONTINUE_SEARCH;
}

LONG WINAPI logUnhandled(EXCEPTION_POINTERS* info) {
    cw::logf("UNHANDLED exception 0x%08lX at %p; terminating", info->ExceptionRecord->ExceptionCode, info->ExceptionRecord->ExceptionAddress);
    ExitProcess(3);
}

} // namespace

extern "C" __declspec(dllexport) void __cdecl CwRun() {
    const std::filesystem::path exeDirectory = executableDirectory();
    const char* logPath = std::getenv("CW_LOG_PATH");
    cw::logInit(logPath != nullptr ? std::filesystem::path(logPath).c_str() : (exeDirectory / "cw_runtime.log").c_str());

    const std::filesystem::path gameRoot = findGameRoot(exeDirectory);
    if (!std::filesystem::exists(gameRoot / "default.xbe")) {
        cw::fatal("game files not found at '%s' (set CW_GAME_ROOT or edit game_root.txt)", gameRoot.string().c_str());
    }
    cw::logf("Clone Wars native runtime: game root '%s'", gameRoot.string().c_str());

    // The XBE header shares the PE header page, and the entry point writes into the certificate.
    DWORD oldProtect;
    VirtualProtect(reinterpret_cast<void*>(kXbeBase), 0x1000, PAGE_READWRITE, &oldProtect);

    const std::uint32_t imageEnd = static_cast<std::uint32_t>(kXbeBase + readHeader32(0x10C));
    std::uint32_t entry = readHeader32(0x128) ^ kEntryRetailKey;
    std::uint32_t thunkTable = readHeader32(0x158) ^ kThunkRetailKey;
    if (entry < kXbeBase || entry >= imageEnd) {
        entry = readHeader32(0x128) ^ kEntryDebugKey;
        thunkTable = readHeader32(0x158) ^ kThunkDebugKey;
    }
    cw::logf("XBE entry 0x%08X, kernel thunk table 0x%08X", entry, thunkTable);

    AddVectoredExceptionHandler(1, &logExceptions);
    SetUnhandledExceptionFilter(&logUnhandled);
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    cw::threads::initializeProcess();
    cw::threads::attachCurrentThread(0x1000, 0);
    const char* hddRoot = std::getenv("CW_HDD_ROOT");
    cw::kernel::initialize(gameRoot, hddRoot != nullptr ? std::filesystem::path(hddRoot) : exeDirectory / "hdd");
    cw::kernel::installThunks(reinterpret_cast<std::uint32_t*>(static_cast<std::uintptr_t>(thunkTable)));
    cw::threads::patchSegmentAccesses();
    cw::hle::installHooks();
    cw::options::install();
    cw::mods::loadPlugins(gameRoot, exeDirectory);
    cw::trace::installFromEnvironment();
    if (const char* exitAfter = std::getenv("CW_EXIT_MS")) {
        // Automated runs: end the process after a fixed time.
        const DWORD milliseconds = static_cast<DWORD>(std::strtoul(exitAfter, nullptr, 10));
        CreateThread(nullptr, 0, [](LPVOID parameter) -> DWORD {
            Sleep(static_cast<DWORD>(reinterpret_cast<std::uintptr_t>(parameter)));
            cw::logf("CW_EXIT_MS reached; exiting");
            ExitProcess(0);
        }, reinterpret_cast<LPVOID>(static_cast<std::uintptr_t>(milliseconds)), 0, nullptr);
    }
    if (const char* watchdog = std::getenv("CW_WATCHDOG_MS")) {
        cw::threads::startWatchdog(static_cast<DWORD>(std::strtoul(watchdog, nullptr, 10)));
    }

    cw::logf("jumping to XBE entry point");
    reinterpret_cast<void(__cdecl*)()>(static_cast<std::uintptr_t>(entry))();
    cw::logf("XBE entry point returned; startup thread exiting");
    ExitThread(0);
}
