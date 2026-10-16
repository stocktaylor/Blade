// blade - the dashboard's console settings (XConfig), kept by Blade.

#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace blade {

// Loads the settings the dashboard saved before (config_dir/xconfig.toml) and
// saves later changes there.
void LoadDisplaySettings(const std::filesystem::path& config_dir);

// Replaces Display > Screen Resolution's list with window sizes. Call after
// the XEX is loaded.
void PatchResolutionList();

// The saved Display settings: Screen Resolution (window size) and Screen
// Format, relabelled Display Mode (windowed / fullscreen).
struct DisplayChoice {
  uint32_t width = 0, height = 0;  // 0 = not chosen yet
  std::optional<bool> fullscreen;
};
DisplayChoice CurrentDisplayChoice();

// Called (on the guest thread that saved it) when either setting changes.
void SetDisplayChangedCallback(std::function<void(const DisplayChoice&)> callback);

// Arguments for a launched game from the saved Display settings.
std::vector<std::string> DisplayLaunchArgs();

}  // namespace blade
