#pragma once
// deadlyprem - controller layouts (DP1 2026-09-14, DPRecomp #19).
//
// The Xbox 360 game aims with the right trigger, fires with A while aiming
// and holds breath / locks on with the left trigger. The Director's Cut
// (PS3/PC) moved that to the modern layout: left trigger aims, right trigger
// fires. This header is the pure remap, applied to each physical pad's state
// by the state filter installed in deadlyprem_hooks.cpp; it knows nothing
// about the runtime so a plain test executable can cover it
// (tests/pad_layout_test.cpp).
//
// "dc" layout, outside a vehicle (every guest input stays reachable):
//   physical LT              -> guest RT   (draw weapon / aim), always
//   physical RT, aiming      -> guest A    (fire)
//   physical RT, not aiming  -> guest LT   (hold breath / lock-on, trigger
//                                          QTEs: the two triggers are swapped)
//   physical A,  aiming      -> guest LT   (hold breath while aiming)
//   physical A,  not aiming  -> guest A    (interact / accept, menus)
// "Aiming" = physical LT past the game's own trigger threshold (the pad
// update of the game, PAL sub_82523238, turns a trigger into a button when
// its value is > 30). A press keeps the role it was given when it started
// until it is released, so lowering the weapon while a button is still held
// never produces a fresh press edge of a different guest button.
// In a vehicle the pad passes through untouched: the car drives on RT/LT and
// the DC changed those differently (its brake/boost sit on the shoulders).

#include <cstdint>
#include <cstring>

namespace dp {

enum class PadLayout { kOriginal, kDc };

constexpr uint16_t kPadButtonA = 0x1000;     // X_INPUT_GAMEPAD_A
constexpr uint8_t kPadTriggerThreshold = 30;  // game: trigger pressed when value > 30

// Per-pad latch state (one per guest user), zero-initialised.
struct PadLayoutState {
  enum Role : uint8_t { kNone = 0, kAsA, kAsLt };
  Role a_role = kNone;   // what physical A is currently delivering
  Role rt_role = kNone;  // what physical RT is currently delivering
};

inline bool PadTriggerPressed(uint8_t value) { return value > kPadTriggerThreshold; }

// Remaps one pad state in place. `buttons` are X_INPUT_GAMEPAD_BUTTON bits
// (host byte order), the triggers 0..255.
inline void ApplyPadLayout(PadLayout layout, uint16_t& buttons, uint8_t& left_trigger,
                           uint8_t& right_trigger, bool in_vehicle, PadLayoutState& state) {
  if (layout != PadLayout::kDc || in_vehicle) {
    state = PadLayoutState{};  // nothing is latched while the remap is off
    return;
  }
  const uint8_t phys_lt = left_trigger;
  const uint8_t phys_rt = right_trigger;
  const bool phys_a = (buttons & kPadButtonA) != 0;
  const bool aiming = PadTriggerPressed(phys_lt);

  // Roles are decided on the press and kept until the release.
  if (!phys_a) {
    state.a_role = PadLayoutState::kNone;
  } else if (state.a_role == PadLayoutState::kNone) {
    state.a_role = aiming ? PadLayoutState::kAsLt : PadLayoutState::kAsA;
  }
  if (!PadTriggerPressed(phys_rt)) {
    state.rt_role = PadLayoutState::kNone;
  } else if (state.rt_role == PadLayoutState::kNone) {
    state.rt_role = aiming ? PadLayoutState::kAsA : PadLayoutState::kAsLt;
  }

  right_trigger = phys_lt;
  left_trigger = 0;
  buttons = static_cast<uint16_t>(buttons & ~kPadButtonA);
  if (state.a_role == PadLayoutState::kAsA) buttons |= kPadButtonA;
  if (state.a_role == PadLayoutState::kAsLt) left_trigger = 255;
  if (state.rt_role == PadLayoutState::kAsA) buttons |= kPadButtonA;
  if (state.rt_role == PadLayoutState::kAsLt) left_trigger = phys_rt;
}

// Exact names only ("original", "dc"); anything else is the original layout
// and `recognised` (when given) says whether the name was valid.
inline PadLayout ParsePadLayout(const char* name, bool* recognised = nullptr) {
  const bool dc = name && std::strcmp(name, "dc") == 0;
  const bool original = name && std::strcmp(name, "original") == 0;
  if (recognised) *recognised = dc || original;
  return dc ? PadLayout::kDc : PadLayout::kOriginal;
}

}  // namespace dp
