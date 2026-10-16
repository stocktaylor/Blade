// blade - the dashboard as a launcher for other rexglue recomps.
//
// launcher.toml lists the recomps. They are shown in Games > Demos (the
// dashboard's XamContentAggregateCreateEnumerator), and selecting one replaces
// the dashboard process with a small relay (this executable, run with
// --blade-relay) that runs the game, waits for it to exit however it exits,
// and starts the dashboard again. See blade_launcher.cpp.

#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace blade {

struct LauncherTitle {
  std::string id;    // [title.<id>]; the content file name the dashboard hands back
  std::string name;  // shown in the dashboard
  uint32_t title_id = 0;
  std::filesystem::path exe;
  std::vector<std::string> args;
  std::filesystem::path cwd;  // default: launcher.toml's folder
};

// Loads the registry: --blade_launcher_config, else launcher.toml next to the
// executable, else next to the game folder (the project root in a dev build).
void LoadLauncherConfig(const std::filesystem::path& game_data_root);

const std::vector<LauncherTitle>& LauncherTitles();
const LauncherTitle* FindLauncherTitle(std::string_view id);

// Replaces this process with the relay for `title`. Returns only on failure.
void LaunchTitle(const LauncherTitle& title);

}  // namespace blade
