#include "Kernel.h"

#include "Log.h"
#include "Nt.h"

#include <mutex>
#include <unordered_map>

namespace cw::kernel {

namespace {

using namespace xbox;

constexpr ULONG kMemNoZero = 0x00800000;
constexpr ULONG kPageCacheFlags = PAGE_NOCACHE | PAGE_WRITECOMBINE;

HANDLE g_poolHeap = nullptr;
std::mutex g_allocationsMutex;
std::unordered_map<void*, SIZE_T> g_allocationSizes;

ULONG hostProtect(ULONG protect) {
    const ULONG stripped = protect & ~kPageCacheFlags;
    return stripped == 0 ? PAGE_READWRITE : stripped;
}

void* allocateTracked(SIZE_T size, ULONG protect, SIZE_T alignment) {
    const SIZE_T padded = alignment > 0x10000 ? size + alignment : size;
    void* base = VirtualAlloc(nullptr, padded, MEM_RESERVE | MEM_COMMIT, hostProtect(protect));
    if (base == nullptr) {
        return nullptr;
    }
    if (alignment > 0x10000) {
        // Release and re-reserve at the aligned address inside the probed range.
        const auto aligned = (reinterpret_cast<std::uintptr_t>(base) + alignment - 1) & ~(alignment - 1);
        VirtualFree(base, 0, MEM_RELEASE);
        base = VirtualAlloc(reinterpret_cast<void*>(aligned), size, MEM_RESERVE | MEM_COMMIT, hostProtect(protect));
        if (base == nullptr) {
            return nullptr;
        }
    }
    std::lock_guard lock(g_allocationsMutex);
    g_allocationSizes[base] = size;
    return base;
}

void freeTracked(void* base) {
    {
        std::lock_guard lock(g_allocationsMutex);
        g_allocationSizes.erase(base);
    }
    VirtualFree(base, 0, MEM_RELEASE);
}

NTSTATUS __stdcall xNtAllocateVirtualMemory(PVOID* baseAddress, ULONG_PTR zeroBits, PSIZE_T regionSize, ULONG allocationType, ULONG protect) {
    const PVOID requestedBase = *baseAddress;
    const SIZE_T requestedSize = *regionSize;
    const NTSTATUS status = NtAllocateVirtualMemory(GetCurrentProcess(), baseAddress, zeroBits, regionSize, allocationType & ~kMemNoZero, hostProtect(protect));
    logf("NtAllocateVirtualMemory(base=%p, size=0x%lX, type=0x%lX, protect=0x%lX) -> 0x%08X base=%p",
        requestedBase, static_cast<unsigned long>(requestedSize), allocationType, protect, status, *baseAddress);
    return status;
}

NTSTATUS __stdcall xNtFreeVirtualMemory(PVOID* baseAddress, PSIZE_T regionSize, ULONG freeType) {
    return NtFreeVirtualMemory(GetCurrentProcess(), baseAddress, regionSize, freeType);
}

NTSTATUS __stdcall xNtQueryVirtualMemory(PVOID baseAddress, MEMORY_BASIC_INFORMATION* information) {
    return VirtualQuery(baseAddress, information, sizeof(*information)) == 0 ? kStatusInvalidParameter : kStatusSuccess;
}

PVOID __stdcall xMmAllocateContiguousMemoryEx(SIZE_T numberOfBytes, ULONG_PTR lowestAddress, ULONG_PTR highestAddress, ULONG_PTR alignment, ULONG protect) {
    return allocateTracked(numberOfBytes, protect, alignment);
}

PVOID __stdcall xMmAllocateContiguousMemory(SIZE_T numberOfBytes) {
    return allocateTracked(numberOfBytes, PAGE_READWRITE, 0);
}

void __stdcall xMmFreeContiguousMemory(PVOID baseAddress) {
    freeTracked(baseAddress);
}

PVOID __stdcall xMmAllocateSystemMemory(SIZE_T numberOfBytes, ULONG protect) {
    return allocateTracked(numberOfBytes, protect, 0);
}

ULONG __stdcall xMmFreeSystemMemory(PVOID baseAddress, SIZE_T numberOfBytes) {
    freeTracked(baseAddress);
    return static_cast<ULONG>(numberOfBytes / 0x1000);
}

SIZE_T __stdcall xMmQueryAllocationSize(PVOID baseAddress) {
    std::lock_guard lock(g_allocationsMutex);
    auto entry = g_allocationSizes.find(baseAddress);
    return entry == g_allocationSizes.end() ? 0 : entry->second;
}

void __stdcall xMmPersistContiguousMemory(PVOID baseAddress, SIZE_T numberOfBytes, BOOLEAN persist) {
}

ULONG_PTR __stdcall xMmGetPhysicalAddress(PVOID baseAddress) {
    return reinterpret_cast<ULONG_PTR>(baseAddress);
}

void __stdcall xMmSetAddressProtect(PVOID baseAddress, ULONG numberOfBytes, ULONG newProtect) {
    DWORD oldProtect;
    VirtualProtect(baseAddress, numberOfBytes, hostProtect(newProtect), &oldProtect);
}

ULONG __stdcall xMmQueryAddressProtect(PVOID virtualAddress) {
    MEMORY_BASIC_INFORMATION information{};
    VirtualQuery(virtualAddress, &information, sizeof(information));
    return information.Protect;
}

void __stdcall xMmLockUnlockBufferPages(PVOID baseAddress, SIZE_T numberOfBytes, BOOLEAN unlockPages) {
}

void __stdcall xMmLockUnlockPhysicalPage(ULONG_PTR physicalAddress, BOOLEAN unlockPage) {
}

struct MmStatistics {
    ULONG Length;
    ULONG TotalPhysicalPages;
    ULONG AvailablePages;
    ULONG VirtualMemoryBytesCommitted;
    ULONG VirtualMemoryBytesReserved;
    ULONG CachePagesCommitted;
    ULONG PoolPagesCommitted;
    ULONG StackPagesCommitted;
    ULONG ImagePagesCommitted;
};

NTSTATUS __stdcall xMmQueryStatistics(MmStatistics* statistics) {
    if (statistics == nullptr || statistics->Length != sizeof(MmStatistics)) {
        return kStatusInvalidParameter;
    }
    // Report the 64 MB retail console the game was designed for.
    statistics->TotalPhysicalPages = 0x4000;
    statistics->AvailablePages = 0x2000;
    statistics->VirtualMemoryBytesCommitted = 0x2000000;
    statistics->VirtualMemoryBytesReserved = 0x4000000;
    statistics->CachePagesCommitted = 0;
    statistics->PoolPagesCommitted = 0x100;
    statistics->StackPagesCommitted = 0x40;
    statistics->ImagePagesCommitted = 0x650;
    return kStatusSuccess;
}

PVOID __stdcall xMmClaimGpuInstanceMemory(SIZE_T numberOfBytes, PSIZE_T numberOfPaddingBytes) {
    if (numberOfPaddingBytes != nullptr) {
        *numberOfPaddingBytes = 0;
    }
    return allocateTracked(numberOfBytes == static_cast<SIZE_T>(-1) ? 0x10000 : numberOfBytes, PAGE_READWRITE, 0);
}

PVOID __stdcall xMmCreateKernelStack(ULONG numberOfBytes, BOOLEAN debuggerThread) {
    auto* base = static_cast<std::uint8_t*>(allocateTracked(numberOfBytes, PAGE_READWRITE, 0));
    return base == nullptr ? nullptr : base + numberOfBytes;
}

void __stdcall xMmDeleteKernelStack(PVOID stackBase, PVOID stackLimit) {
    freeTracked(stackLimit);
}

PVOID __stdcall xExAllocatePoolWithTag(SIZE_T numberOfBytes, ULONG tag) {
    return HeapAlloc(g_poolHeap, HEAP_ZERO_MEMORY, numberOfBytes);
}

void __stdcall xExFreePool(PVOID pool) {
    HeapFree(g_poolHeap, 0, pool);
}

ULONG __stdcall xExQueryPoolBlockSize(PVOID poolBlock) {
    return static_cast<ULONG>(HeapSize(g_poolHeap, 0, poolBlock));
}

} // namespace

void registerMemoryExports() {
    g_poolHeap = HeapCreate(0, 0, 0);
    registerExport(15, reinterpret_cast<void*>(&xExAllocatePoolWithTag));
    registerExport(17, reinterpret_cast<void*>(&xExFreePool));
    registerExport(23, reinterpret_cast<void*>(&xExQueryPoolBlockSize));
    registerExport(165, reinterpret_cast<void*>(&xMmAllocateContiguousMemory));
    registerExport(166, reinterpret_cast<void*>(&xMmAllocateContiguousMemoryEx));
    registerExport(167, reinterpret_cast<void*>(&xMmAllocateSystemMemory));
    registerExport(168, reinterpret_cast<void*>(&xMmClaimGpuInstanceMemory));
    registerExport(169, reinterpret_cast<void*>(&xMmCreateKernelStack));
    registerExport(170, reinterpret_cast<void*>(&xMmDeleteKernelStack));
    registerExport(171, reinterpret_cast<void*>(&xMmFreeContiguousMemory));
    registerExport(172, reinterpret_cast<void*>(&xMmFreeSystemMemory));
    registerExport(173, reinterpret_cast<void*>(&xMmGetPhysicalAddress));
    registerExport(175, reinterpret_cast<void*>(&xMmLockUnlockBufferPages));
    registerExport(176, reinterpret_cast<void*>(&xMmLockUnlockPhysicalPage));
    registerExport(178, reinterpret_cast<void*>(&xMmPersistContiguousMemory));
    registerExport(179, reinterpret_cast<void*>(&xMmQueryAddressProtect));
    registerExport(180, reinterpret_cast<void*>(&xMmQueryAllocationSize));
    registerExport(181, reinterpret_cast<void*>(&xMmQueryStatistics));
    registerExport(182, reinterpret_cast<void*>(&xMmSetAddressProtect));
    registerExport(184, reinterpret_cast<void*>(&xNtAllocateVirtualMemory));
    registerExport(199, reinterpret_cast<void*>(&xNtFreeVirtualMemory));
    registerExport(217, reinterpret_cast<void*>(&xNtQueryVirtualMemory));
}

} // namespace cw::kernel
