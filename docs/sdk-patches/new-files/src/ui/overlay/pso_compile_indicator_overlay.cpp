/**
 * @file        ui/overlay/pso_compile_indicator_overlay.cpp
 *
 * @brief       Runtime PSO compile indicator overlay. See header.
 */
#include <rex/ui/overlay/pso_compile_indicator_overlay.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>

#include <imgui.h>

#include <rex/cvar.h>
#include <rex/ui/overlay/pso_compile_indicator_state.h>

REXCVAR_DEFINE_BOOL(show_shader_compile_indicator, true, "UI",
                    "Show a small corner badge while shaders / pipelines are being compiled "
                    "(new scene = new pipelines). Auto-hides ~1.2 s after the queue empties.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_STRING(shader_compile_indicator_corner, "top_right", "UI",
                      "Corner of the compile indicator: 'top_left', 'top_right', 'bottom_left', "
                      "'bottom_right'.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(shader_compile_indicator_verbose, false, "UI",
                    "Add counters (pending, session misses / completions / library stores) "
                    "under the indicator line.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace rex::ui {

namespace {

constexpr std::chrono::milliseconds kIdleGracePeriod{1200};

const char* SpinnerFrame() {
  static const char* kFrames[] = {".  ", ".. ", "...", " ..", "  .", "   "};
  auto now = std::chrono::steady_clock::now().time_since_epoch();
  auto idx = (std::chrono::duration_cast<std::chrono::milliseconds>(now).count() / 120) %
             (sizeof(kFrames) / sizeof(kFrames[0]));
  return kFrames[idx];
}

ImVec2 ChooseAnchor(const ImGuiIO& io, const std::string& corner_value) {
  constexpr float kMargin = 12.0f;
  if (corner_value == "top_right") {
    return ImVec2(io.DisplaySize.x - kMargin, kMargin);
  }
  if (corner_value == "bottom_left") {
    return ImVec2(kMargin, io.DisplaySize.y - kMargin);
  }
  if (corner_value == "bottom_right") {
    return ImVec2(io.DisplaySize.x - kMargin, io.DisplaySize.y - kMargin);
  }
  return ImVec2(kMargin, kMargin);
}

ImVec2 ChoosePivot(const std::string& corner_value) {
  if (corner_value == "top_right") return ImVec2(1.0f, 0.0f);
  if (corner_value == "bottom_left") return ImVec2(0.0f, 1.0f);
  if (corner_value == "bottom_right") return ImVec2(1.0f, 1.0f);
  return ImVec2(0.0f, 0.0f);
}

}  // namespace

void PsoCompileIndicatorDialog::OnDraw(ImGuiIO& io) {
  if (!REXCVAR_GET(show_shader_compile_indicator)) {
    return;
  }
  auto& state = rex::graphics::d3d12::PsoCompileIndicatorStateRef();
  const uint32_t busy = state.creation_threads_busy.load(std::memory_order_relaxed);
  const uint32_t queued = state.creation_queue_size.load(std::memory_order_relaxed);
  const uint64_t pending = uint64_t(busy) + uint64_t(queued);

  const auto now = std::chrono::steady_clock::now();
  if (pending > 0) {
    was_ever_active_ = true;
    became_idle_at_ = std::chrono::steady_clock::time_point{};
  } else if (was_ever_active_ && became_idle_at_.time_since_epoch().count() == 0) {
    became_idle_at_ = now;
  }

  const uint64_t last_completion_ns =
      state.last_completion_steady_ns.load(std::memory_order_relaxed);
  bool recent_async_completion = false;
  if (last_completion_ns != 0) {
    std::chrono::steady_clock::time_point last_completion_tp{
        std::chrono::nanoseconds{last_completion_ns}};
    recent_async_completion = (now - last_completion_tp) < kIdleGracePeriod;
  }

  const uint64_t last_sync_ns = state.last_sync_compile_steady_ns.load(std::memory_order_relaxed);
  bool recent_sync_compile = false;
  if (last_sync_ns != 0) {
    std::chrono::steady_clock::time_point last_sync_tp{std::chrono::nanoseconds{last_sync_ns}};
    recent_sync_compile = (now - last_sync_tp) < kIdleGracePeriod;
  }

  bool visible = pending > 0;
  if (!visible && was_ever_active_) {
    visible = (now - became_idle_at_) < kIdleGracePeriod;
  }
  if (!visible && (recent_async_completion || recent_sync_compile)) {
    visible = true;
  }
  if (!visible) {
    return;
  }

  const std::string corner = REXCVAR_GET(shader_compile_indicator_corner);
  ImGui::SetNextWindowPos(ChooseAnchor(io, corner), ImGuiCond_Always, ChoosePivot(corner));
  ImGui::SetNextWindowBgAlpha(0.6f);
  ImGui::PushStyleColor(ImGuiCol_Border, IM_COL32(200, 160, 40, 180));
  ImGui::PushStyleColor(ImGuiCol_WindowBg, IM_COL32(20, 20, 20, 200));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.0f, 5.0f));
  if (ImGui::Begin("##pso_compile_indicator", nullptr,
                   ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs |
                       ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoSavedSettings |
                       ImGuiWindowFlags_NoFocusOnAppearing |
                       ImGuiWindowFlags_AlwaysAutoResize)) {
    if (pending > 0) {
      ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.20f, 0.95f), "Compiling shaders%s",
                         SpinnerFrame());
      if (REXCVAR_GET(shader_compile_indicator_verbose)) {
        ImGui::Text("%u pending", uint32_t(pending));
      }
    } else if (recent_sync_compile) {
      uint32_t bits = state.last_sync_compile_duration_ms_bits.load(std::memory_order_relaxed);
      float duration_ms = 0.0f;
      std::memcpy(&duration_ms, &bits, sizeof(duration_ms));
      ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.20f, 0.95f), "Shader stall %.0f ms",
                         duration_ms);
    } else {
      ImGui::TextColored(ImVec4(0.6f, 0.9f, 0.6f, 0.95f), "Shaders ready");
    }
    if (REXCVAR_GET(shader_compile_indicator_verbose)) {
      ImGui::Separator();
      ImGui::Text("misses: %llu  done: %llu  lib: %llu",
                  static_cast<unsigned long long>(
                      state.total_misses_session.load(std::memory_order_relaxed)),
                  static_cast<unsigned long long>(
                      state.total_completions_session.load(std::memory_order_relaxed)),
                  static_cast<unsigned long long>(
                      state.library_stores_session.load(std::memory_order_relaxed)));
    }
  }
  ImGui::End();
  ImGui::PopStyleVar(2);
  ImGui::PopStyleColor(2);
}

}  // namespace rex::ui
