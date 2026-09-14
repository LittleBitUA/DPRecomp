// deadlyprem - tests for the controller layout remap (DPRecomp #19).
// Plain executable, no framework: exit code 0 = pass. Build target
// dp_pad_layout_test (see CMakeLists.txt); run it after any change to
// src/deadlyprem_pad_layout.h.
#include <cstdint>
#include <cstdio>

#include "../src/deadlyprem_pad_layout.h"

namespace {

int g_failures = 0;

#define CHECK(cond)                                                              \
  do {                                                                           \
    if (!(cond)) {                                                               \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                \
      ++g_failures;                                                              \
    }                                                                            \
  } while (0)

struct Pad {
  uint16_t buttons = 0;
  uint8_t lt = 0;
  uint8_t rt = 0;
};

// One poll through a fresh latch state.
Pad Apply(dp::PadLayout layout, Pad in, bool in_vehicle = false) {
  dp::PadLayoutState state;
  dp::ApplyPadLayout(layout, in.buttons, in.lt, in.rt, in_vehicle, state);
  return in;
}

// One poll through a carried latch state (sequences).
Pad Poll(dp::PadLayoutState& state, Pad in, bool in_vehicle = false) {
  dp::ApplyPadLayout(dp::PadLayout::kDc, in.buttons, in.lt, in.rt, in_vehicle, state);
  return in;
}

constexpr uint16_t kA = dp::kPadButtonA;
constexpr uint16_t kB = 0x2000;
constexpr uint16_t kStart = 0x0010;
constexpr uint8_t kT = dp::kPadTriggerThreshold;

}  // namespace

