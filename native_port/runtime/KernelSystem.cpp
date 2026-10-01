#include "Kernel.h"

#include "Log.h"
#include "Nt.h"
#include "Threads.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace cw::kernel {

namespace {

using namespace xbox;

constexpr ULONG kRegBinary = 3;
constexpr ULONG kRegDword = 4;

struct XboxHardwareInfoData {
    ULONG Flags;
    UCHAR GpuRevision;
    UCHAR McpRevision;
    UCHAR Unknown3;
    UCHAR Unknown4;
};

struct XboxKrnlVersionData {
    USHORT Major;
    USHORT Minor;
    USHORT Build;
    USHORT Qfe;
};

XboxHardwareInfoData g_hardwareInfo{0, 0xD2, 0xB1, 0, 0};
XboxKrnlVersionData g_kernelVersion{1, 0, 5838, 1};
void* g_launchDataPage = nullptr;
char g_imageFileNameBuffer[] = "\\Device\\CdRom0\\default.xbe";
AnsiString g_imageFileName{sizeof(g_imageFileNameBuffer) - 1, sizeof(g_imageFileNameBuffer), g_imageFileNameBuffer};
UCHAR g_hdKey[16] = {};
UCHAR g_lanKey[16] = {};
UCHAR g_publicKeyData[284] = {};
ULONG g_diskCachePartitionCount = 3;
char g_diskModelBuffer[] = "CWNATIVE";
char g_diskSerialBuffer[] = "000000000000";
AnsiString g_diskModelNumber{sizeof(g_diskModelBuffer) - 1, sizeof(g_diskModelBuffer), g_diskModelBuffer};
AnsiString g_diskSerialNumber{sizeof(g_diskSerialBuffer) - 1, sizeof(g_diskSerialBuffer), g_diskSerialBuffer};
ULONG g_bootSmcVideoMode = 0;
UCHAR g_idexChannelObject[0x100] = {};
UCHAR g_eventObjectType[0x40] = {};
UCHAR g_fileObjectType[0x40] = {};
void* g_avSavedDataAddress = nullptr;

NTSTATUS __stdcall xExQueryNonVolatileSetting(ULONG valueIndex, PULONG type, PVOID value, ULONG valueLength, PULONG resultLength) {
    ULONG dwordValue = 0;
    bool isDword = true;
    switch (valueIndex) {
    case 0x007: dwordValue = 1; break;          // XC_LANGUAGE: English
    case 0x008:                                 // XC_VIDEO_FLAGS: widescreen unless CW_WIDESCREEN=0
        dwordValue = std::getenv("CW_WIDESCREEN") != nullptr && std::strcmp(std::getenv("CW_WIDESCREEN"), "0") == 0 ? 0 : 0x00010000;
        break;
    case 0x009: dwordValue = 0; break;          // XC_AUDIO_FLAGS
    case 0x00A: dwordValue = 0; break;          // XC_PARENTAL_CONTROL_GAMES
    case 0x00C: dwordValue = 0; break;          // XC_PARENTAL_CONTROL_MOVIES
    case 0x011: dwordValue = 0; break;          // XC_MISC_FLAGS
    case 0x012: dwordValue = 1; break;          // XC_DVD_REGION
    case 0x103: dwordValue = 0x00400100; break; // XC_FACTORY_AV_REGION: NTSC-M
    case 0x104: dwordValue = 1; break;          // XC_FACTORY_GAME_REGION: North America
    default: isDword = false; break;
    }

    if (isDword) {
        if (type != nullptr) {
            *type = kRegDword;
        }
        if (value != nullptr && valueLength >= sizeof(ULONG)) {
            *static_cast<ULONG*>(value) = dwordValue;
        }
        if (resultLength != nullptr) {
            *resultLength = sizeof(ULONG);
        }
        return kStatusSuccess;
    }

    logf("ExQueryNonVolatileSetting(0x%lX) returning zeroed data", valueIndex);
    if (type != nullptr) {
        *type = kRegBinary;
    }
    if (value != nullptr) {
        std::memset(value, 0, valueLength);
    }
    if (resultLength != nullptr) {
        *resultLength = valueLength;
    }
    return kStatusSuccess;
}

ULONG __cdecl xDbgPrint(const char* format, ...) {
    char buffer[1024];
    va_list args;
    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    size_t length = std::strlen(buffer);
    while (length > 0 && (buffer[length - 1] == '\n' || buffer[length - 1] == '\r')) {
        buffer[--length] = '\0';
    }
    logf("DbgPrint: %s", buffer);
    return kStatusSuccess;
}

void __stdcall xHalReturnToFirmware(ULONG routine) {
    threads::logCallChain("HalReturnToFirmware");
    logf("HalReturnToFirmware(%lu): the title asked to leave; exiting", routine);
    ExitProcess(0);
}

void __stdcall xHalInitiateShutdown() {
    logf("HalInitiateShutdown: exiting");
    ExitProcess(0);
}

BOOLEAN __stdcall xHalIsResetOrShutdownPending() {
    return FALSE;
}

void __stdcall xHalRegisterShutdownNotification(void* registration, BOOLEAN registerNotification) {
}

void __stdcall xHalReadWritePCISpace(ULONG busNumber, ULONG slotNumber, ULONG registerNumber, PVOID buffer, ULONG length, BOOLEAN writePciSpace) {
    if (!writePciSpace) {
        std::memset(buffer, 0, length);
    }
}

ULONG __stdcall xHalGetInterruptVector(ULONG busInterruptLevel, UCHAR* irql) {
    if (irql != nullptr) {
        *irql = static_cast<UCHAR>(26 - busInterruptLevel);
    }
    return busInterruptLevel + 0x30;
}

PVOID __stdcall xAvGetSavedDataAddress() {
    return g_avSavedDataAddress;
}

void __stdcall xAvSetSavedDataAddress(PVOID address) {
    g_avSavedDataAddress = address;
}

void __stdcall xAvSendTVEncoderOption(PVOID registerBase, ULONG option, ULONG param, PULONG result) {
    if (result != nullptr) {
        *result = 0;
    }
}

ULONG __stdcall xAvSetDisplayMode(PVOID registerBase, ULONG step, ULONG mode, ULONG format, ULONG pitch, ULONG frameBuffer) {
    return 0;
}

void __stdcall xKeBugCheck(ULONG code) {
    fatal("KeBugCheck(0x%08lX)", code);
}

NTSTATUS __stdcall xKeSaveFloatingPointState(PVOID state) {
    return kStatusSuccess;
}

NTSTATUS __stdcall xKeRestoreFloatingPointState(PVOID state) {
    return kStatusSuccess;
}

NTSTATUS __stdcall xXeLoadSection(PVOID sectionHeader) {
    // Every section is mapped at startup; only the reference count is maintained.
    InterlockedIncrement(reinterpret_cast<volatile LONG*>(static_cast<std::uint8_t*>(sectionHeader) + 0x18));
    return kStatusSuccess;
}

NTSTATUS __stdcall xXeUnloadSection(PVOID sectionHeader) {
    InterlockedDecrement(reinterpret_cast<volatile LONG*>(static_cast<std::uint8_t*>(sectionHeader) + 0x18));
    return kStatusSuccess;
}

ULONG __stdcall xPhyGetLinkState(BOOLEAN verify) {
    return 0;
}

NTSTATUS __stdcall xPhyInitialize(BOOLEAN forceReset, PVOID parameter) {
    return kStatusSuccess;
}

NTSTATUS __stdcall xNtSetSystemTime(PLARGE_INTEGER systemTime, PLARGE_INTEGER previousTime) {
    return kStatusSuccess;
}

void __stdcall xRtlInitAnsiString(AnsiString* destination, const char* source) {
    destination->Buffer = const_cast<char*>(source);
    if (source == nullptr) {
        destination->Length = 0;
        destination->MaximumLength = 0;
        return;
    }
    const size_t length = std::strlen(source);
    destination->Length = static_cast<USHORT>(length);
    destination->MaximumLength = static_cast<USHORT>(length + 1);
}

NTSTATUS __stdcall xRtlAnsiStringToUnicodeString(UnicodeString* destination, const AnsiString* source, BOOLEAN allocateDestination) {
    const USHORT bytes = static_cast<USHORT>(source->Length * sizeof(wchar_t));
    if (allocateDestination) {
        destination->Buffer = static_cast<wchar_t*>(HeapAlloc(GetProcessHeap(), 0, bytes + sizeof(wchar_t)));
        destination->MaximumLength = bytes + sizeof(wchar_t);
    } else if (destination->MaximumLength < bytes) {
        return kStatusBufferTooSmall;
    }
    for (USHORT index = 0; index < source->Length; ++index) {
        destination->Buffer[index] = static_cast<unsigned char>(source->Buffer[index]);
    }
    destination->Length = bytes;
    if (destination->MaximumLength > bytes) {
        destination->Buffer[source->Length] = L'\0';
    }
    return kStatusSuccess;
}

NTSTATUS __stdcall xRtlUnicodeStringToAnsiString(AnsiString* destination, const UnicodeString* source, BOOLEAN allocateDestination) {
    const USHORT characters = source->Length / sizeof(wchar_t);
    if (allocateDestination) {
        destination->Buffer = static_cast<char*>(HeapAlloc(GetProcessHeap(), 0, characters + 1));
        destination->MaximumLength = characters + 1;
    } else if (destination->MaximumLength < characters) {
        return kStatusBufferTooSmall;
    }
    for (USHORT index = 0; index < characters; ++index) {
        const wchar_t value = source->Buffer[index];
        destination->Buffer[index] = value < 0x100 ? static_cast<char>(value) : '?';
    }
    destination->Length = characters;
    if (destination->MaximumLength > characters) {
        destination->Buffer[characters] = '\0';
    }
    return kStatusSuccess;
}

LONG __stdcall xRtlCompareString(const AnsiString* string1, const AnsiString* string2, BOOLEAN caseInsensitive) {
    const USHORT length = std::min(string1->Length, string2->Length);
    for (USHORT index = 0; index < length; ++index) {
        int a = static_cast<unsigned char>(string1->Buffer[index]);
        int b = static_cast<unsigned char>(string2->Buffer[index]);
        if (caseInsensitive) {
            a = std::toupper(a);
            b = std::toupper(b);
        }
        if (a != b) {
            return a - b;
        }
    }
    return static_cast<LONG>(string1->Length) - static_cast<LONG>(string2->Length);
}

BOOLEAN __stdcall xRtlEqualString(const AnsiString* string1, const AnsiString* string2, BOOLEAN caseInsensitive) {
    return string1->Length == string2->Length && xRtlCompareString(string1, string2, caseInsensitive) == 0;
}

SIZE_T __stdcall xRtlCompareMemoryUlong(const ULONG* source, SIZE_T length, ULONG pattern) {
    SIZE_T matched = 0;
    for (SIZE_T index = 0; index < length / sizeof(ULONG) && source[index] == pattern; ++index) {
        matched += sizeof(ULONG);
    }
    return matched;
}

ULONG __stdcall xRtlNtStatusToDosError(NTSTATUS status) {
    return RtlNtStatusToDosError(status);
}

BOOLEAN __stdcall xRtlTimeFieldsToTime(nt::TimeFields* timeFields, PLARGE_INTEGER time) {
    return RtlTimeFieldsToTime(timeFields, time);
}

void __stdcall xRtlTimeToTimeFields(PLARGE_INTEGER time, nt::TimeFields* timeFields) {
    RtlTimeToTimeFields(time, timeFields);
}

// I/O manager driver plumbing is only reached by the Xbox device stacks, which are replaced at a higher level.
NTSTATUS __stdcall xIoCreateDevice(PVOID driverObject, ULONG deviceExtensionSize, AnsiString* deviceName, ULONG deviceType,
    BOOLEAN exclusive, PVOID* deviceObject) {
    logf("IoCreateDevice('%s') -> stub device", toString(deviceName).c_str());
    auto* device = static_cast<std::uint8_t*>(HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, 0x100 + deviceExtensionSize));
    *reinterpret_cast<PVOID*>(device + 0x18) = device + 0x100;
    *deviceObject = device;
    return kStatusSuccess;
}

