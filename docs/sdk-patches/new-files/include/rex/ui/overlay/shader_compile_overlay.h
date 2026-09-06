/**
 * @file        rex/ui/overlay/shader_compile_overlay.h
 *
 * @brief       Launch-time shader storage progress dialog (DP1 port of the
 *              Downpour fork, 2026-09-06). Polls the storage replay counters
 *              in PsoCompileIndicatorStateRef() every frame and shows a
 *              "PSO N / M" toast until the persistent shader storage has
 *              finished translating shaders and creating cached pipelines;
 *              then fires the completion callback once and closes itself.
 *
 * Allocate with `new`: the dialog registers itself with the drawer in the
 * ImGuiDialog constructor and is deleted by the drawer after Close().
 */
#pragma once

#include <chrono>
#include <functional>
#include <string>

#include <rex/ui/imgui_dialog.h>

namespace rex::ui {

class ShaderCompileDialog final : public ImGuiDialog {
 public:
  using CompleteCallback = std::function<void()>;

  ShaderCompileDialog(ImGuiDrawer* drawer, std::string title, std::string subtitle,
                      CompleteCallback complete);

 protected:
  void OnDraw(ImGuiIO& io) override;

 private:
  std::string title_;
  std::string subtitle_;
  CompleteCallback complete_;
  bool completion_fired_ = false;
  uint32_t peak_pipelines_created_ = 0;
  uint32_t peak_pipelines_total_ = 0;
  uint32_t peak_shaders_translated_ = 0;
  std::chrono::steady_clock::time_point opened_at_ = std::chrono::steady_clock::now();
  std::chrono::steady_clock::time_point finished_at_{};
  bool finished_seen_ = false;
  static constexpr std::chrono::milliseconds kMinDisplayDuration{1200};
  static constexpr std::chrono::milliseconds kPostFinishedHoldDuration{500};
};

}  // namespace rex::ui
