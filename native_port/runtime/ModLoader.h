#pragma once

#include <filesystem>

namespace cw::mods {

// Loads plugin DLLs from <exe dir>/plugins and <CW_MOD_ROOT>/plugins and calls their CwModInit.
void loadPlugins(const std::filesystem::path& gameRoot, const std::filesystem::path& exeDirectory);

} // namespace cw::mods
