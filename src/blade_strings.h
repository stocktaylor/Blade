// blade - dashboard text from strings.toml (see blade_strings.cpp).

#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>

namespace blade {

// Loads strings.toml and rebuilds the XUI resource sections it changes (and,
// with --blade_extract_strings / --blade_dump_resources, writes those). Call
// after the XEX is loaded and before the dashboard starts.
void PrepareDashStrings(const std::filesystem::path& game_data_root);

// Guest address and size of a rebuilt section of the dashboard's own module.
std::optional<std::pair<uint32_t, uint32_t>> FindRebuiltSection(const std::string& name);

}  // namespace blade
