// deadlyprem - ReXGlue Recompiled Project
//
// Customize your app by overriding virtual hooks from rex::ReXApp.

#pragma once

#include <algorithm>
#include <cctype>
#include <filesystem>
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
        "Deadly Premonition Recompilation 1.1 (build " __DATE__ ") - ReXGlue 0.10 nightly - by «Little Bit»");
    if (window()) {
      window()->SetTitle("Deadly Premonition Recompilation | 1.1 «Little Bit»");
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
    // Saves / profile / shader cache (2026-09-06, fixed in 1.1). The SDK hands
    // us user_data_root already resolved to Documents\deadlyprem (its platform
    // user folder), so the 1.0 "if empty" check never fired and everything
    // silently stayed in Documents - the userdata\ folder next to the exe (and
    // the pre-warmed shader storage shipped in the zip) were never used.
    // v1.1: portable layout unconditionally unless --user_data_root /
    // --cache_root were given on the command line, plus a one-time migration
    // of the 1.0 state (saves under <XUID>\, shader storage under
    // cache\shaders\shareable\) from the legacy Documents location.
    const std::filesystem::path legacy_user_root = paths.user_data_root;
    if (!rex::cvar::HasNonDefaultValue("user_data_root")) {
      paths.user_data_root = rex::filesystem::GetExecutableFolder() / "userdata";
    }
    if (!rex::cvar::HasNonDefaultValue("cache_root")) {
      paths.cache_root = paths.user_data_root / "cache";
    }
    MigrateLegacyUserData(legacy_user_root, paths.user_data_root, paths.cache_root);
  }

  // One-time copy of the Documents\deadlyprem state into the portable layout.
  // Only fills what is missing at the destination; never deletes the source.
  static void MigrateLegacyUserData(const std::filesystem::path& legacy_root,
                                    const std::filesystem::path& user_root,
                                    const std::filesystem::path& cache_root) {
    namespace fs = std::filesystem;
    std::error_code ec;
    if (legacy_root.empty() || legacy_root == user_root || !fs::is_directory(legacy_root, ec)) {
      return;
    }
    size_t copied = 0;
    // Profiles / saves: <16 hex digits XUID>\...
    for (const auto& entry : fs::directory_iterator(legacy_root, ec)) {
      if (!entry.is_directory(ec)) continue;
      const std::string name = entry.path().filename().string();
      if (name.size() != 16 ||
          !std::all_of(name.begin(), name.end(), [](unsigned char c) { return std::isxdigit(c); })) {
        continue;
      }
      const fs::path dst = user_root / name;
      if (fs::exists(dst, ec)) continue;
      fs::create_directories(user_root, ec);
      fs::copy(entry.path(), dst, fs::copy_options::recursive, ec);
      if (!ec) {
        ++copied;
        REXLOG_INFO("Migrated profile {} from {} to {}", name, legacy_root.string(),
                    user_root.string());
      } else {
        REXLOG_WARN("Profile migration of {} failed: {}", name, ec.message());
        ec.clear();
      }
    }
    // Shader storage (portable, shareable): copy files that do not exist yet.
    const fs::path legacy_shareable = legacy_root / "cache" / "shaders" / "shareable";
    const fs::path dst_shareable = cache_root / "shaders" / "shareable";
    if (fs::is_directory(legacy_shareable, ec)) {
      for (const auto& entry : fs::directory_iterator(legacy_shareable, ec)) {
        if (!entry.is_regular_file(ec)) continue;
        const fs::path dst = dst_shareable / entry.path().filename();
        if (fs::exists(dst, ec)) continue;
        fs::create_directories(dst_shareable, ec);
        fs::copy_file(entry.path(), dst, ec);
        if (!ec) {
          ++copied;
        } else {
          ec.clear();
        }
      }
    }
    if (copied) {
      REXLOG_INFO("Legacy user data migration: {} item(s) copied from {}", copied,
                  legacy_root.string());
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
