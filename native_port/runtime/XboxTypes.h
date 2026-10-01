#pragma once

#include <windows.h>

#include <cstddef>
#include <cstdint>

// Xbox kernel ABI types. Layouts follow the Xbox kernel, which uses ANSI object names.
namespace cw::xbox {

using NTSTATUS = LONG;

constexpr NTSTATUS kStatusSuccess = 0;
constexpr NTSTATUS kStatusAlerted = 0x101;
constexpr NTSTATUS kStatusTimeout = 0x102;
constexpr NTSTATUS kStatusPending = 0x103;
constexpr NTSTATUS kStatusUserApc = 0xC0;
constexpr NTSTATUS kStatusUnsuccessful = static_cast<NTSTATUS>(0xC0000001);
constexpr NTSTATUS kStatusNotImplemented = static_cast<NTSTATUS>(0xC0000002);
constexpr NTSTATUS kStatusInvalidHandle = static_cast<NTSTATUS>(0xC0000008);
constexpr NTSTATUS kStatusInvalidParameter = static_cast<NTSTATUS>(0xC000000D);
constexpr NTSTATUS kStatusNoMemory = static_cast<NTSTATUS>(0xC0000017);
constexpr NTSTATUS kStatusBufferTooSmall = static_cast<NTSTATUS>(0xC0000023);
constexpr NTSTATUS kStatusObjectNameInvalid = static_cast<NTSTATUS>(0xC0000033);
constexpr NTSTATUS kStatusObjectNameNotFound = static_cast<NTSTATUS>(0xC0000034);
constexpr NTSTATUS kStatusObjectPathNotFound = static_cast<NTSTATUS>(0xC000003A);

struct AnsiString {
    USHORT Length;
    USHORT MaximumLength;
    char* Buffer;
};

struct UnicodeString {
    USHORT Length;
    USHORT MaximumLength;
    wchar_t* Buffer;
};

struct ObjectAttributes {
    HANDLE RootDirectory;
    AnsiString* ObjectName;
    ULONG Attributes;
};

struct IoStatusBlock {
    union {
        NTSTATUS Status;
        void* Pointer;
    };
    ULONG_PTR Information;
};

struct DispatcherHeader {
    UCHAR Type;
    UCHAR Absolute;
    UCHAR Size;
    UCHAR Inserted;
    LONG SignalState;
    LIST_ENTRY WaitListHead;
};

struct Kdpc;
using DeferredRoutine = void(__stdcall*)(Kdpc* dpc, void* context, void* argument1, void* argument2);

struct Kdpc {
    SHORT Type;
    BOOLEAN Inserted;
    UCHAR Padding;
    LIST_ENTRY DpcListEntry;
    DeferredRoutine Routine;
    void* DeferredContext;
    void* SystemArgument1;
    void* SystemArgument2;
};

struct Ktimer {
    DispatcherHeader Header;
    ULARGE_INTEGER DueTime;
    LIST_ENTRY TimerListEntry;
    Kdpc* Dpc;
    LONG Period;
};

static_assert(sizeof(ObjectAttributes) == 12);
static_assert(sizeof(DispatcherHeader) == 16);
static_assert(offsetof(Kdpc, Routine) == 0x0C);
static_assert(offsetof(Ktimer, Dpc) == 0x20);

} // namespace cw::xbox
