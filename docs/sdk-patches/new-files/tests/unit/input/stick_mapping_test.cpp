// DPRecomp #30 (2026-09-19): mouse -> stick mapping, vector vs per-axis.
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstdlib>

#include <rex/input/mnk/stick_mapping.h>

using rex::input::mnk::MapMouseToStick;
using rex::input::mnk::StickDeflection;

namespace {
constexpr double kFloor = 8689.0;   // the game's inner right-stick deadzone
constexpr double kGain = 300.0;     // mnk_sensitivity 1.0 * mnk_stick_scale 300
constexpr int32_t kDeadzone = 8689; // what a per-axis reader ignores below
}  // namespace

TEST_CASE("stick mapping: no motion is no deflection", "[input][mnk]") {
  const StickDeflection v = MapMouseToStick(0.0, 0.0, kFloor, kGain, false);
  CHECK(v.x == 0);
  CHECK(v.y == 0);
  const StickDeflection p = MapMouseToStick(0.0, 0.0, kFloor, kGain, true);
  CHECK(p.x == 0);
  CHECK(p.y == 0);
}

TEST_CASE("stick mapping: vector mode keeps the direction and floors the length", "[input][mnk]") {
  const StickDeflection v = MapMouseToStick(3.0, 4.0, kFloor, kGain, false);
  // length = 8689 + 5 * 300 = 10189, direction (0.6, 0.8)
  CHECK(v.x == std::lround(0.6 * 10189.0));
  CHECK(v.y == std::lround(0.8 * 10189.0));
  const StickDeflection n = MapMouseToStick(-3.0, -4.0, kFloor, kGain, false);
  CHECK(n.x == -v.x);
  CHECK(n.y == -v.y);
}

TEST_CASE("stick mapping: the #30 staircase - a vector diagonal clears a per-axis deadzone on one axis only",
          "[input][mnk]") {
  // A small diagonal move: 1 px right, 1 px up.
  const StickDeflection v = MapMouseToStick(1.0, 1.0, kFloor, kGain, false);
  // length 8689 + 424 = 9113 -> each axis 0.707 * 9113 = 6444 < 8689: BOTH
  // axes below a per-axis deadzone; a 2:1 move clears only the larger one.
  CHECK(std::abs(v.x) < kDeadzone);
  CHECK(std::abs(v.y) < kDeadzone);
  const StickDeflection w = MapMouseToStick(2.0, 1.0, kFloor, kGain, false);
  CHECK(std::abs(w.x) < kDeadzone);
  CHECK(std::abs(w.y) < kDeadzone);
  const StickDeflection u = MapMouseToStick(6.0, 3.0, kFloor, kGain, false);
  CHECK(std::abs(u.x) >= kDeadzone);
  CHECK(std::abs(u.y) < kDeadzone);  // the staircase: x moves, y does not
}

TEST_CASE("stick mapping: per-axis mode clears the deadzone on every axis that moved", "[input][mnk]") {
  const StickDeflection p = MapMouseToStick(1.0, 1.0, kFloor, kGain, true);
  CHECK(p.x == 8689 + 300);
  CHECK(p.y == 8689 + 300);
  const StickDeflection q = MapMouseToStick(6.0, -3.0, kFloor, kGain, true);
  CHECK(q.x == 8689 + 1800);
  CHECK(q.y == -(8689 + 900));
  CHECK(std::abs(q.x) >= kDeadzone);
  CHECK(std::abs(q.y) >= kDeadzone);
  // Motion on one axis only leaves the other axis at rest.
  const StickDeflection h = MapMouseToStick(4.0, 0.0, kFloor, kGain, true);
  CHECK(h.x == 8689 + 1200);
  CHECK(h.y == 0);
}

TEST_CASE("stick mapping: both modes clamp to the stick range", "[input][mnk]") {
  const StickDeflection v = MapMouseToStick(300.0, 400.0, kFloor, kGain, false);
  CHECK(std::abs(v.x) <= 32767);
  CHECK(std::abs(v.y) <= 32767);
  CHECK(std::lround(std::hypot(double(v.x), double(v.y))) == 32767);
  const StickDeflection p = MapMouseToStick(-300.0, 400.0, kFloor, kGain, true);
  CHECK(p.x == -32767);
  CHECK(p.y == 32767);
  // A zero floor is plain gain.
  const StickDeflection z = MapMouseToStick(2.0, 0.0, 0.0, kGain, true);
  CHECK(z.x == 600);
}
