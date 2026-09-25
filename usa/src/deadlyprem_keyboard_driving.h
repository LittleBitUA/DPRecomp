#pragma once
// deadlyprem - keyboard driving ([new_fix_25092026_glitch], user report 24.09).
//
// In the car the game drives on the pad's triggers: RT = accelerate, LT =
// brake / reverse, the left stick steers. The keyboard layer binds W/S to the
// left stick's Y and the triggers to Space / Ctrl, so on a keyboard:
//   1. the car reversed on Ctrl, not S;
//   2. W did nothing without Space held (W is stick-up, which the car ignores);
//   3. the mouse steered: holding RT (Space, the gas) is the keyboard layer's
//      "aiming" state, in which mouse motion goes to the LEFT stick.
// While York is driving (the game's player-driven car update ran within
// dp_pad_layout_vehicle_ms, see DPInVehicle) the keyboard/mouse device's stick
// Y becomes the triggers instead: up = accelerate, down = brake / reverse, at
// the same strength (the keyboard ramp still applies). With W as the gas nobody
// needs to hold Space, so the mouse stays on the camera. Physical pads are
// never touched.
//
// While the device's own RT is held (Space) nothing changes, exactly as before
// this fix: in that state the keyboard layer adds mouse motion to the LEFT
// stick, with a per-axis minimum deflection, so its Y is mouse drift, not S. 1
// pixel of downward drift would otherwise have braked the car (LT ~70) under
// full gas (critic finding, 25.09). Ctrl (LT) adds no mouse motion to the
// stick, so W + Ctrl still maps.
//
// Pure function, no runtime access: tests/keyboard_driving_test.cpp.

#include <algorithm>
#include <cstdint>

namespace dp {

// thumb_ly: stick Y (+ = up), triggers 0..255, all as the keyboard/mouse device
// reported them. Returns true when it changed anything.
inline bool ApplyKeyboardDriving(bool in_vehicle, int16_t& thumb_ly, uint8_t& left_trigger, uint8_t& right_trigger) {
  if (!in_vehicle || thumb_ly == 0) return false;
  if (right_trigger != 0) return false;  // Space held: stick Y carries mouse motion
  const int32_t v = thumb_ly;
  const int32_t mag = v > 0 ? v : -v;  // |INT16_MIN| = 32768 clamps to 255 below
  const uint8_t level = static_cast<uint8_t>(std::min<int32_t>(255, (mag * 255 + 16383) / 32767));
  if (v > 0) {
    right_trigger = std::max(right_trigger, level);
  } else {
    left_trigger = std::max(left_trigger, level);
  }
  thumb_ly = 0;
  return true;
}

}  // namespace dp
