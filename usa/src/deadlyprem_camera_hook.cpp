// deadlyprem - direct mouse camera control (DP1, 2026-09-06, reworked for 1.1).
//
// Emulating a right stick from mouse motion is velocity control through the
// game's deadzone and acceleration curve, and it never feels like a mouse.
// Instead the camera update routines of the game are hooked at the merge
// point right after the right-stick X contribution has been folded into the
// frame's yaw, and the raw mouse deltas (drained from the MnK driver) are
// applied to the camera fields the routine keeps on the camera controller
// object (r31 = 0x82928AD0 on PAL):
//   r31 + 108  pitch,  r31 + 112  yaw
// Two families exist (see deadlyprem_config.toml):
//   * "delta" routines (PAL sub_82338A38 & co, fmadds): the fields are the
//     per-frame deltas; stick right => yaw -= ..., stick up => pitch += ...
//   * "absolute" routines (walking camera sub_8233AB28, aim sub_82339DF8,
//     fnmsubs): every frame the routine RESETS yaw from the camera anchor
//     (anchor+128) and pitch from atan2 of the view direction, then adds
//     the stick on top; both fields are TARGETS that a spring follows and
//     they are re-derived next frame. Adding the mouse delta there behaves
//     like a stick nudge: only the fraction the spring consumes in one frame
//     survives, the rest is forgotten -> "slow", frame-rate dependent, and the
//     auto-centering wins as soon as the mouse stops (DPRecomp #11).
// v1.1 fixes for the absolute family:
//   1. catch-up: the un-consumed angle stays pending. Each frame the hook
//      measures how far the camera actually moved since the previous frame,
//      subtracts that from the pending angle, adds the new mouse motion and
//      writes target = current + pending. N pixels = a fixed angle, whatever
//      the spring or the frame rate do. Idle for dp_mouse_camera_hold_ms
//      releases the pending angle so the game's own behaviour resumes.
//   2. direct: the anchor pointer is captured by DPCameraAnchorHook at the
//      `lfs f0,128(r3)` that reads the current yaw (PAL 0x8233B220 walking,
//      0x82339EBC aim) and the yaw step is also added to anchor+128, the
//      camera yaw state itself, so the turn is immediate instead of sprung.
// While the hook is active the MnK driver stops mapping the mouse to the
// stick, so the two never fight.

#include <algorithm>
#include <chrono>
#include <cmath>
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
REXCVAR_DEFINE_DOUBLE(dp_mouse_camera_sensitivity, 0.003, "DP1",
                      "Camera angle change per mouse pixel (radians; 0.003 = one full turn "
                      "per ~2100 px)")
    .range(0.0002, 0.05)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(dp_mouse_camera_invert_y, false, "DP1", "Invert mouse Y for the camera hook")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(dp_mouse_camera_direct, true, "DP1",
                    "Also turn the camera yaw state itself (anchor) by the mouse step so the "
                    "turn is immediate instead of following the game's camera spring")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(dp_mouse_camera_catchup, true, "DP1",
                    "Keep the mouse angle the camera has not reached yet pending across frames "
                    "(position control) instead of a one-frame stick-like nudge")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_INT32(dp_mouse_camera_hold_ms, 120, "DP1",
                     "Idle mouse time after which the pending camera angle is released and "
                     "the game's own camera behaviour (auto-centering) resumes")
    .range(0, 2000)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(dp_mouse_camera_log, false, "DP1",
                    "Log the camera hook (first calls and every 300th) for diagnostics")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace {

constexpr float kPi = 3.14159265358979f;
constexpr float kMaxPendingYaw = 1.5f;    // radians the hook may stay "ahead" of the camera
constexpr float kMaxPendingPitch = 0.9f;  // the game clamps pitch itself; keep the residue bounded

float WrapAngle(float a) {
  while (a > kPi) a -= 2.0f * kPi;
  while (a < -kPi) a += 2.0f * kPi;
  return a;
}

float LoadF32(uint8_t* p) { return rex::memory::load_and_swap<float>(p); }
void StoreF32(uint8_t* p, float v) { rex::memory::store_and_swap<float>(p, v); }

// Anchor (camera state object) captured by DPCameraAnchorHook in the absolute
// routines right before the mouse hook of the same routine runs. The serial
// makes sure the pointer used comes from this very call.
uint32_t g_anchor = 0;
uint64_t g_anchor_serial = 0;
uint64_t g_anchor_serial_applied = 0;

struct CatchUpState {
  float pending_yaw = 0.0f;
  float pending_pitch = 0.0f;
  float prev_yaw = 0.0f;
  float prev_pitch = 0.0f;
  bool have_prev = false;
  const char* tag = nullptr;
  std::chrono::steady_clock::time_point last_call{};
  std::chrono::steady_clock::time_point last_mouse{};
};
CatchUpState g_catchup;

}  // namespace

// Aim camera (PAL sub_8233B3C0, hooked at its anchor read 0x8233B87C): it is
// driven by the LEFT stick through its own velocity model (+144 target,
// +128 smoothed offset, yaw = base + offset), and +112 is rebuilt every
// frame, so the target-based hook cannot steer it. Report the frame to the
// MnK driver, which then maps the mouse onto the left stick instead.
void DPCameraAimHook() { rex::input::mnk::NoteMouseAimFrame(); }

// Absolute routines, at `lfs f0,128(r3)` (r3 = anchor): remember the anchor.
void DPCameraAnchorHook(PPCRegister& r3) {
  g_anchor = r3.u32;
  ++g_anchor_serial;
}