NTSTATUS __fastcall xIofCallDriver(PVOID deviceObject, PVOID irp) {
    logf("IofCallDriver(%p, %p) is not supported", deviceObject, irp);
    return kStatusUnsuccessful;
}

void __fastcall xIofCompleteRequest(PVOID irp, CHAR priorityBoost) {
}

} // namespace

void registerSystemExports() {
    registerExport(1, reinterpret_cast<void*>(&xAvGetSavedDataAddress));
    registerExport(2, reinterpret_cast<void*>(&xAvSendTVEncoderOption));
    registerExport(3, reinterpret_cast<void*>(&xAvSetDisplayMode));
    registerExport(4, reinterpret_cast<void*>(&xAvSetSavedDataAddress));
    registerExport(8, reinterpret_cast<void*>(&xDbgPrint));
    registerExport(16, g_eventObjectType);
    registerExport(24, reinterpret_cast<void*>(&xExQueryNonVolatileSetting));
    registerExport(40, &g_diskCachePartitionCount);
    registerExport(41, &g_diskModelNumber);
    registerExport(42, &g_diskSerialNumber);
    registerExport(44, reinterpret_cast<void*>(&xHalGetInterruptVector));
    registerExport(46, reinterpret_cast<void*>(&xHalReadWritePCISpace));
    registerExport(47, reinterpret_cast<void*>(&xHalRegisterShutdownNotification));
    registerExport(49, reinterpret_cast<void*>(&xHalReturnToFirmware));
    registerExport(65, reinterpret_cast<void*>(&xIoCreateDevice));
    registerExport(71, g_fileObjectType);
    registerExport(86, reinterpret_cast<void*>(&xIofCallDriver));
    registerExport(87, reinterpret_cast<void*>(&xIofCompleteRequest));
    registerExport(95, reinterpret_cast<void*>(&xKeBugCheck));
    registerExport(139, reinterpret_cast<void*>(&xKeRestoreFloatingPointState));
    registerExport(142, reinterpret_cast<void*>(&xKeSaveFloatingPointState));
    registerExport(164, &g_launchDataPage);
    registerExport(228, reinterpret_cast<void*>(&xNtSetSystemTime));
    registerExport(252, reinterpret_cast<void*>(&xPhyGetLinkState));
    registerExport(253, reinterpret_cast<void*>(&xPhyInitialize));
    registerExport(260, reinterpret_cast<void*>(&xRtlAnsiStringToUnicodeString));
    registerExport(269, reinterpret_cast<void*>(&xRtlCompareMemoryUlong));
    registerExport(270, reinterpret_cast<void*>(&xRtlCompareString));
    registerExport(279, reinterpret_cast<void*>(&xRtlEqualString));
    registerExport(289, reinterpret_cast<void*>(&xRtlInitAnsiString));
    registerExport(301, reinterpret_cast<void*>(&xRtlNtStatusToDosError));
    // x86 unwinding and exception capture resume in the caller's frame, so these must not be wrapped.
    const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    registerExport(302, reinterpret_cast<void*>(GetProcAddress(ntdll, "RtlRaiseException")));
    registerExport(304, reinterpret_cast<void*>(&xRtlTimeFieldsToTime));
    registerExport(305, reinterpret_cast<void*>(&xRtlTimeToTimeFields));
    registerExport(308, reinterpret_cast<void*>(&xRtlUnicodeStringToAnsiString));
    registerExport(312, reinterpret_cast<void*>(GetProcAddress(ntdll, "RtlUnwind")));
    registerExport(322, &g_hardwareInfo);
    registerExport(323, g_hdKey);
    registerExport(324, &g_kernelVersion);
    registerExport(326, &g_imageFileName);
    registerExport(327, reinterpret_cast<void*>(&xXeLoadSection));
    registerExport(328, reinterpret_cast<void*>(&xXeUnloadSection));
    registerExport(353, g_lanKey);
    registerExport(355, g_publicKeyData);
    registerExport(356, &g_bootSmcVideoMode);
    registerExport(357, g_idexChannelObject);
    registerExport(358, reinterpret_cast<void*>(&xHalIsResetOrShutdownPending));
    registerExport(360, reinterpret_cast<void*>(&xHalInitiateShutdown));
}

} // namespace cw::kernel
