#include "Kernel.h"

#include "Log.h"

#include <cstring>

namespace cw::kernel {

namespace {

constexpr std::uint16_t kExportCount = 379;
void* g_exports[kExportCount] = {};

struct ExportName {
    std::uint16_t ordinal;
    const char* name;
};

// Ordinals of every kernel export imported by default.xbe (from the Ghidra import labels).
constexpr ExportName kImportedNames[] = {
    {1, "AvGetSavedDataAddress"}, {2, "AvSendTVEncoderOption"}, {3, "AvSetDisplayMode"}, {4, "AvSetSavedDataAddress"},
    {8, "DbgPrint"}, {15, "ExAllocatePoolWithTag"}, {16, "ExEventObjectType"}, {17, "ExFreePool"},
    {23, "ExQueryPoolBlockSize"}, {24, "ExQueryNonVolatileSetting"}, {40, "HalDiskCachePartitionCount"},
    {41, "HalDiskModelNumber"}, {42, "HalDiskSerialNumber"}, {44, "HalGetInterruptVector"},
    {46, "HalReadWritePCISpace"}, {47, "HalRegisterShutdownNotification"}, {49, "HalReturnToFirmware"},
    {62, "IoBuildSynchronousFsdRequest"}, {65, "IoCreateDevice"}, {67, "IoCreateSymbolicLink"},
    {69, "IoDeleteSymbolicLink"}, {71, "IoFileObjectType"}, {74, "IoInvalidDeviceRequest"},
    {81, "IoStartNextPacket"}, {83, "IoStartPacket"}, {84, "IoSynchronousDeviceIoControlRequest"},
    {85, "IoSynchronousFsdRequest"}, {86, "IofCallDriver"}, {87, "IofCompleteRequest"}, {95, "KeBugCheck"},
    {97, "KeCancelTimer"}, {98, "KeConnectInterrupt"}, {99, "KeDelayExecutionThread"},
    {100, "KeDisconnectInterrupt"}, {107, "KeInitializeDpc"}, {109, "KeInitializeInterrupt"},
    {113, "KeInitializeTimerEx"}, {119, "KeInsertQueueDpc"}, {126, "KeQueryPerformanceCounter"},
    {127, "KeQueryPerformanceFrequency"}, {128, "KeQuerySystemTime"}, {129, "KeRaiseIrqlToDpcLevel"},
    {137, "KeRemoveQueueDpc"}, {139, "KeRestoreFloatingPointState"}, {142, "KeSaveFloatingPointState"},
    {145, "KeSetEvent"}, {149, "KeSetTimer"}, {150, "KeSetTimerEx"}, {151, "KeStallExecutionProcessor"},
    {153, "KeSynchronizeExecution"}, {156, "KeTickCount"}, {158, "KeWaitForMultipleObjects"},
    {159, "KeWaitForSingleObject"}, {160, "KfRaiseIrql"}, {161, "KfLowerIrql"}, {164, "LaunchDataPage"},
    {165, "MmAllocateContiguousMemory"}, {166, "MmAllocateContiguousMemoryEx"}, {167, "MmAllocateSystemMemory"},
    {168, "MmClaimGpuInstanceMemory"}, {169, "MmCreateKernelStack"}, {170, "MmDeleteKernelStack"},
    {171, "MmFreeContiguousMemory"}, {172, "MmFreeSystemMemory"}, {173, "MmGetPhysicalAddress"},
    {175, "MmLockUnlockBufferPages"}, {176, "MmLockUnlockPhysicalPage"}, {178, "MmPersistContiguousMemory"},
    {179, "MmQueryAddressProtect"}, {180, "MmQueryAllocationSize"}, {181, "MmQueryStatistics"},
    {182, "MmSetAddressProtect"}, {184, "NtAllocateVirtualMemory"}, {187, "NtClose"}, {189, "NtCreateEvent"},
    {190, "NtCreateFile"}, {195, "NtDeleteFile"}, {196, "NtDeviceIoControlFile"}, {198, "NtFlushBuffersFile"},
    {199, "NtFreeVirtualMemory"}, {200, "NtFsControlFile"}, {202, "NtOpenFile"}, {203, "NtOpenSymbolicLinkObject"},
    {207, "NtQueryDirectoryFile"}, {210, "NtQueryFullAttributesFile"}, {211, "NtQueryInformationFile"},
    {215, "NtQuerySymbolicLinkObject"}, {217, "NtQueryVirtualMemory"}, {218, "NtQueryVolumeInformationFile"},
    {219, "NtReadFile"}, {225, "NtSetEvent"}, {226, "NtSetInformationFile"}, {228, "NtSetSystemTime"},
    {233, "NtWaitForSingleObject"}, {234, "NtWaitForSingleObjectEx"}, {236, "NtWriteFile"},
    {246, "ObReferenceObjectByHandle"}, {247, "ObReferenceObjectByName"}, {250, "ObfDereferenceObject"},
    {252, "PhyGetLinkState"}, {253, "PhyInitialize"}, {255, "PsCreateSystemThreadEx"},
    {258, "PsTerminateSystemThread"}, {260, "RtlAnsiStringToUnicodeString"}, {269, "RtlCompareMemoryUlong"},
    {270, "RtlCompareString"}, {277, "RtlEnterCriticalSection"}, {279, "RtlEqualString"},
    {289, "RtlInitAnsiString"}, {291, "RtlInitializeCriticalSection"}, {294, "RtlLeaveCriticalSection"},
    {301, "RtlNtStatusToDosError"}, {302, "RtlRaiseException"}, {304, "RtlTimeFieldsToTime"},
    {305, "RtlTimeToTimeFields"}, {308, "RtlUnicodeStringToAnsiString"}, {312, "RtlUnwind"},
    {322, "XboxHardwareInfo"}, {323, "XboxHDKey"}, {324, "XboxKrnlVersion"}, {326, "XeImageFileName"},
    {327, "XeLoadSection"}, {328, "XeUnloadSection"}, {335, "XcSHAInit"}, {336, "XcSHAUpdate"},
    {337, "XcSHAFinal"}, {338, "XcRC4Key"}, {339, "XcRC4Crypt"}, {340, "XcHMAC"}, {343, "XcPKGetKeyLen"},
    {344, "XcVerifyPKCS1Signature"}, {345, "XcModExp"}, {346, "XcDESKeyParity"}, {347, "XcKeyTable"},
    {349, "XcBlockCryptCBC"}, {353, "XboxLANKey"}, {355, "XePublicKeyData"}, {356, "HalBootSMCVideoMode"},
    {357, "IdexChannelObject"}, {358, "HalIsResetOrShutdownPending"}, {359, "IoMarkIrpMustComplete"},
    {360, "HalInitiateShutdown"},
};

const char* nameOf(std::uint32_t ordinal) {
    for (const ExportName& entry : kImportedNames) {
        if (entry.ordinal == ordinal) {
            return entry.name;
        }
    }
    return "?";
}

[[noreturn]] void __cdecl unimplementedExport(std::uint32_t ordinal) {
    fatal("unimplemented Xbox kernel export %u (%s)", ordinal, nameOf(ordinal));
}

void* makeUnimplementedStub(std::uint32_t ordinal) {
    auto* stub = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, 16, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE));
    stub[0] = 0x68;
    std::memcpy(stub + 1, &ordinal, 4);
    stub[5] = 0xE8;
    const auto relative = static_cast<std::int32_t>(reinterpret_cast<std::uint8_t*>(&unimplementedExport) - (stub + 10));
    std::memcpy(stub + 6, &relative, 4);
    return stub;
}

} // namespace

