// [new_fix_03102026_mouselook] Mouse camera rules that depend on the game's
// camera mode (DPRecomp #45). Pure functions, tested in tests/mouse_camera_test.cpp.
//
// Camera modes: the controller object (PAL 0x82928AD0, r31 in the camera
// hooks) keeps its mode at +324; the per-mode update routines sit in a table
// at PAL 0x82928C68 (17 entries): 1 = walking (sub_8233AB28), 2 = aim
// (sub_8233B3C0), 9 = car (sub_82338A38, run by the car code, sub_82355940),
// 10/11 = context target (sub_8233BE20), 16 = telescope (sub_8233C190).
// Pad layout read by these routines (sub_82522AD8): +192/+196 = left stick,
// +200/+204 = right stick, deadzone 0.25.
#pragma once

#include <algorithm>
#include <cstdint>

namespace dp {

inline constexpr uint32_t kCameraModeOffset = 324;  // controller + 324: current camera mode
inline constexpr float kRadiansPerDegree = 0.0174532924f;  // PAL 0x8200185C

// ---------------------------------------------------------------------------
// Car (mode 9, PAL sub_82338A38 / USA sub_823387E8). The view index sits at
// controller + 320: view 0 (the default chase camera, branch at PAL
// 0x82338FD8) and views 1-2 (branch from PAL 0x82338B30). Every frame the
// routine sets the yaw target (+112) to the car's heading and the pitch target
// (+108) to a resting value, then adds the right stick's deflection:
//   yaw   -= (|x| - 0.25) * sign * view[28] * 4/3 degrees
//   pitch += (|y| - 0.25) * sign * view[20] (up) or view[24] (down) * 4/3 degrees
// with `view` a 36-byte record of the game's car camera table. Full
// deflection is therefore view[28] / view[20] / view[24] degrees. The camera
// itself follows those targets smoothly (+48 yaw, +44 pitch).
//
// Before #45 the chase camera had no mouse hook at all: the mouse became the
// right stick, which is zero in every frame the mouse does not move, so the
// camera was pulled back behind the car at once ("wrenching forward"). The
// views 1-2 hook nudged the target for one frame, with the same result. Now
// the mouse holds an offset within the stick's reach while it moves and for
// dp_mouse_camera_car_hold_ms after; then the offset is dropped and the camera
// glides back the way it does when the stick is released.
inline constexpr uint32_t kCameraViewOffset = 320;  // controller + 320: car camera view
inline constexpr uint32_t kCarViewRecordSize = 36;
inline constexpr uint32_t kCarViewPitchUp = 20;     // degrees at full stick up
inline constexpr uint32_t kCarViewPitchDown = 24;   // degrees at full stick down
inline constexpr uint32_t kCarViewYaw = 28;         // degrees at full stick left/right

// The car view record the routine reads: views 1-2 index it as
// r27 + (view + r28) * 36 (r28 = 3 * the car's camera set, r27 = the table);
// view 0 has it in r28 already.
constexpr uint32_t CarViewRecord(uint32_t table, uint32_t set_index, uint32_t view) {
  return table + (view + set_index) * kCarViewRecordSize;
}

// New held angle (radians) for one axis. `step`: this frame's mouse step;
// `lo` / `hi`: the stick's reach on this axis; `holding`: the mouse moved
// within the hold time (this frame included). Not holding drops the offset.
inline float CarHeldAngle(float pending, float step, float lo, float hi, bool holding) {
  if (!holding) return 0.0f;
  return std::clamp(pending + step, lo, hi);
}

// ---------------------------------------------------------------------------
// Walking cameras (the "absolute" routines): every frame the routine resets
// the pitch target (+108) to the camera's resting pitch and subtracts the
// right stick's deflection, (|y| - 0.25) * scale degrees, then clamps:
//   mode 1 (sub_8233AB28, indoors):            scale 40 (PAL 0x82001E00), clamp -45..+60
//   modes 0, 7, 13, 14 (sub_823393A8, sub_823386C0, sub_82339688,
//   sub_82339AD0; mode 0 = walking outdoors):  scale 30 (PAL 0x82001310), clamp -30..+40
// The stick therefore reaches 30 / 22.5 degrees. The mouse's pending pitch was
// bounded at 0.9 rad (52 degrees) only, so pushing the mouse up drove the
// target onto the clamp, where the camera sinks into the ground and jitters
// against it (#45; traces 044 / 046, 03.10). The stick's reach is the limit now.
inline constexpr uint32_t kCameraModeWalk = 1;
inline constexpr float kWalkStickPitchReach = (1.0f - 0.25f) * 40.0f * kRadiansPerDegree;   // 0.5236 rad
inline constexpr float kOtherStickPitchReach = (1.0f - 0.25f) * 30.0f * kRadiansPerDegree;  // 0.3927 rad

// How far the mouse may hold the pitch away from the routine's own target in
// this camera mode; `fallback` for modes not measured.
inline float PendingPitchLimit(uint32_t mode, float fallback) {
  switch (mode) {
    case 1:
      return kWalkStickPitchReach;
    case 0:
    case 7:
    case 13:
    case 14:
      return kOtherStickPitchReach;
    default:
      return fallback;
  }
}

}  // namespace dp
