/**
 * @file        rex/input/guest_input_gate.h
 *
 * @brief       [new_fix_28092026_upstream] Per-user connection tracking and
 *              the button mask used while a system dialog owns the input.
 *
 * Port of the parts of upstream 3cd7243 (2026-09-01) that DP needs:
 *  - XN_SYS_INPUTDEVICESCHANGED when a user's pad connects or disconnects, so a
 *    pad plugged in (or woken up) mid-game is noticed. Unlike upstream, the
 *    first observation of a user is only a baseline: at boot the pad is simply
 *    there, as on a console, and no notification is sent.
 *  - While a XAM dialog is on screen the guest reads a neutral pad; buttons
 *    still held when it closes stay masked until released, so the press that
 *    dismissed the dialog does not also reach the game.
 * Pure state, no runtime dependencies, unit-tested.
 */
#pragma once

#include <array>
#include <cstdint>

#include <rex/input/device.h>

namespace rex::input {

class GuestInputGate {
 public:
  enum class Change { kNone, kConnected, kDisconnected };

  // Records whether a user currently has a device; returns the change to
  // announce. The first observation per user returns kNone (baseline).
  Change Observe(uint32_t user_index, bool connected) {
    if (user_index >= kMaxGuestUsers) {
      return Change::kNone;
    }
    const int8_t now = connected ? 1 : 0;
    const int8_t before = state_[user_index];
    state_[user_index] = now;
    if (!connected) {
      consumed_[user_index] = 0;
    }
    if (before < 0 || before == now) {
      return Change::kNone;
    }
    return connected ? Change::kConnected : Change::kDisconnected;
  }

  // Buttons held when a dialog closes: masked until each one is released.
  void Consume(uint32_t user_index, uint16_t held_buttons) {
    if (user_index < kMaxGuestUsers) {
      consumed_[user_index] |= held_buttons;
    }
  }

  // Applies the mask to what the guest is about to read.
  uint16_t Mask(uint32_t user_index, uint16_t buttons) {
    if (user_index >= kMaxGuestUsers) {
      return buttons;
    }
    consumed_[user_index] &= buttons;  // a released button leaves the mask
    return static_cast<uint16_t>(buttons & ~consumed_[user_index]);
  }

  uint16_t consumed(uint32_t user_index) const {
    return user_index < kMaxGuestUsers ? consumed_[user_index] : 0;
  }

 private:
  std::array<int8_t, kMaxGuestUsers> state_ = {-1, -1, -1, -1};
  std::array<uint16_t, kMaxGuestUsers> consumed_ = {};
};

}  // namespace rex::input
