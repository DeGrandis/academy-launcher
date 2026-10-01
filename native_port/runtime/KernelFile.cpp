#include "Kernel.h"

#include "Log.h"
#include "Nt.h"

#include <algorithm>
#include <cctype>
#include <map>
#include <mutex>
#include <vector>

namespace cw::kernel {

namespace {

using namespace xbox;

constexpr ULONG kFileOpen = 1;
constexpr ULONG kFileNoIntermediateBuffering = 0x00000008;
constexpr ULONG kFileDirectoryInformationClass = 1;
constexpr std::uintptr_t kSymlinkHandleBase = 0x7FF00000;

std::filesystem::path g_gameRoot;
std::filesystem::path g_hddRoot;
std::mutex g_linksMutex;
std::map<std::string, std::string> g_symbolicLinks;
std::map<std::uintptr_t, std::string> g_symlinkHandles;
std::uintptr_t g_nextSymlinkHandle = kSymlinkHandleBase;

enum class DeviceKind { Dvd, HardDisk };
std::mutex g_handleDevicesMutex;
std::map<HANDLE, DeviceKind> g_handleDevices;

constexpr ULONG kFileFsSizeInformation = 3;
constexpr ULONG kFileFsFullSizeInformation = 7;

std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

bool startsWithNoCase(const std::string& value, const std::string& prefix) {
    return value.size() >= prefix.size() && lower(value.substr(0, prefix.size())) == lower(prefix);
}

// Maps an Xbox device path to a host directory.
bool mapDevicePath(const std::string& xboxPath, std::filesystem::path& hostPath) {
    struct DeviceMapping {
        const char* device;
        std::filesystem::path root;
    };
    const DeviceMapping mappings[] = {
        {"\\Device\\CdRom0", g_gameRoot},
        {"\\Device\\Harddisk0\\Partition1", g_hddRoot / "E"},
        {"\\Device\\Harddisk0\\Partition2", g_hddRoot / "C"},
        {"\\Device\\Harddisk0\\Partition3", g_hddRoot / "X"},
        {"\\Device\\Harddisk0\\Partition4", g_hddRoot / "Y"},
        {"\\Device\\Harddisk0\\Partition5", g_hddRoot / "Z"},
        {"\\Device\\Harddisk0\\Partition6", g_hddRoot / "F"},
        {"\\Device\\Harddisk0\\Partition7", g_hddRoot / "G"},
    };

    for (const DeviceMapping& mapping : mappings) {
        const std::string device = mapping.device;
        if (!startsWithNoCase(xboxPath, device)) {
            continue;
        }
        std::string rest = xboxPath.substr(device.size());
        if (!rest.empty() && rest[0] != '\\') {
            continue;
        }
        while (!rest.empty() && rest[0] == '\\') {
            rest.erase(0, 1);
        }
        if (mapping.root != g_gameRoot) {
            std::error_code error;
            std::filesystem::create_directories(mapping.root, error);
        }
        hostPath = rest.empty() ? mapping.root : mapping.root / rest;
        return true;
    }
    return false;
}

std::string resolveSymbolicLinks(std::string path) {
    std::lock_guard lock(g_linksMutex);
    for (int depth = 0; depth < 8; ++depth) {
        bool replaced = false;
        for (const auto& [link, target] : g_symbolicLinks) {
            if (startsWithNoCase(path, link) && (path.size() == link.size() || path[link.size()] == '\\')) {
                path = target + path.substr(link.size());
                replaced = true;
                break;
            }
        }
        if (!replaced) {
            break;
        }
    }
    return path;
}

struct HostObjectAttributes {
    std::wstring name;
    nt::UnicodeString unicode{};
    nt::ObjectAttributes attributes{};
    DeviceKind device = DeviceKind::HardDisk;
};

NTSTATUS translateObjectAttributes(const ObjectAttributes* xboxAttributes, HostObjectAttributes& host) {
    std::string name = toString(xboxAttributes->ObjectName);
    host.attributes.Length = sizeof(nt::ObjectAttributes);
    host.attributes.Attributes = nt::kObjCaseInsensitive;

    // (HANDLE)-3 is ObDosDevicesDirectory: the name is a drive path such as "D:\".
    const bool dosDevicesRoot = xboxAttributes->RootDirectory == reinterpret_cast<HANDLE>(static_cast<LONG_PTR>(-3));
    if (dosDevicesRoot) {
        name = "\\??\\" + name;
    }

    if (!dosDevicesRoot && xboxAttributes->RootDirectory != nullptr && xboxAttributes->RootDirectory != INVALID_HANDLE_VALUE) {
        host.attributes.RootDirectory = xboxAttributes->RootDirectory;
        {
            std::lock_guard lock(g_handleDevicesMutex);
            auto parent = g_handleDevices.find(xboxAttributes->RootDirectory);
            if (parent != g_handleDevices.end()) {
                host.device = parent->second;
            }
        }
        while (!name.empty() && name[0] == '\\') {
            name.erase(0, 1);
        }
        host.name = std::filesystem::path(name).wstring();
    } else {
        const std::string resolved = resolveSymbolicLinks(name);
        std::filesystem::path hostPath;
        if (!mapDevicePath(resolved, hostPath)) {
            logf("file: no mapping for Xbox path '%s' (resolved '%s')", name.c_str(), resolved.c_str());
            return kStatusObjectPathNotFound;
        }
        host.device = startsWithNoCase(resolved, "\\Device\\CdRom0") ? DeviceKind::Dvd : DeviceKind::HardDisk;
        host.name = L"\\??\\" + hostPath.lexically_normal().wstring();
        while (host.name.size() > 7 && host.name.back() == L'\\') {
            host.name.pop_back();
        }
    }

    host.unicode.Buffer = host.name.data();
    host.unicode.Length = static_cast<USHORT>(host.name.size() * sizeof(wchar_t));
    host.unicode.MaximumLength = host.unicode.Length;
    host.attributes.ObjectName = &host.unicode;
    return kStatusSuccess;
}

NTSTATUS __stdcall xNtCreateFile(PHANDLE fileHandle, ACCESS_MASK desiredAccess, ObjectAttributes* objectAttributes, IoStatusBlock* ioStatusBlock,
    PLARGE_INTEGER allocationSize, ULONG fileAttributes, ULONG shareAccess, ULONG createDisposition, ULONG createOptions) {
    HostObjectAttributes host;
    NTSTATUS status = translateObjectAttributes(objectAttributes, host);
    if (status == kStatusSuccess) {
        // The Xbox I/O manager implicitly allows attribute queries on any handle it returns.
        status = NtCreateFile(fileHandle, desiredAccess | FILE_READ_ATTRIBUTES | SYNCHRONIZE, &host.attributes, ioStatusBlock, allocationSize, fileAttributes,
            shareAccess, createDisposition, createOptions & ~kFileNoIntermediateBuffering, nullptr, 0);
        if (status == kStatusSuccess) {
            std::lock_guard lock(g_handleDevicesMutex);
            g_handleDevices[*fileHandle] = host.device;
        }
    } else if (ioStatusBlock != nullptr) {
        ioStatusBlock->Status = status;
    }
    logf("NtCreateFile('%s') -> 0x%08X", toString(objectAttributes->ObjectName).c_str(), status);
    return status;
}

NTSTATUS __stdcall xNtOpenFile(PHANDLE fileHandle, ACCESS_MASK desiredAccess, ObjectAttributes* objectAttributes, IoStatusBlock* ioStatusBlock,
    ULONG shareAccess, ULONG openOptions) {
    return xNtCreateFile(fileHandle, desiredAccess, objectAttributes, ioStatusBlock, nullptr, 0, shareAccess, kFileOpen, openOptions);
}

NTSTATUS __stdcall xNtReadFile(HANDLE fileHandle, HANDLE event, PVOID apcRoutine, PVOID apcContext, IoStatusBlock* ioStatusBlock,
    PVOID buffer, ULONG length, PLARGE_INTEGER byteOffset) {
    const NTSTATUS status = NtReadFile(fileHandle, event, apcRoutine, apcContext, ioStatusBlock, buffer, length, byteOffset, nullptr);
    if (status < 0) {
        logf("NtReadFile(%p, len=%lu, offset=%lld) -> 0x%08X read=%lu", fileHandle, length,
            byteOffset != nullptr ? byteOffset->QuadPart : -1LL, status, static_cast<unsigned long>(ioStatusBlock->Information));
    }
    return status;
}

NTSTATUS __stdcall xNtWriteFile(HANDLE fileHandle, HANDLE event, PVOID apcRoutine, PVOID apcContext, IoStatusBlock* ioStatusBlock,
    PVOID buffer, ULONG length, PLARGE_INTEGER byteOffset) {
    return NtWriteFile(fileHandle, event, apcRoutine, apcContext, ioStatusBlock, buffer, length, byteOffset, nullptr);
}

NTSTATUS __stdcall xNtQueryInformationFile(HANDLE fileHandle, IoStatusBlock* ioStatusBlock, PVOID information, ULONG length, ULONG informationClass) {
    const NTSTATUS status = NtQueryInformationFile(fileHandle, ioStatusBlock, information, length, informationClass);
    logf("NtQueryInformationFile(%p, class=%lu) -> 0x%08X", fileHandle, informationClass, status);
    return status;
}

NTSTATUS __stdcall xNtSetInformationFile(HANDLE fileHandle, IoStatusBlock* ioStatusBlock, PVOID information, ULONG length, ULONG informationClass) {
    const NTSTATUS status = NtSetInformationFile(fileHandle, ioStatusBlock, information, length, informationClass);
    logf("NtSetInformationFile(%p, class=%lu) -> 0x%08X", fileHandle, informationClass, status);
    return status;
}

// Re-expresses host volume sizes in Xbox geometry: 16 KB FATX clusters on the HDD, 2 KB sectors on the DVD.
void convertSizeInformation(HANDLE fileHandle, PVOID information, ULONG informationClass) {
    DeviceKind device = DeviceKind::HardDisk;
    {
        std::lock_guard lock(g_handleDevicesMutex);
        auto entry = g_handleDevices.find(fileHandle);
        if (entry != g_handleDevices.end()) {
            device = entry->second;
        }
    }

    auto* units = static_cast<LARGE_INTEGER*>(information);
    const int unitCount = informationClass == kFileFsFullSizeInformation ? 3 : 2;
    auto* sizes = reinterpret_cast<ULONG*>(units + unitCount);
    const ULONGLONG hostUnitBytes = static_cast<ULONGLONG>(sizes[0]) * sizes[1];
    const ULONG bytesPerSector = device == DeviceKind::Dvd ? 2048 : 512;
    const ULONG sectorsPerUnit = device == DeviceKind::Dvd ? 1 : 32;
    const ULONGLONG unitBytes = static_cast<ULONGLONG>(bytesPerSector) * sectorsPerUnit;
    // 1 GB keeps byte totals well inside the signed 32-bit math titles use for free-block checks.
    constexpr ULONGLONG kMaxBytes = 0x40000000ull;
    for (int index = 0; index < unitCount; ++index) {
        const ULONGLONG bytes = std::min<ULONGLONG>(static_cast<ULONGLONG>(units[index].QuadPart) * hostUnitBytes, kMaxBytes);
        units[index].QuadPart = static_cast<LONGLONG>(bytes / unitBytes);
    }
    sizes[0] = sectorsPerUnit;
    sizes[1] = bytesPerSector;
}

NTSTATUS __stdcall xNtQueryVolumeInformationFile(HANDLE fileHandle, IoStatusBlock* ioStatusBlock, PVOID information, ULONG length, ULONG informationClass) {
    const NTSTATUS status = NtQueryVolumeInformationFile(fileHandle, ioStatusBlock, information, length, informationClass);
    if (status == kStatusSuccess && (informationClass == kFileFsSizeInformation || informationClass == kFileFsFullSizeInformation)) {
        convertSizeInformation(fileHandle, information, informationClass);
    }
    logf("NtQueryVolumeInformationFile(%p, class=%lu) -> 0x%08X", fileHandle, informationClass, status);
    return status;
}

NTSTATUS __stdcall xNtQueryFullAttributesFile(ObjectAttributes* objectAttributes, PVOID information) {
    HostObjectAttributes host;
    NTSTATUS status = translateObjectAttributes(objectAttributes, host);
    if (status == kStatusSuccess) {
        status = NtQueryFullAttributesFile(&host.attributes, information);
    }
    return status;
}

NTSTATUS __stdcall xNtDeleteFile(ObjectAttributes* objectAttributes) {
    HostObjectAttributes host;
    NTSTATUS status = translateObjectAttributes(objectAttributes, host);
    if (status == kStatusSuccess) {
        status = NtDeleteFile(&host.attributes);
    }
    return status;
}

NTSTATUS __stdcall xNtFlushBuffersFile(HANDLE fileHandle, IoStatusBlock* ioStatusBlock) {
    return NtFlushBuffersFile(fileHandle, ioStatusBlock);
}

// Xbox directory entries carry ANSI names, so each NT entry is converted one at a time.
NTSTATUS __stdcall xNtQueryDirectoryFile(HANDLE fileHandle, HANDLE event, PVOID apcRoutine, PVOID apcContext, IoStatusBlock* ioStatusBlock,
    PVOID information, ULONG length, ULONG informationClass, AnsiString* fileMask, BOOLEAN restartScan) {
    if (informationClass != kFileDirectoryInformationClass) {
        logf("NtQueryDirectoryFile: unsupported information class %lu", informationClass);
        return kStatusNotImplemented;
    }

    std::vector<std::uint8_t> hostBuffer(sizeof(nt::FileDirectoryInformation) + MAX_PATH * sizeof(wchar_t));
    std::wstring mask;
    nt::UnicodeString unicodeMask{};
    if (fileMask != nullptr && fileMask->Buffer != nullptr) {
        mask = std::filesystem::path(toString(fileMask)).wstring();
        unicodeMask = {static_cast<USHORT>(mask.size() * 2), static_cast<USHORT>(mask.size() * 2), mask.data()};
    }

    NTSTATUS status = NtQueryDirectoryFile(fileHandle, event, apcRoutine, apcContext, ioStatusBlock, hostBuffer.data(),
        static_cast<ULONG>(hostBuffer.size()), informationClass, TRUE, mask.empty() ? nullptr : &unicodeMask, restartScan);
    if (status != kStatusSuccess) {
        return status;
    }

    const auto* entry = reinterpret_cast<const nt::FileDirectoryInformation*>(hostBuffer.data());
    const std::string name = std::filesystem::path(std::wstring(entry->FileName, entry->FileNameLength / sizeof(wchar_t))).string();
    const ULONG headerSize = offsetof(nt::FileDirectoryInformation, FileName);
    if (length < headerSize + name.size()) {
        return kStatusBufferTooSmall;
    }
    std::memcpy(information, entry, headerSize);
    auto* xboxEntry = static_cast<std::uint8_t*>(information);
    reinterpret_cast<nt::FileDirectoryInformation*>(xboxEntry)->NextEntryOffset = 0;
    reinterpret_cast<nt::FileDirectoryInformation*>(xboxEntry)->FileNameLength = static_cast<ULONG>(name.size());
    std::memcpy(xboxEntry + headerSize, name.data(), name.size());
    ioStatusBlock->Information = headerSize + name.size();
    return kStatusSuccess;
}

NTSTATUS __stdcall xNtFsControlFile(HANDLE fileHandle, HANDLE event, PVOID apcRoutine, PVOID apcContext, IoStatusBlock* ioStatusBlock,
    ULONG controlCode, PVOID inputBuffer, ULONG inputLength, PVOID outputBuffer, ULONG outputLength) {
    logf("NtFsControlFile(handle=%p, code=0x%08lX) ignored", fileHandle, controlCode);
    if (ioStatusBlock != nullptr) {
        ioStatusBlock->Status = kStatusSuccess;
        ioStatusBlock->Information = 0;
    }
    return kStatusSuccess;
}

NTSTATUS __stdcall xNtDeviceIoControlFile(HANDLE fileHandle, HANDLE event, PVOID apcRoutine, PVOID apcContext, IoStatusBlock* ioStatusBlock,
    ULONG controlCode, PVOID inputBuffer, ULONG inputLength, PVOID outputBuffer, ULONG outputLength) {
    logf("NtDeviceIoControlFile(handle=%p, code=0x%08lX) unsupported", fileHandle, controlCode);
    if (ioStatusBlock != nullptr) {
        ioStatusBlock->Status = kStatusInvalidParameter;
        ioStatusBlock->Information = 0;
    }
    return kStatusInvalidParameter;
}

NTSTATUS __stdcall xIoCreateSymbolicLink(AnsiString* symbolicLinkName, AnsiString* deviceName) {
    const std::string link = toString(symbolicLinkName);
    const std::string target = toString(deviceName);
    {
        std::lock_guard lock(g_linksMutex);
        g_symbolicLinks[link] = target;
    }
    logf("IoCreateSymbolicLink('%s' -> '%s')", link.c_str(), target.c_str());
    return kStatusSuccess;
}

NTSTATUS __stdcall xIoDeleteSymbolicLink(AnsiString* symbolicLinkName) {
    std::lock_guard lock(g_linksMutex);
    g_symbolicLinks.erase(toString(symbolicLinkName));
    return kStatusSuccess;
}

NTSTATUS __stdcall xNtOpenSymbolicLinkObject(PHANDLE linkHandle, ObjectAttributes* objectAttributes) {
    const std::string name = toString(objectAttributes->ObjectName);
    std::lock_guard lock(g_linksMutex);
    auto link = std::find_if(g_symbolicLinks.begin(), g_symbolicLinks.end(), [&](const auto& entry) { return lower(entry.first) == lower(name); });
    if (link == g_symbolicLinks.end()) {
        return kStatusObjectNameNotFound;
    }
    const std::uintptr_t handle = g_nextSymlinkHandle;
    g_nextSymlinkHandle += 4;
    g_symlinkHandles[handle] = link->second;
    *linkHandle = reinterpret_cast<HANDLE>(handle);
    return kStatusSuccess;
}

NTSTATUS __stdcall xNtQuerySymbolicLinkObject(HANDLE linkHandle, AnsiString* linkTarget, PULONG returnedLength) {
    std::lock_guard lock(g_linksMutex);
    auto entry = g_symlinkHandles.find(reinterpret_cast<std::uintptr_t>(linkHandle));
    if (entry == g_symlinkHandles.end()) {
        return kStatusInvalidHandle;
    }
    const std::string& target = entry->second;
    if (returnedLength != nullptr) {
        *returnedLength = static_cast<ULONG>(target.size());
    }
    if (linkTarget->MaximumLength < target.size()) {
        return kStatusBufferTooSmall;
    }
    std::memcpy(linkTarget->Buffer, target.data(), target.size());
    linkTarget->Length = static_cast<USHORT>(target.size());
    return kStatusSuccess;
}

NTSTATUS __stdcall xNtClose(HANDLE handle) {
    {
        std::lock_guard lock(g_linksMutex);
        if (g_symlinkHandles.erase(reinterpret_cast<std::uintptr_t>(handle)) != 0) {
            return kStatusSuccess;
        }
    }
    {
        std::lock_guard lock(g_handleDevicesMutex);
        g_handleDevices.erase(handle);
    }
    return NtClose(handle);
}

} // namespace

void createSymbolicLink(const std::string& link, const std::string& target) {
    std::lock_guard lock(g_linksMutex);
    g_symbolicLinks[link] = target;
}

void registerFileExports(const std::filesystem::path& gameRoot, const std::filesystem::path& hddRoot) {
    g_gameRoot = gameRoot;
    g_hddRoot = hddRoot;
    registerExport(67, reinterpret_cast<void*>(&xIoCreateSymbolicLink));
    registerExport(69, reinterpret_cast<void*>(&xIoDeleteSymbolicLink));
    registerExport(187, reinterpret_cast<void*>(&xNtClose));
    registerExport(190, reinterpret_cast<void*>(&xNtCreateFile));
    registerExport(195, reinterpret_cast<void*>(&xNtDeleteFile));
    registerExport(196, reinterpret_cast<void*>(&xNtDeviceIoControlFile));
    registerExport(198, reinterpret_cast<void*>(&xNtFlushBuffersFile));
    registerExport(200, reinterpret_cast<void*>(&xNtFsControlFile));
    registerExport(202, reinterpret_cast<void*>(&xNtOpenFile));
    registerExport(203, reinterpret_cast<void*>(&xNtOpenSymbolicLinkObject));
    registerExport(207, reinterpret_cast<void*>(&xNtQueryDirectoryFile));
    registerExport(210, reinterpret_cast<void*>(&xNtQueryFullAttributesFile));
    registerExport(211, reinterpret_cast<void*>(&xNtQueryInformationFile));
    registerExport(215, reinterpret_cast<void*>(&xNtQuerySymbolicLinkObject));
    registerExport(218, reinterpret_cast<void*>(&xNtQueryVolumeInformationFile));
    registerExport(219, reinterpret_cast<void*>(&xNtReadFile));
    registerExport(226, reinterpret_cast<void*>(&xNtSetInformationFile));
    registerExport(236, reinterpret_cast<void*>(&xNtWriteFile));
}

} // namespace cw::kernel
