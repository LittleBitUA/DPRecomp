// deadlyprem - ReXGlue Recompiled Project
//
// Customize your app by overriding virtual hooks from rex::ReXApp.

#pragma once

#include <cstdlib>
#include <functional>
#include <optional>

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/logging.h>
#include <rex/rex_app.h>
#include <rex/ui/overlay/debug_overlay.h>

#include "deadlyprem_iso_installer.h"

class DeadlypremApp : public rex::ReXApp {
 public:
  using rex::ReXApp::ReXApp;

  static std::unique_ptr<rex::ui::WindowedApp> Create(
      rex::ui::WindowedAppContext& ctx) {
    return std::unique_ptr<DeadlypremApp>(new DeadlypremApp(ctx, "deadlyprem",
        PPCImageConfig));
  }

  void OnPreSetup(rex::RuntimeConfig& config) override {
    // ReXGlue 0.10 loads GPU emulation as a plugin (rexgpu-xenos*.dll, staged
    // next to the exe by GPU_PLUGINS in CMakeLists.txt). The SDK default is
    // "" = no GPU at all, which boots to a black window with
    // "VdInitializeRingBuffer: no GPU emulation loaded" in the log
    // (2026-09-05). The game always needs Xenos emulation, so default it here
    // and let deadlyprem.toml / --gpu_plugin still override.
    if (config.gpu_plugin.empty()) {
      config.gpu_plugin = "xenos";
    }
  }

  // First-run install chain (ported 2026-09-06 from DownpourRecomp PR #27 by
  // Alexbeav): game data missing -> the disc image (ISO) wizard extracts the
  // user's own dump into game_data_root. DP_INSTALL_ISO=<path> env override
  // serves headless installs. No title update step: DP1 has none.
  std::optional<rex::PathConfig> OnFinalizePaths(
      const rex::PathConfig& defaults,
      std::function<void(rex::PathConfig)> resume) override {
    // Window title (user request 2026-09-06). The SDK sets "<name> <build
    // stamp>" in SetupPresentation; this hook runs right after it.
    // F3 debug overlay watermark (ASCII only: the overlay font has no special glyphs).
    rex::ui::SetDebugOverlayBuildStamp(
        "Deadly Premonition Recompilation 1.0 USA (build " __DATE__ ") - ReXGlue 0.10 nightly - by «Little Bit»");
    if (window()) {
      window()->SetTitle("Deadly Premonition Recompilation (USA) | 1.0 «Little Bit»");
    }
    rex::PathConfig runtime_paths = defaults;
    const auto& game_root = runtime_paths.game_data_root;
    if (!deadlyprem::IsGameDataInstalled(game_root)) {
      if (const char* iso = std::getenv("DP_INSTALL_ISO"); iso != nullptr && *iso != '\0') {
        std::string error;
        REXLOG_INFO("Installing game data from DP_INSTALL_ISO={}", iso);
        if (!deadlyprem::InstallGameDataFromIso(iso, game_root, nullptr, nullptr, error)) {
          REXLOG_ERROR("Automated game data installation failed: {}", error);
        }
      }
    }
    if (deadlyprem::IsGameDataInstalled(game_root)) {
      return runtime_paths;
    }
    REXLOG_INFO("Deadly Premonition game data not found at {}; launching the disc image installer.",
                game_root.string());
    deadlyprem::ShowIsoInstallWizard(imgui_drawer(), std::move(runtime_paths), std::move(resume));
    return std::nullopt;
  }

  void OnConfigurePaths(rex::PathConfig& paths) override {
    // The SDK samples game_data_root from the cvar BEFORE deadlyprem.toml is
    // loaded (rex_app.cpp SetupEnvironment: cvar read at the top, LoadConfig
    // after OnConfigurePaths), so a toml-only value never reaches it and the
    // runtime dies with "--game_data_root was not provided". Default to the
    // conventional <exe dir>\assets like every DPRecomp release so far; an
    // explicit --game_data_root on the command line still wins.
    if (paths.game_data_root.empty()) {
      paths.game_data_root = rex::filesystem::GetExecutableFolder() / "assets";
    }
    // Saves / profile (2026-09-06). The SDK default is user_data_root =
    // game_data_root, i.e. inside assets\ (which may be a junction to another
    // install here). v0.1.1 used Documents\deadlyprem, which OneDrive redirects
    // and dehydrates after reboots - the likely cause of the "save sequence
    // loops forever after a restart" report (DPRecomp #6). Keep user data
    // next to the exe instead; --user_data_root still overrides.
    if (paths.user_data_root.empty()) {
      paths.user_data_root = rex::filesystem::GetExecutableFolder() / "userdata";
    }
    // The SDK derives cache_root from the Documents user dir BEFORE this hook
    // runs, so move it next to the exe as well (portable layout, like the
    // Downpour release): <exe>Serdatache holds the shareable shader /
    // pipeline storage (.xsh / .xpso).
    if (!rex::cvar::HasNonDefaultValue("cache_root")) {
      paths.cache_root = paths.user_data_root / "cache";
    }
  }

  // Other virtual hooks available for customization:
  // void OnPostInitLogging() override {}
  // void OnLoadXexImage(std::string& xex_image) override {}
  // void OnPostLoadXexImage() override {}
  // void OnPostSetup() override {}
  // void OnCreateDialogs(rex::ui::ImGuiDrawer* drawer) override {}
  // std::unique_ptr<rex::ui::ImGuiDialog> CreateAchievementsOverlay() override;
  // std::unique_ptr<rex::ui::AchievementNotificationDialog>
  // CreateAchievementNotificationDialog() override;
  // void OnShutdown() override {}
  // void OnConfigurePaths(rex::PathConfig& paths) override {}
};