// Shared body. pitch_sign: +1 when the routine does pitch += stick_y (fmadds
// family, per-frame deltas), -1 when it does pitch -= stick_y (fnmsubs family,
// absolute targets: walking camera sub_8233AB28, aim, ...).
static void ApplyMouseToCamera(PPCRegister& r31, float pitch_sign, const char* tag) {
  using clock = std::chrono::steady_clock;
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
  const float base_pitch = LoadF32(pitch_ptr);
  const float base_yaw = LoadF32(yaw_ptr);
  if (log_now) {
    REXLOG_INFO("DPCameraMouseHook[{}] #{}: active={} r31={:08X} anchor={:08X} dx={} dy={} pitch={} yaw={} pend=({}, {})",
                tag, call_count, active, r31.u32, g_anchor, dx, dy, base_pitch, base_yaw,
                g_catchup.pending_yaw, g_catchup.pending_pitch);
  }
  if (!active) {
    return;
  }
  const float k = float(REXCVAR_GET(dp_mouse_camera_sensitivity));
  const float yaw_step = -dx * k;
  // Mouse up (dy < 0) must act like stick up.
  const float stick_up = REXCVAR_GET(dp_mouse_camera_invert_y) ? dy : -dy;
  const float pitch_step = pitch_sign * stick_up * k;
  const bool absolute = pitch_sign < 0.0f;
  const bool has_mouse = dx != 0.0f || dy != 0.0f;

  if (!absolute || !REXCVAR_GET(dp_mouse_camera_catchup)) {
    // Delta family (or catch-up disabled): plain one-frame nudge (1.0 behaviour).
    if (has_mouse) {
      StoreF32(pitch_ptr, base_pitch + pitch_step);
      StoreF32(yaw_ptr, base_yaw + yaw_step);
    }
  } else {
    CatchUpState& s = g_catchup;
    const clock::time_point now = clock::now();
    const bool reset = s.tag != tag || !s.have_prev ||
                       (now - s.last_call) > std::chrono::milliseconds(250);
    if (reset) {
      s.pending_yaw = 0.0f;
      s.pending_pitch = 0.0f;
    } else {
      // What the camera actually did since last frame (spring, auto-centering,
      // our direct write): consume it from the pending angle.
      s.pending_yaw -= WrapAngle(base_yaw - s.prev_yaw);
      s.pending_pitch -= (base_pitch - s.prev_pitch);
    }
    s.pending_yaw += yaw_step;
    s.pending_pitch += pitch_step;
    if (has_mouse) {
      s.last_mouse = now;
    } else if ((now - s.last_mouse) > std::chrono::milliseconds(REXCVAR_GET(dp_mouse_camera_hold_ms))) {
      // Idle: release what is left so the game's own camera logic takes over.
      s.pending_yaw *= 0.5f;
      s.pending_pitch *= 0.5f;
      if (std::fabs(s.pending_yaw) < 1e-4f) s.pending_yaw = 0.0f;
      if (std::fabs(s.pending_pitch) < 1e-4f) s.pending_pitch = 0.0f;
    }
    s.pending_yaw = std::clamp(s.pending_yaw, -kMaxPendingYaw, kMaxPendingYaw);
    s.pending_pitch = std::clamp(s.pending_pitch, -kMaxPendingPitch, kMaxPendingPitch);
    s.prev_yaw = base_yaw;
    s.prev_pitch = base_pitch;
    s.have_prev = true;
    s.tag = tag;
    s.last_call = now;
    if (s.pending_yaw != 0.0f || s.pending_pitch != 0.0f) {
      StoreF32(yaw_ptr, WrapAngle(base_yaw + s.pending_yaw));
      StoreF32(pitch_ptr, base_pitch + s.pending_pitch);  // the routine clamps pitch right after
    }
  }

  // Direct: turn the yaw state (anchor+128) by this frame's mouse step so the
  // camera does not have to spring towards the target first. Only with an
  // anchor captured during this very routine call.
  if (absolute && has_mouse && REXCVAR_GET(dp_mouse_camera_direct) && g_anchor != 0 &&
      g_anchor_serial != g_anchor_serial_applied) {
    g_anchor_serial_applied = g_anchor_serial;
    uint8_t* anchor_yaw = memory->TranslateVirtual<uint8_t*>(g_anchor + 128);
    StoreF32(anchor_yaw, WrapAngle(LoadF32(anchor_yaw) + yaw_step));
  }
}

// Delta family (per-frame deltas, pitch += stick_y): PAL 0x82338CA8 (sub_82338A38),
// 0x8233A2D4 (sub_8233A150), 0x8233BE98 (sub_8233BE20).
void DPCameraMouseHook(PPCRegister& r31) { ApplyMouseToCamera(r31, +1.0f, "delta"); }

// Absolute family (targets reset every frame, pitch -= stick_y): PAL 0x8233B2A4
// (sub_8233AB28 walking camera, found by tracing on 2026-09-06), 0x82338818,
// 0x8233956C, 0x823399D0, 0x82339CC4, 0x82339F74 (sub_82339DF8 aim, left-stick
// driven). +112 = yaw, +108 = pitch (clamped by the game right after the stick
// Y step); the anchor object holds the yaw state at +128.
void DPCameraMouseHookWalk(PPCRegister& r31) { ApplyMouseToCamera(r31, -1.0f, "absolute"); }
