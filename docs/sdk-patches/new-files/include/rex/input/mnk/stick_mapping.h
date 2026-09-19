// Mouse motion -> stick deflection (header-only, no driver state; unit-tested).
//
// DP1 rev. 2 (2026-09-06): VECTOR mapping. The game's inner deadzone (floor)
// is added to the LENGTH of the motion vector, so the direction of the mouse
// motion is preserved exactly and only the magnitude is remapped.
//
// DPRecomp #30 (2026-09-19): PER-AXIS mapping for aim mode. The aim routine
// reads the left stick axis by axis, each with its own deadzone; a vector
// mapped diagonal clears it on one axis at a time and staircases. Adding the
// floor to every axis that moved makes both axes register in the same poll.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace rex {
namespace input {
namespace mnk {

struct StickDeflection {
  int32_t x = 0;
  int32_t y = 0;
};

/// dx/dy: mouse motion this poll (stick convention: +y = up). floor: the
/// deadzone floor in stick units, gain: stick units per pixel.
inline StickDeflection MapMouseToStick(double dx, double dy, double floor, double gain,
                                       bool per_axis) {
  constexpr double kMax = 32767.0;
  StickDeflection out;
  floor = std::clamp(floor, 0.0, kMax);
  if (per_axis) {
    auto axis = [&](double d) -> int32_t {
      if (d == 0.0) return 0;
      const double v = std::min(floor + std::abs(d) * gain, kMax);
      return static_cast<int32_t>(std::lround(d < 0.0 ? -v : v));
    };
    out.x = axis(dx);
    out.y = axis(dy);
    return out;
  }
  const double len = std::sqrt(dx * dx + dy * dy);
  if (len <= 0.0) return out;
  const double out_len = std::min(floor + len * gain, kMax);
  out.x = static_cast<int32_t>(std::lround(dx / len * out_len));
  out.y = static_cast<int32_t>(std::lround(dy / len * out_len));
  return out;
}

}  // namespace mnk
}  // namespace input
}  // namespace rex
