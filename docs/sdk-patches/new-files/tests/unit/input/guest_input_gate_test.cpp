// [new_fix_28092026_upstream] Connection tracking and dialog button mask.
#include <catch2/catch_test_macros.hpp>

#include <rex/input/guest_input_gate.h>

using rex::input::GuestInputGate;
using Change = GuestInputGate::Change;

TEST_CASE("guest_input_gate: first observation is a silent baseline", "[guest_input_gate]") {
  GuestInputGate gate;
  CHECK(gate.Observe(0, true) == Change::kNone);   // pad present at boot: no notification
  CHECK(gate.Observe(1, false) == Change::kNone);  // empty slot at boot: none either
  CHECK(gate.Observe(0, true) == Change::kNone);
  CHECK(gate.Observe(1, false) == Change::kNone);
}

TEST_CASE("guest_input_gate: real changes are reported once", "[guest_input_gate]") {
  GuestInputGate gate;
  gate.Observe(0, true);
  CHECK(gate.Observe(0, false) == Change::kDisconnected);
  CHECK(gate.Observe(0, false) == Change::kNone);
  CHECK(gate.Observe(0, true) == Change::kConnected);
  CHECK(gate.Observe(0, true) == Change::kNone);
  gate.Observe(2, false);
  CHECK(gate.Observe(2, true) == Change::kConnected);  // a second pad plugged in mid-game
  CHECK(gate.Observe(7, true) == Change::kNone);       // out of range
}

TEST_CASE("guest_input_gate: the dismissing press does not reach the game", "[guest_input_gate]") {
  constexpr uint16_t kA = 0x1000, kB = 0x2000, kUp = 0x0001;
  GuestInputGate gate;
  gate.Observe(0, true);
  CHECK(gate.Mask(0, kA | kUp) == (kA | kUp));  // nothing consumed yet

  gate.Consume(0, kA);                  // dialog closed while A was held
  CHECK(gate.Mask(0, kA | kUp) == kUp); // A hidden, other input passes
  CHECK(gate.Mask(0, kA | kB) == kB);   // still held: still hidden; a new button passes
  CHECK(gate.Mask(0, 0) == 0);          // released...
  CHECK(gate.consumed(0) == 0);
  CHECK(gate.Mask(0, kA) == kA);        // ...so the next press is the game's
}

TEST_CASE("guest_input_gate: disconnect clears the mask", "[guest_input_gate]") {
  GuestInputGate gate;
  gate.Observe(0, true);
  gate.Consume(0, 0x1000);
  gate.Observe(0, false);
  CHECK(gate.consumed(0) == 0);
  gate.Consume(9, 0xFFFF);  // out of range is ignored
  CHECK(gate.Mask(9, 0x1234) == 0x1234);
}
