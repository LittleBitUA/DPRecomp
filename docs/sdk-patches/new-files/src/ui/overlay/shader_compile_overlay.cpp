/**
 * @file        ui/overlay/shader_compile_overlay.cpp
 *
 * @brief       Launch-time shader storage progress dialog. See header.
 */
#include <rex/ui/overlay/shader_compile_overlay.h>

#include <algorithm>
#include <cstdio>
#include <utility>

#include <imgui.h>

#include <rex/ui/overlay/pso_compile_indicator_state.h>

namespace rex::ui {

ShaderCompileDialog::ShaderCompileDialog(ImGuiDrawer* drawer, std::string title,
                                         std::string subtitle, CompleteCallback complete)
    : ImGuiDialog(drawer),
      title_(std::move(title)),
      subtitle_(std::move(subtitle)),
      complete_(std::move(complete)) {}

void ShaderCompileDialog::OnDraw(ImGuiIO& io) {
  const auto& state = rex::graphics::d3d12::PsoCompileIndicatorStateRef();
  const bool finished = state.storage_finished.load(std::memory_order_acquire);
  const uint32_t shaders = state.storage_shaders_translated.load(std::memory_order_relaxed);
  const uint32_t pipelines_total_now = state.storage_pipelines_total.load(std::memory_order_acquire);
  const uint32_t pipelines_created_now =
      state.storage_pipelines_created.load(std::memory_order_relaxed);
  peak_pipelines_created_ = std::max(peak_pipelines_created_, pipelines_created_now);
  peak_pipelines_total_ = std::max(peak_pipelines_total_, pipelines_total_now);
  peak_shaders_translated_ = std::max(peak_shaders_translated_, shaders);
  const uint32_t pipelines_created = std::min(peak_pipelines_created_, peak_pipelines_total_);
  const uint32_t pipelines_total = peak_pipelines_total_;
  const uint32_t shaders_translated = peak_shaders_translated_;

  const auto now = std::chrono::steady_clock::now();
  if (finished && !finished_seen_) {
    finished_seen_ = true;
    finished_at_ = now;
  }
  const auto elapsed_since_open = now - opened_at_;

  const float toast_width = 340.0f;
  ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x - 16.0f, 16.0f), ImGuiCond_Always,
                          ImVec2(1.0f, 0.0f));
  ImGui::SetNextWindowSize(ImVec2(toast_width, 0.0f), ImGuiCond_Always);
  ImGui::SetNextWindowBgAlpha(0.92f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10.0f, 8.0f));
  ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(6.0f, 3.0f));
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(6.0f, 5.0f));
  if (ImGui::Begin(title_.c_str(), nullptr,
                   ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize |
                       ImGuiWindowFlags_NoMove | ImGuiWindowFlags_AlwaysAutoResize |
                       ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                       ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs)) {
    char status[160];
    if (finished_seen_) {
      if (pipelines_total > 0) {
        std::snprintf(status, sizeof(status), "Ready: %u shaders, %u pipelines in %.1f s",
                      shaders_translated, pipelines_total,
                      std::chrono::duration<double>(finished_at_ - opened_at_).count());
      } else {
        std::snprintf(status, sizeof(status), "Shader cache empty (first run: expect stutter)");
      }
    } else if (pipelines_total > 0) {
      std::snprintf(status, sizeof(status), "Pipelines %u / %u", pipelines_created,
                    pipelines_total);
    } else if (shaders_translated > 0) {
      std::snprintf(status, sizeof(status), "Translating shaders: %u", shaders_translated);
    } else {
      std::snprintf(status, sizeof(status), "Loading shader storage...");
    }
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextUnformatted(status);
    if (!subtitle_.empty()) {
      ImGui::TextDisabled("%s", subtitle_.c_str());
    }
    ImGui::PopTextWrapPos();
    float fraction = 0.0f;
    if (finished_seen_) {
      fraction = 1.0f;
    } else if (pipelines_total > 0) {
      fraction = std::clamp(float(double(pipelines_created) / double(pipelines_total)), 0.0f, 1.0f);
    }
    ImGui::ProgressBar(fraction, ImVec2(-1.0f, 6.0f), "");
  }
  ImGui::End();
  ImGui::PopStyleVar(3);

  if (finished && !completion_fired_) {
    const bool min_duration_met = elapsed_since_open >= kMinDisplayDuration;
    const bool post_finished_hold_met = (now - finished_at_) >= kPostFinishedHoldDuration;
    if (min_duration_met && post_finished_hold_met) {
      completion_fired_ = true;
      auto cb = std::move(complete_);
      Close();
      if (cb) {
        cb();
      }
      return;  // the drawer deletes this dialog after OnDraw returns
    }
  }
}

}  // namespace rex::ui
