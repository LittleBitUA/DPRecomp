/**
 * @file        ui/overlay/pso_compile_indicator_state.cpp
 *
 * @brief       Storage for the cross-module PSO compile counters (see header).
 */
#include <rex/ui/overlay/pso_compile_indicator_state.h>

namespace rex::graphics::d3d12 {

PsoCompileIndicatorState& PsoCompileIndicatorStateRef() {
  static PsoCompileIndicatorState state;
  return state;
}

}  // namespace rex::graphics::d3d12
