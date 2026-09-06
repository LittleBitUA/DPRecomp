// deadlyprem - direct mouse camera control (DP1, 2026-09-06).
//
// Emulating a right stick from mouse motion is velocity control through the
// game's deadzone and acceleration curve, and it never feels like a mouse.
// Instead, the camera update routine of the game (PAL sub_82338A38) is hooked
// at 0x82338CA8 - the merge point right after the right-stick X contribution
// has been folded into this frame's yaw delta, executed every frame - and the
// raw mouse deltas (drained from the MnK driver) are added straight to the
// per-frame camera deltas the routine keeps on its object:
//   r31 + 108  delta pitch (right stick Y is added after this point, on top)
//   r31 + 112  delta yaw   (right stick X was added before this point)
// Signs follow the stick: stick right => yaw -= ..., stick up => pitch += ...
// While the hook is active the MnK driver stops mapping the mouse to the
// stick, so the two never fight.

#include <cstdint>

#include <rex/cvar.h>
#include <rex/input/mnk/mnk_input_driver.h>
#include <rex/logging.h>
#include <rex/memory/utils.h>
#include <rex/system/kernel_state.h>

#include "deadlyprem_pch.h"

REXCVAR_DEFINE_BOOL(dp_mouse_camera, true, "DP1",
                    "Mouse controls the camera directly (game camera hook) instead of "
                    "emulating the right stick")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_DOUBLE(dp_mouse_camera_sensitivity, 0.015, "DP1",
                      "Camera angle change per mouse pixel (game camera units)")
    .range(0.0001, 0.05)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(dp_mouse_camera_invert_y, false, "DP1", "Invert mouse Y for the camera hook")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(dp_mouse_camera_log, false, "DP1",
                    "Log the camera hook (first calls and every 300th) for diagnostics")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

// Shared body. pitch_sign: +1 when the routine does pitch += stick_y (0x82338A38,
// fmadds), -1 when it does pitch -= stick_y (0x8233AB28 walking camera, fnmsubs).
static void ApplyMouseToCamera(PPCRegister& r31, float pitch_sign, const char* tag) {
  static uint32_t call_count = 0;
  ++call_count;
  const bool active = REXCVAR_GET(dp_mouse_camera);
  if (rex::input::mnk::IsMouseCameraHookActive() != active) {
    rex::input::mnk::SetMouseCameraHookActive(active);
  }
  float dx = 0.0f, dy = 0.0f;
  rex::input::mnk::TakeMouseCameraDelta(dx, dy);
  const bool log_now = REXCVAR_GET(dp_mouse_camera_log) && (call_count <= 3 || (call_count % 300) == 0);
  auto* memory = REX_KERNEL_MEMORY();
  if (!memory) {
    return;
  }
  uint8_t* pitch_ptr = memory->TranslateVirtual<uint8_t*>(r31.u32 + 108);
  uint8_t* yaw_ptr = memory->TranslateVirtual<uint8_t*>(r31.u32 + 112);
  if (log_now) {
    REXLOG_INFO("DPCameraMouseHook[{}] #{}: active={} r31={:08X} dx={} dy={} pitch={} yaw={}", tag,
                call_count, active, r31.u32, dx, dy, rex::memory::load_and_swap<float>(pitch_ptr),
                rex::memory::load_and_swap<float>(yaw_ptr));
  }
  if (!active || (dx == 0.0f && dy == 0.0f)) {
    return;
  }
  const float k = float(REXCVAR_GET(dp_mouse_camera_sensitivity));
  const float yaw_step = -dx * k;
  // Mouse up (dy < 0) must act like stick up.
  const float stick_up = REXCVAR_GET(dp_mouse_camera_invert_y) ? dy : -dy;
  const float pitch_step = pitch_sign * stick_up * k;
  rex::memory::store_and_swap<float>(pitch_ptr,
                                     rex::memory::load_and_swap<float>(pitch_ptr) + pitch_step);
  rex::memory::store_and_swap<float>(yaw_ptr, rex::memory::load_and_swap<float>(yaw_ptr) + yaw_step);
}

// PAL 0x82338CA8 (sub_82338A38, per-frame deltas, pitch += stick_y).
void DPCameraMouseHook(PPCRegister& r31) { ApplyMouseToCamera(r31, +1.0f, "82338A38"); }

// PAL 0x8233B2A4 (sub_8233AB28, the walking gameplay camera found by tracing on
// 2026-09-06): merge point after the right-stick X contribution; +112 = yaw,
// +108 = pitch (absolute, clamped by the game right after the stick Y step),
// both updated as field -= stick * speed.
void DPCameraMouseHookWalk(PPCRegister& r31) { ApplyMouseToCamera(r31, -1.0f, "8233AB28"); }
