// blade - ReXGlue Recompiled Project
//
// Customize your app by overriding virtual hooks from rex::ReXApp.

#pragma once

#include <functional>
#include <memory>
#include <optional>

#include <rex/filesystem.h>
#include <rex/filesystem/devices/host_path_device.h>
#include <rex/filesystem/vfs.h>
#include <rex/rex_app.h>
#include <rex/runtime.h>
#include <rex/ui/window.h>
#include <rex/ui/windowed_app_context.h>

#include "blade_display.h"
#if BLADE_LAUNCHER
#include <rexlauncher/launch_menu.h>

#include "blade_launcher.h"
#include "blade_strings.h"
#endif

class BladeApp : public rex::ReXApp {
 public:
  using rex::ReXApp::ReXApp;

  static std::unique_ptr<rex::ui::WindowedApp> Create(
      rex::ui::WindowedAppContext& ctx) {
    return std::unique_ptr<BladeApp>(new BladeApp(ctx, "blade",
        PPCImageConfig));
  }

  // With no --game_data_root: an "assets" or "game" folder next to the
  // executable (a packaged build, see rextools `rex dist`), else the project's
  // assets folder (cmake/extract_assets.cmake), so a dev build runs as is.
  void OnConfigurePaths(rex::PathConfig& paths) override {
    if (!paths.game_data_root.empty()) {
      return;
    }
    auto exe_dir = rex::filesystem::GetExecutableFolder();
    for (const char* name : {"assets", "game"}) {
      if (std::filesystem::exists(exe_dir / name / "default.xex")) {
        paths.game_data_root = exe_dir / name;
        return;
      }
    }
    paths.game_data_root = BLADE_PROJECT_ASSETS_DIR;
  }

#if BLADE_LAUNCHER
  // rexlauncher's menu, without its launch screen (the dashboard is the
  // launcher): F1 or Back + Start opens it over the dashboard. Window size
  // and display mode stay with the dashboard's Display settings, which also
  // drive launched games (blade_display.cpp); the menu has the rendering
  // options and the Controls page.
  void OnConfigureFonts(ImFontAtlas* atlas) override { rexlauncher::ConfigureFonts(atlas); }

  std::optional<rex::PathConfig> OnFinalizePaths(
      const rex::PathConfig& defaults, std::function<void(rex::PathConfig)> resume) override {
    (void)resume;
    rexlauncher::ConfigureInGameMenu({.title = "Blade", .display_options = false},
                                     defaults.config_path);
    return defaults;
  }

  // Dashboard text from strings.toml: rebuilds the XUI resource sections it
  // changes before the dashboard starts (see blade_strings.cpp).
  void OnPostLoadXexImage() override {
    blade::PrepareDashStrings(game_data_root());
    blade::PatchResolutionList();
  }
#endif

  // The dashboard is a flash title: it runs as \Device\Flash\dash.xex and opens
  // its companion files (gamercrd.xzp, fonts, ...) through media:, which points
  // at the flash. Mount the game folder (the flash dump) there read-only.
  void OnPostSetup() override {
    auto* fs = runtime()->file_system();
    auto flash = std::make_unique<rex::filesystem::HostPathDevice>(
        "\\Device\\Flash", std::filesystem::absolute(game_data_root()), true);
    if (flash->Initialize() && fs->RegisterDevice(std::move(flash))) {
      fs->RegisterSymbolicLink("media:", "\\Device\\Flash");
      fs->RegisterSymbolicLink("flash:", "\\Device\\Flash");
    }

    blade::LoadDisplaySettings(ConfigDir());

#if BLADE_LAUNCHER
    rexlauncher::EnableInGameMenu(imgui_drawer(), app_context(), runtime());
    blade::LoadLauncherConfig(game_data_root());

    // Display settings also drive Blade's own window: the saved Screen
    // Resolution and Display Mode now, and each change the dashboard saves.
    // (The window already exists: ReXApp creates it, and calls
    // OnCreateDialogs, before the runtime and OnPostSetup.)
    blade::SetDisplayChangedCallback(
        [this](const blade::DisplayChoice& choice) { ApplyDisplay(choice); });
    ApplyDisplay(blade::CurrentDisplayChoice());
#endif
  }

  // Where Blade's config files live: next to the executable in a packaged
  // build (rex dist), else the project folder (the game folder's parent).
  std::filesystem::path ConfigDir() const {
    auto exe_dir = rex::filesystem::GetExecutableFolder();
    if (std::filesystem::exists(exe_dir / "launcher.toml")) return exe_dir;
    return std::filesystem::absolute(game_data_root()).parent_path();
  }

#if BLADE_LAUNCHER
  void OnShutdown() override { blade::SetDisplayChangedCallback(nullptr); }

  void ApplyDisplay(const blade::DisplayChoice& choice) {
    app_context().CallInUIThread([this, choice] {
      auto* w = window();
      if (!w) return;
      // Size first: a window leaving fullscreen comes back at the new size.
      if (choice.width && choice.height) w->SetDesiredLogicalSize(choice.width, choice.height);
      if (choice.fullscreen) w->SetFullscreen(*choice.fullscreen);
    });
  }
#endif

  // Override virtual hooks for customization:
  // void OnPostInitLogging() override {}
  // void OnPreSetup(rex::RuntimeConfig& config) override {}
  // void OnLoadXexImage(std::string& xex_image) override {}
  // std::unique_ptr<rex::ui::ImGuiDialog> CreateAchievementsOverlay() override;
  // std::unique_ptr<rex::ui::AchievementNotificationDialog>
  // CreateAchievementNotificationDialog() override;
};
