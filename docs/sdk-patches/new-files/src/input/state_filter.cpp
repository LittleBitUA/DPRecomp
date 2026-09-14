/**
 * @file        input/state_filter.cpp
 * @brief       Title-side filtering of the merged guest controller state.
 *
 * DP1 2026-09-14 (DPRecomp #19), see the header.
 */

#include <rex/input/state_filter.h>

#include <mutex>

#include <rex/input/device.h>

namespace rex::input {

namespace {

std::mutex g_mutex;
StateFilter g_filter;
uint16_t g_inject_buttons[kMaxGuestUsers] = {};
uint32_t g_inject_polls[kMaxGuestUsers] = {};

}  // namespace

void SetStateFilter(StateFilter filter) {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_filter = std::move(filter);
}

void InjectButtons(uint32_t user_index, uint16_t buttons, uint32_t polls) {
  if (user_index >= kMaxGuestUsers) {
    return;
  }
  std::lock_guard<std::mutex> lock(g_mutex);
  g_inject_buttons[user_index] = polls ? buttons : 0;
  g_inject_polls[user_index] = buttons ? polls : 0;
}

uint32_t PendingInjectedPolls(uint32_t user_index) {
  if (user_index >= kMaxGuestUsers) {
    return 0;
  }
  std::lock_guard<std::mutex> lock(g_mutex);
  return g_inject_polls[user_index];
}

void ApplyStateFilter(uint32_t user_index, bool synthetic, X_INPUT_GAMEPAD& gamepad) {
  if (user_index >= kMaxGuestUsers) {
    return;
  }
  StateFilter filter;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    filter = g_filter;  // copy: the filter runs without the lock held
  }
  if (filter) {
    filter(user_index, synthetic, gamepad);
  }
}

void ApplyInjectedButtons(uint32_t user_index, X_INPUT_GAMEPAD& gamepad) {
  if (user_index >= kMaxGuestUsers) {
    return;
  }
  std::lock_guard<std::mutex> lock(g_mutex);
  if (g_inject_polls[user_index]) {
    gamepad.buttons = static_cast<uint16_t>(static_cast<uint16_t>(gamepad.buttons) |
                                            g_inject_buttons[user_index]);
    if (--g_inject_polls[user_index] == 0) {
      g_inject_buttons[user_index] = 0;
    }
  }
}

}  // namespace rex::input
