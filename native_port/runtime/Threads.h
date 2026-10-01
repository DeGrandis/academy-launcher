#pragma once

#include <windows.h>

#include <cstdint>

namespace cw::threads {

// Byte offsets inside the emulated Xbox KPCR/KTHREAD that the game reads through FS.
constexpr std::uint32_t kKpcrStackBase = 0x04;
constexpr std::uint32_t kKpcrSelf = 0x18;
constexpr std::uint32_t kKpcrPrcb = 0x20;
constexpr std::uint32_t kKpcrIrql = 0x24;
constexpr std::uint32_t kKpcrCurrentThread = 0x28;
constexpr std::uint32_t kKpcrSize = 0x400;
constexpr std::uint32_t kKthreadTlsData = 0x28;
constexpr std::uint32_t kEthreadUniqueThread = 0x12C;
constexpr std::uint32_t kKthreadSize = 0x200;

void initializeProcess();
void attachCurrentThread(std::uint32_t tlsDataSize, UCHAR irql);
std::uint8_t* currentKpcr();
UCHAR currentIrql();
void setCurrentIrql(UCHAR irql);
void patchSegmentAccesses();
void startWatchdog(DWORD intervalMs);
void logCallChain(const char* reason);
void logCallChainFrom(const char* reason, std::uintptr_t ebp);

} // namespace cw::threads
