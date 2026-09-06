// DP1 2026-09-06: AcquireWizardDialog ported from the Downpour SDK fork (rex/ui/overlay/acquire_wizard_overlay) into the app, since the nightly SDK has no such overlay.
/**
 * @file        rex/ui/overlay/acquire_wizard_overlay.h
 *
 * @brief       Generic pre-runtime acquisition dialog: install a payload either
 *              by fetching it automatically (e.g. a download) or from a
 *              user-selected source file.
 */
#pragma once

#include <atomic>
#include <filesystem>
#include <functional>
#include <string>
#include <thread>

#include <rex/ui/imgui_dialog.h>

namespace deadlyprem {
using rex::ui::ImGuiDialog;
using rex::ui::ImGuiDrawer;

class AcquireWizardDialog final : public ImGuiDialog {
 public:
  struct Options {
    std::string title;
    std::string intro;
    std::string target_directory;
    std::string initial_status;
    // Leave a label empty to hide the corresponding button.
    std::string fetch_button_label;
    std::string pick_button_label;
    // Shown while a fetch is in progress but no bytes have arrived yet (e.g.
    // waiting on the server's first byte). Falls back to fetch_working_status
    // if empty.
    std::string fetch_connecting_status;
    std::string fetch_working_status;
    std::string install_working_status;
    std::string done_status;
    std::string done_button_label;
  };

  using PickSourceCallback = std::function<std::filesystem::path()>;
  using InstallCallback = std::function<bool(const std::filesystem::path& source,
                                             std::atomic<uint64_t>& copied_bytes,
                                             std::atomic<uint64_t>& total_bytes,
                                             std::string& error)>;
  using FetchCallback = std::function<bool(std::atomic<uint64_t>& copied_bytes,
                                           std::atomic<uint64_t>& total_bytes,
                                           std::string& error)>;
  using CompleteCallback = std::function<void()>;

  AcquireWizardDialog(ImGuiDrawer* drawer, Options options, FetchCallback fetch,
                      PickSourceCallback pick_source, InstallCallback install,
                      CompleteCallback complete);

 protected:
  void OnClose() override;
  void OnDraw(ImGuiIO& io) override;

 private:
  enum class State {
    kWaitingForChoice,
    kWorking,
    kDone,
    kFailed,
  };

  void StartWork(std::function<bool(std::string&)> work, std::string busy_status);
  void StartFetch();
  void PickSourceAndInstall();
  void FinishWorkIfNeeded();
  const std::string& WorkingStatus() const;

  Options options_;
  FetchCallback fetch_;
  PickSourceCallback pick_source_;
  InstallCallback install_;
  CompleteCallback complete_;
  std::thread work_thread_;
  std::atomic<bool> work_done_{false};
  std::atomic<bool> work_ok_{false};
  std::atomic<uint64_t> copied_bytes_{0};
  std::atomic<uint64_t> total_bytes_{0};
  State state_ = State::kWaitingForChoice;
  bool working_is_fetch_ = false;
  std::filesystem::path source_path_;
  std::string status_;
  std::string error_;
};

}  // namespace deadlyprem
