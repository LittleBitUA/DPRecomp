// DP1 2026-09-14 (DPRecomp #19): title-side per-device state filter and
// button injection on the merged state.
#include <catch2/catch_test_macros.hpp>

#include <cstdint>

#include <rex/input/input.h>
#include <rex/input/state_filter.h>

using rex::input::ApplyInjectedButtons;
using rex::input::ApplyStateFilter;
using rex::input::InjectButtons;
using rex::input::PendingInjectedPolls;
using rex::input::SetStateFilter;
using rex::input::X_INPUT_GAMEPAD;
using rex::input::X_INPUT_GAMEPAD_A;
using rex::input::X_INPUT_GAMEPAD_START;

namespace {

struct Reset {
  Reset() { SetStateFilter({}); for (uint32_t u = 0; u < 4; ++u) InjectButtons(u, 0, 0); }
  ~Reset() { SetStateFilter({}); for (uint32_t u = 0; u < 4; ++u) InjectButtons(u, 0, 0); }
};

uint16_t Buttons(const X_INPUT_GAMEPAD& g) { return static_cast<uint16_t>(g.buttons); }

}  // namespace

TEST_CASE("state filter: nothing installed leaves the pad alone", "[input][filter]") {
  Reset reset;
  X_INPUT_GAMEPAD g = {};
  g.buttons = static_cast<uint16_t>(X_INPUT_GAMEPAD_A);
  g.left_trigger = 12;
  ApplyStateFilter(0, false, g);
  ApplyInjectedButtons(0, g);
  CHECK(Buttons(g) == X_INPUT_GAMEPAD_A);
  CHECK(g.left_trigger == 12);
}

TEST_CASE("state filter: the filter sees the user index and the synthetic flag and may rewrite the pad",
          "[input][filter]") {
  Reset reset;
  uint32_t seen_user = 99;
  bool seen_synthetic = false;
  SetStateFilter([&](uint32_t user, bool synthetic, X_INPUT_GAMEPAD& pad) {
    seen_user = user;
    seen_synthetic = synthetic;
    if (synthetic) return;  // what a layout filter does with keyboard/mouse emulation
    pad.right_trigger = pad.left_trigger;
    pad.left_trigger = 0;
  });
  X_INPUT_GAMEPAD g = {};
  g.left_trigger = 200;
  ApplyStateFilter(2, false, g);
  CHECK(seen_user == 2);
  CHECK_FALSE(seen_synthetic);
  CHECK(g.right_trigger == 200);
  CHECK(g.left_trigger == 0);

  X_INPUT_GAMEPAD k = {};
  k.left_trigger = 200;
  ApplyStateFilter(2, true, k);
  CHECK(seen_synthetic);
  CHECK(k.left_trigger == 200);  // synthetic device untouched by that filter

  SetStateFilter({});
  g.left_trigger = 50;
  ApplyStateFilter(2, false, g);
  CHECK(g.left_trigger == 50);  // removed
}

TEST_CASE("button injection: held for N polls, then released exactly once", "[input][filter]") {
  Reset reset;
  InjectButtons(0, static_cast<uint16_t>(X_INPUT_GAMEPAD_START), 3);
  CHECK(PendingInjectedPolls(0) == 3);
  for (int i = 0; i < 3; ++i) {
    X_INPUT_GAMEPAD g = {};
    ApplyInjectedButtons(0, g);
    CHECK((Buttons(g) & X_INPUT_GAMEPAD_START) == X_INPUT_GAMEPAD_START);
  }
  CHECK(PendingInjectedPolls(0) == 0);
  X_INPUT_GAMEPAD g = {};
  ApplyInjectedButtons(0, g);
  CHECK(Buttons(g) == 0);  // released, not sticky
}

TEST_CASE("button injection: ORs into what the devices report and is per user", "[input][filter]") {
  Reset reset;
  InjectButtons(1, static_cast<uint16_t>(X_INPUT_GAMEPAD_START), 1);
  X_INPUT_GAMEPAD other = {};
  ApplyInjectedButtons(0, other);
  CHECK(Buttons(other) == 0);  // user 0 untouched
  CHECK(PendingInjectedPolls(1) == 1);
  X_INPUT_GAMEPAD g = {};
  g.buttons = static_cast<uint16_t>(X_INPUT_GAMEPAD_A);
  ApplyInjectedButtons(1, g);
  CHECK(Buttons(g) == (X_INPUT_GAMEPAD_A | X_INPUT_GAMEPAD_START));
}

TEST_CASE("button injection: zero polls or an empty mask cancels, a new press replaces, bad user index is ignored",
          "[input][filter]") {
  Reset reset;
  InjectButtons(0, static_cast<uint16_t>(X_INPUT_GAMEPAD_START), 5);
  InjectButtons(0, static_cast<uint16_t>(X_INPUT_GAMEPAD_START), 0);
  CHECK(PendingInjectedPolls(0) == 0);
  InjectButtons(0, static_cast<uint16_t>(X_INPUT_GAMEPAD_START), 5);
  InjectButtons(0, 0, 5);
  CHECK(PendingInjectedPolls(0) == 0);
  // replace, not accumulate
  InjectButtons(0, static_cast<uint16_t>(X_INPUT_GAMEPAD_START), 5);
  InjectButtons(0, static_cast<uint16_t>(X_INPUT_GAMEPAD_A), 2);
  CHECK(PendingInjectedPolls(0) == 2);
  X_INPUT_GAMEPAD r = {};
  ApplyInjectedButtons(0, r);
  CHECK(Buttons(r) == X_INPUT_GAMEPAD_A);
  InjectButtons(0, 0, 0);
  // bad user index: no press, and an installed filter is not invoked either
  bool filter_called = false;
  SetStateFilter([&](uint32_t, bool, X_INPUT_GAMEPAD&) { filter_called = true; });
  InjectButtons(7, static_cast<uint16_t>(X_INPUT_GAMEPAD_START), 5);
  CHECK(PendingInjectedPolls(7) == 0);
  X_INPUT_GAMEPAD g = {};
  ApplyInjectedButtons(7, g);
  ApplyStateFilter(7, false, g);
  CHECK(Buttons(g) == 0);
  CHECK_FALSE(filter_called);
}

TEST_CASE("button injection: the filter never sees injected buttons (it runs per device, before the merge)",
          "[input][filter]") {
  Reset reset;
  uint16_t seen = 0xFFFF;
  SetStateFilter([&](uint32_t, bool, X_INPUT_GAMEPAD& pad) { seen = static_cast<uint16_t>(pad.buttons); });
  InjectButtons(0, static_cast<uint16_t>(X_INPUT_GAMEPAD_START), 1);
  X_INPUT_GAMEPAD g = {};
  ApplyStateFilter(0, false, g);
  ApplyInjectedButtons(0, g);
  CHECK(seen == 0);
  CHECK(Buttons(g) == X_INPUT_GAMEPAD_START);
}
