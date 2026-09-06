/**
 * @file        rex/ui/overlay/pso_compile_indicator_overlay.h
 *
 * @brief       Runtime PSO compile indicator overlay (DP1 port of the Downpour
 *              fork, 2026-09-06). A small corner badge that appears while
 *              background pipeline compiles are in flight (or right after an
 *              inline compile stall) and auto-hides shortly after.
 */
#pragma once

#include <chrono>

#include <rex/ui/imgui_dialog.h>

namespace rex::ui {

class PsoCompileIndicatorDialog : public ImGuiDialog {
 public:
  explicit PsoCompileIndicatorDialog(ImGuiDrawer* drawer) : ImGuiDialog(drawer) {}

 protected:
  void OnDraw(ImGuiIO& io) override;

 private:
  bool was_ever_active_ = false;
  std::chrono::steady_clock::time_point became_idle_at_{};
};

}  // namespace rex::ui