int main() {
  // Original layout: everything passes through.
  {
    Pad p = Apply(dp::PadLayout::kOriginal, {kA | kB, 200, 100});
    CHECK(p.buttons == (kA | kB));
    CHECK(p.lt == 200);
    CHECK(p.rt == 100);
  }
  // DC: left trigger becomes the aim (guest RT), nothing else pressed.
  {
    Pad p = Apply(dp::PadLayout::kDc, {0, 200, 0});
    CHECK(p.rt == 200);
    CHECK(p.lt == 0);
    CHECK(p.buttons == 0);
  }
  // DC: right trigger alone (not aiming) = the original LT function (hold
  // breath / lock-on / trigger QTEs), analog value kept, no accidental A.
  {
    Pad p = Apply(dp::PadLayout::kDc, {0, 0, 255});
    CHECK(p.rt == 0);
    CHECK(p.lt == 255);
    CHECK(p.buttons == 0);
    Pad q = Apply(dp::PadLayout::kDc, {0, 0, 100});
    CHECK(q.lt == 100);
  }
  // DC: aim + right trigger = fire (guest A), and NOT hold breath.
  {
    Pad p = Apply(dp::PadLayout::kDc, {0, 255, 255});
    CHECK(p.rt == 255);
    CHECK(p.buttons == kA);
    CHECK(p.lt == 0);
  }
  // DC: aim + A = hold breath (guest LT), and A itself is NOT passed (no shot).
  {
    Pad p = Apply(dp::PadLayout::kDc, {kA, 255, 0});
    CHECK(p.rt == 255);
    CHECK(p.lt == 255);
    CHECK((p.buttons & kA) == 0);
  }
  // DC: aim + A + right trigger = fire while holding breath.
  {
    Pad p = Apply(dp::PadLayout::kDc, {kA, 255, 255});
    CHECK(p.rt == 255);
    CHECK(p.lt == 255);
    CHECK((p.buttons & kA) == kA);
  }
  // DC: A without aiming stays A (menus, interact), other buttons untouched.
  {
    Pad p = Apply(dp::PadLayout::kDc, {kA | kB | kStart, 0, 0});
    CHECK(p.buttons == (kA | kB | kStart));
    CHECK(p.lt == 0);
    CHECK(p.rt == 0);
  }
  // Threshold matches the game's pad update: a trigger counts as pressed
  // when its value is > 30. Exactly 30 is NOT aiming (A stays A, RT -> LT).
  {
    Pad p = Apply(dp::PadLayout::kDc, {kA, kT, 255});
    CHECK(p.rt == kT);
    CHECK(p.buttons == kA);
    CHECK(p.lt == 255);
    Pad q = Apply(dp::PadLayout::kDc, {kA, kT + 1, 255});
    CHECK(q.buttons == kA);  // fire
    CHECK(q.lt == 255);      // A = hold breath while aiming
    // RT at/below the threshold is not a press in any role.
    Pad r = Apply(dp::PadLayout::kDc, {0, 255, kT});
    CHECK(r.buttons == 0);
    CHECK(r.lt == 0);
  }
  // DC in a vehicle: pass-through (RT drives, LT brakes).
  {
    Pad p = Apply(dp::PadLayout::kDc, {kA, 40, 255}, true);
    CHECK(p.buttons == kA);
    CHECK(p.lt == 40);
    CHECK(p.rt == 255);
  }
  // Sequence: hold breath (A while aiming), release LT first, A still held.
  // The A press must NOT turn into a fresh guest-A press edge.
  {
    dp::PadLayoutState s;
    Pad p1 = Poll(s, {kA, 255, 0});
    CHECK(p1.lt == 255 && (p1.buttons & kA) == 0);
    Pad p2 = Poll(s, {kA, 0, 0});  // LT released, A still down
    CHECK((p2.buttons & kA) == 0);
    CHECK(p2.lt == 255);  // keeps its role until released
    Pad p3 = Poll(s, {0, 0, 0});   // A released
    CHECK(p3.buttons == 0 && p3.lt == 0);
    Pad p4 = Poll(s, {kA, 0, 0});  // a new A press outside aiming = A
    CHECK(p4.buttons == kA && p4.lt == 0);
  }
  // Sequence: fire (RT while aiming), release LT first, RT still held: no
  // fresh guest-LT press; RT keeps delivering A until released.
  {
    dp::PadLayoutState s;
    Pad p1 = Poll(s, {0, 255, 255});
    CHECK(p1.buttons == kA && p1.lt == 0);
    Pad p2 = Poll(s, {0, 0, 255});
    CHECK(p2.buttons == kA && p2.lt == 0);
    Pad p3 = Poll(s, {0, 0, 0});
    CHECK(p3.buttons == 0 && p3.lt == 0);
  }
  // Sequence: A pressed first (interact), then LT pulled: A keeps being A
  // (no surprise breath hold), and a new A press after that is breath.
  {
    dp::PadLayoutState s;
    Pad p1 = Poll(s, {kA, 0, 0});
    CHECK(p1.buttons == kA);
    Pad p2 = Poll(s, {kA, 255, 0});
    CHECK(p2.buttons == kA && p2.lt == 0);
    Poll(s, {0, 255, 0});
    Pad p4 = Poll(s, {kA, 255, 0});
    CHECK((p4.buttons & kA) == 0 && p4.lt == 255);
  }
  // Sequence: RT held as breath (not aiming), then LT pulled: RT keeps the
  // LT role (no shot fired by pulling the aim trigger).
  {
    dp::PadLayoutState s;
    Pad p1 = Poll(s, {0, 0, 255});
    CHECK(p1.lt == 255 && p1.buttons == 0);
    Pad p2 = Poll(s, {0, 255, 255});
    CHECK(p2.lt == 255 && p2.buttons == 0 && p2.rt == 255);
  }
  // Entering a vehicle with something latched clears the latch.
  {
    dp::PadLayoutState s;
    Poll(s, {kA, 255, 0});
    CHECK(s.a_role == dp::PadLayoutState::kAsLt);
    Poll(s, {kA, 255, 0}, true);
    CHECK(s.a_role == dp::PadLayoutState::kNone);
  }
  // Parsing: exact names only.
  {
    bool ok = false;
    CHECK(dp::ParsePadLayout("original", &ok) == dp::PadLayout::kOriginal && ok);
    CHECK(dp::ParsePadLayout("dc", &ok) == dp::PadLayout::kDc && ok);
    CHECK(dp::ParsePadLayout("", &ok) == dp::PadLayout::kOriginal && !ok);
    CHECK(dp::ParsePadLayout(nullptr, &ok) == dp::PadLayout::kOriginal && !ok);
    CHECK(dp::ParsePadLayout("DC", &ok) == dp::PadLayout::kOriginal && !ok);
    CHECK(dp::ParsePadLayout("default", &ok) == dp::PadLayout::kOriginal && !ok);
    CHECK(dp::ParsePadLayout("disabled", &ok) == dp::PadLayout::kOriginal && !ok);
    CHECK(dp::ParsePadLayout("directors_cut", &ok) == dp::PadLayout::kOriginal && !ok);
  }

  if (g_failures) {
    std::printf("%d failure(s)\n", g_failures);
    return 1;
  }
  std::printf("pad_layout_test: all checks passed\n");
  return 0;
}
