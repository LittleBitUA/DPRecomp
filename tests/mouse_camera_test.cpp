// [new_fix_03102026_mouselook]
// Tests for the mode-dependent mouse camera rules (src/deadlyprem_mouse_camera.h).
// Plain executable, exit code 0 = pass. Build target dp_mouse_camera_test.
#include <cmath>
#include <cstdint>
#include <cstdio>

#include "../src/deadlyprem_mouse_camera.h"

namespace {

int g_failures = 0;

#define CHECK(cond)                                               \
  do {                                                            \
    if (!(cond)) {                                                \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      ++g_failures;                                               \
    }                                                             \
  } while (0)

bool Near(float a, float b, float eps = 1e-3f) { return std::fabs(a - b) <= eps; }

// The game's chase camera target for one frame with the right stick at full
// deflection (PAL sub_82338A38 view 0): offset = (1 - 0.25) * reach * 4/3.
float StickFullDeflection(float reach_degrees) { return (1.0f - 0.25f) * reach_degrees * (4.0f / 3.0f); }

}  // namespace

int main() {
  using dp::CarHeldAngle;
  const float deg = dp::kRadiansPerDegree;

  // The mouse may hold exactly what the stick reaches: full deflection = the
  // view record's degrees.
  CHECK(Near(StickFullDeflection(30.0f), 30.0f));

  // While the mouse moves, the offset adds up and stays (the bug: the stick
  // emulation was zero in every frame without motion -> back behind the car).
  {
    float yaw = 0.0f;
    yaw = CarHeldAngle(yaw, 0.1f, -30.0f * deg, 30.0f * deg, true);
    for (int i = 0; i < 59; ++i) yaw = CarHeldAngle(yaw, 0.0f, -30.0f * deg, 30.0f * deg, true);
    CHECK(Near(yaw, 0.1f, 1e-6f));
  }

  // Bounded by the stick's reach on each side; pitch up / down differ.
  {
    float yaw = 0.0f, pitch = 0.0f;
    for (int i = 0; i < 100; ++i) {
      yaw = CarHeldAngle(yaw, -0.05f, -30.0f * deg, 30.0f * deg, true);
      pitch = CarHeldAngle(pitch, 0.05f, -10.0f * deg, 20.0f * deg, true);
    }
    CHECK(Near(yaw, -30.0f * deg, 1e-5f));
    CHECK(Near(pitch, 20.0f * deg, 1e-5f));
    for (int i = 0; i < 100; ++i) pitch = CarHeldAngle(pitch, -0.05f, -10.0f * deg, 20.0f * deg, true);
    CHECK(Near(pitch, -10.0f * deg, 1e-5f));
  }

  // After the hold time the offset goes, and the game's smoothing brings the
  // camera back as after a stick release.
  CHECK(CarHeldAngle(0.4f, 0.0f, -1.0f, 1.0f, false) == 0.0f);
  CHECK(CarHeldAngle(0.4f, 0.2f, -1.0f, 1.0f, false) == 0.0f);

  // The view record the routine reads (views 1-2: r27 + (view + r28) * 36).
  CHECK(dp::CarViewRecord(0x82100000u, 3u, 1u) == 0x82100000u + 4u * 36u);
  CHECK(dp::CarViewRecord(0x82100000u, 0u, 0u) == 0x82100000u);
  CHECK(dp::kCarViewYaw == 28 && dp::kCarViewPitchUp == 20 && dp::kCarViewPitchDown == 24);

  // Walking pitch: the mouse may hold the pitch as far as the stick can (30
  // degrees), not 0.9 rad. Trace 044 (03.10): resting pitch 0.112, a quick
  // flick up drove the pending pitch to -0.9 -> target -0.788, past the game's
  // -45 degree clamp (-0.785): the camera sat on the clamp, in the ground.
  {
    const float resting = 0.112f;
    const float limit = dp::PendingPitchLimit(dp::kCameraModeWalk, 0.9f);
    CHECK(Near(limit, 0.5236f, 1e-3f));
    CHECK(resting - limit > -0.7853982f);  // stays above the game's clamp
    CHECK(resting - 0.9f < -0.7853982f);   // the old bound did not
    CHECK(dp::PendingPitchLimit(2, 0.9f) == 0.9f);  // modes not measured keep the fallback
  }

  // Walking outdoors (mode 0) and modes 7, 13, 14: stick reach 22.5 degrees,
  // clamp -30 degrees. Trace 046 (03.10, USA near SWERY '65): resting 0.0898,
  // pending -0.9 -> target -0.81, past the -30 degree clamp (-0.5236).
  {
    const float resting = 0.0898f;
    for (uint32_t mode : {0u, 7u, 13u, 14u}) {
      const float limit = dp::PendingPitchLimit(mode, 0.9f);
      CHECK(Near(limit, 0.3927f, 1e-3f));
      CHECK(resting - limit > -0.5235988f);
    }
    CHECK(resting - 0.9f < -0.5235988f);  // the old bound did not
  }

  if (g_failures == 0) std::printf("dp_mouse_camera_test: all passed\n");
  return g_failures == 0 ? 0 : 1;
}