void registerExport(std::uint16_t ordinal, void* address) {
    if (ordinal >= kExportCount) {
        fatal("kernel export ordinal %u out of range", ordinal);
    }
    g_exports[ordinal] = address;
}

std::string toString(const xbox::AnsiString* value) {
    if (value == nullptr || value->Buffer == nullptr) {
        return {};
    }
    return std::string(value->Buffer, value->Length);
}

void initialize(const std::filesystem::path& gameRoot, const std::filesystem::path& hddRoot) {
    registerFileExports(gameRoot, hddRoot);
    registerSyncExports();
    registerMemoryExports();
    registerSystemExports();
    registerCryptoExports();
    startSystemThreads();
}

void installThunks(std::uint32_t* thunkTable) {
    std::uint32_t resolved = 0;
    std::uint32_t missing = 0;
    for (std::uint32_t* entry = thunkTable; *entry != 0; ++entry) {
        if ((*entry & 0x80000000) == 0) {
            fatal("kernel thunk entry %p is not an ordinal import", entry);
        }
        const std::uint32_t ordinal = *entry & 0x7FFFFFFF;
        void* target = ordinal < kExportCount ? g_exports[ordinal] : nullptr;
        if (target == nullptr) {
            logf("kernel export %u (%s) has no implementation yet", ordinal, nameOf(ordinal));
            target = makeUnimplementedStub(ordinal);
            ++missing;
        } else {
            ++resolved;
        }
        *entry = static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(target));
    }
    logf("kernel thunks: %u resolved, %u unimplemented", resolved, missing);
}

} // namespace cw::kernel
