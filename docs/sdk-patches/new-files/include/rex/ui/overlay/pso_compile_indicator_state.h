/**
 * @file        rex/ui/overlay/pso_compile_indicator_state.h
 *
 * @brief       Lightweight cross-module hook for the PSO compile indicator
 *              overlay and the launch-time shader storage progress dialog.
 *
 * DP1 port (2026-09-06) of the Downpour fork design: the pipeline cache lives
 * in the GPU plugin (rexgpu-xenos*.dll) while the overlays live in rexui
 * (rexruntime), so the counters are owned by the runtime (defined in
 * src/ui/overlay/pso_compile_indicator_state.cpp, exported) and the plugin
 * writes into them through PsoCompileIndicatorStateRef().
 */
#pragma once

#include <atomic>
#include <cstdint>

namespace rex::graphics::d3d12 {

struct PsoCompileIndicatorState {
  // Pipelines currently being created on a background creation thread.
  std::atomic<uint32_t> creation_threads_busy{0};
  // Pipelines waiting in the priority queue for a free creation thread.
  std::atomic<uint32_t> creation_queue_size{0};
  // Cumulative count of cache misses (new pipelines requested by draws).
  std::atomic<uint64_t> total_misses_session{0};
  // Cumulative count of PSO creations finished this session (including the
  // launch-time storage replay).
  std::atomic<uint64_t> total_completions_session{0};
  // steady_clock nanoseconds since epoch of the last completed creation.
  std::atomic<uint64_t> last_completion_steady_ns{0};
  // PSOs added to the ID3D12PipelineLibrary this session.
  std::atomic<uint64_t> library_stores_session{0};
  // Inline (synchronous, draw-blocking) compiles.
  std::atomic<uint64_t> sync_compiles_session{0};
  std::atomic<uint64_t> last_sync_compile_steady_ns{0};
  std::atomic<uint32_t> last_sync_compile_duration_ms_bits{0};

  // Launch-time shader storage replay progress (InitializeShaderStorage).
  std::atomic<uint32_t> storage_shaders_translated{0};
  std::atomic<uint32_t> storage_pipelines_total{0};
  std::atomic<uint32_t> storage_pipelines_created{0};
  std::atomic<bool> storage_finished{false};
};

PsoCompileIndicatorState& PsoCompileIndicatorStateRef();

}  // namespace rex::graphics::d3d12
