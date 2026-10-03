// [new_fix_03102026_effects]
// Tests for fixed-step effects at 60 FPS (src/deadlyprem_effect_step.h).
// Plain executable, exit code 0 = pass. Build target dp_effect_step_test.
#include <cstdint>
#include <cstdio>

#include "../src/deadlyprem_effect_step.h"

namespace {

int g_failures = 0;

#define CHECK(cond)                                               \
  do {                                                            \
    if (!(cond)) {                                                \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      ++g_failures;                                               \
    }                                                             \
  } while (0)

// Effect time advanced per real second: updates per second x delta per update.
double PerSecond(double fps, double delta) { return fps * delta; }

}  // namespace

int main() {
  using dp::FixedStepEffectDelta;
  using dp::IsFixedStepEffect;

  // The flag: bit 1 of +604 only.
  CHECK(IsFixedStepEffect(0x2));
  CHECK(IsFixedStepEffect(0xFFFFFFFF));
  CHECK(!IsFixedStepEffect(0x0));
  CHECK(!IsFixedStepEffect(0x1));
  CHECK(!IsFixedStepEffect(0x4));
  CHECK(!IsFixedStepEffect(0xFFFFFFFD));

  // The console at 30 FPS: tick 2, fixed-step effects get exactly 1.0.
  CHECK(FixedStepEffectDelta(2.0) == 1.0);

  // The bug: at 60 FPS the constant 1.0 per frame is twice the console's pace.
  const double console = PerSecond(30.0, 1.0);
  CHECK(PerSecond(60.0, 1.0) == 2.0 * console);

  // The fix: half the tick (1.0 at 60 FPS) keeps the console's pace...
  CHECK(PerSecond(60.0, FixedStepEffectDelta(1.0)) == console);
  // ...and a catch-up frame (tick 2) still advances by what two frames would.
  CHECK(FixedStepEffectDelta(2.0) == 2.0 * FixedStepEffectDelta(1.0));
  // Fractional ticks (dp_60fps_integer_tick off) scale the same way.
  CHECK(FixedStepEffectDelta(1.2) == 0.6);

  // Ordinary effects relative to fixed-step ones stay 2:1, as on the console.
  CHECK(1.0 / FixedStepEffectDelta(1.0) == 2.0);

  if (g_failures == 0) std::printf("dp_effect_step_test: all passed\n");
  return g_failures == 0 ? 0 : 1;
}
