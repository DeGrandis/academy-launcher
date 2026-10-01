#pragma once

#include <windows.h>

// Windows NT native API used to back the Xbox kernel. Declared here instead of <winternl.h>
// so the Xbox ABI types and the host NT types never collide.
namespace cw::nt {

struct UnicodeString {
    USHORT Length;
    USHORT MaximumLength;
    PWSTR Buffer;
};

struct ObjectAttributes {
    ULONG Length;
    HANDLE RootDirectory;
    UnicodeString* ObjectName;
    ULONG Attributes;
    PVOID SecurityDescriptor;
    PVOID SecurityQualityOfService;
};

struct FileDirectoryInformation {
    ULONG NextEntryOffset;
    ULONG FileIndex;
    LARGE_INTEGER CreationTime;
    LARGE_INTEGER LastAccessTime;
    LARGE_INTEGER LastWriteTime;
    LARGE_INTEGER ChangeTime;
    LARGE_INTEGER EndOfFile;
    LARGE_INTEGER AllocationSize;
    ULONG FileAttributes;
    ULONG FileNameLength;
    WCHAR FileName[1];
};

struct TimeFields {
    SHORT Year;
    SHORT Month;
    SHORT Day;
    SHORT Hour;
    SHORT Minute;
    SHORT Second;
    SHORT Milliseconds;
    SHORT Weekday;
};

constexpr ULONG kObjCaseInsensitive = 0x40;

} // namespace cw::nt

extern "C" {
NTSYSAPI LONG NTAPI NtCreateFile(PHANDLE, ACCESS_MASK, cw::nt::ObjectAttributes*, PVOID, PLARGE_INTEGER, ULONG, ULONG, ULONG, ULONG, PVOID, ULONG);
NTSYSAPI LONG NTAPI NtReadFile(HANDLE, HANDLE, PVOID, PVOID, PVOID, PVOID, ULONG, PLARGE_INTEGER, PULONG);
NTSYSAPI LONG NTAPI NtWriteFile(HANDLE, HANDLE, PVOID, PVOID, PVOID, const void*, ULONG, PLARGE_INTEGER, PULONG);
NTSYSAPI LONG NTAPI NtQueryInformationFile(HANDLE, PVOID, PVOID, ULONG, ULONG);
NTSYSAPI LONG NTAPI NtSetInformationFile(HANDLE, PVOID, PVOID, ULONG, ULONG);
NTSYSAPI LONG NTAPI NtQueryVolumeInformationFile(HANDLE, PVOID, PVOID, ULONG, ULONG);
NTSYSAPI LONG NTAPI NtQueryFullAttributesFile(cw::nt::ObjectAttributes*, PVOID);
NTSYSAPI LONG NTAPI NtQueryDirectoryFile(HANDLE, HANDLE, PVOID, PVOID, PVOID, PVOID, ULONG, ULONG, BOOLEAN, cw::nt::UnicodeString*, BOOLEAN);
NTSYSAPI LONG NTAPI NtFlushBuffersFile(HANDLE, PVOID);
NTSYSAPI LONG NTAPI NtDeleteFile(cw::nt::ObjectAttributes*);
NTSYSAPI LONG NTAPI NtClose(HANDLE);
NTSYSAPI LONG NTAPI NtWaitForSingleObject(HANDLE, BOOLEAN, PLARGE_INTEGER);
NTSYSAPI LONG NTAPI NtWaitForMultipleObjects(ULONG, PHANDLE, ULONG, BOOLEAN, PLARGE_INTEGER);
NTSYSAPI LONG NTAPI NtDelayExecution(BOOLEAN, PLARGE_INTEGER);
NTSYSAPI LONG NTAPI NtAllocateVirtualMemory(HANDLE, PVOID*, ULONG_PTR, PSIZE_T, ULONG, ULONG);
NTSYSAPI LONG NTAPI NtFreeVirtualMemory(HANDLE, PVOID*, PSIZE_T, ULONG);
NTSYSAPI ULONG NTAPI RtlNtStatusToDosError(LONG);
NTSYSAPI BOOLEAN NTAPI RtlTimeFieldsToTime(cw::nt::TimeFields*, PLARGE_INTEGER);
NTSYSAPI VOID NTAPI RtlTimeToTimeFields(PLARGE_INTEGER, cw::nt::TimeFields*);
NTSYSAPI VOID NTAPI RtlRaiseException(PEXCEPTION_RECORD);
}
