#pragma once

#include "XboxTypes.h"

#include <cstdint>
#include <filesystem>
#include <string>

namespace cw::kernel {

void initialize(const std::filesystem::path& gameRoot, const std::filesystem::path& hddRoot);
void installThunks(std::uint32_t* thunkTable);

// Internal registration used by the individual kernel modules.
void registerExport(std::uint16_t ordinal, void* address);
void registerFileExports(const std::filesystem::path& gameRoot, const std::filesystem::path& hddRoot);
void registerSyncExports();
void registerMemoryExports();
void registerSystemExports();
void registerCryptoExports();
void startSystemThreads();

std::string toString(const xbox::AnsiString* value);
void createSymbolicLink(const std::string& link, const std::string& target);

} // namespace cw::kernel
