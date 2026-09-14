#pragma once
/**
 * @file        input/state_filter.h
 * @brief       Title-side per-device pad filter and synthetic button presses.
 *
 * DP1 2026-09-14 (DPRecomp #19). Two small things a recomp project needs
 * between the drivers and the guest that neither driver can provide:
 *
 *   * a state filter: one callback the title installs to remap each physical
 *     pad (an alternative button layout, context-sensitive swaps that need
 *     game state the drivers know nothing about). It runs per device, before
 *     the devices of a user are merged, and is told whether the device is
 *     synthetic (keyboard/mouse emulation), whose bindings already speak the
 *     game's own layout;
 *   * button injection: a synthetic press of a button mask for the next N
 *     polls (skipping a "press start" screen, scripted smoke tests). The
 *     press is held for the requested number of polls and then released, so
 *     the title sees exactly one press edge.
 *
 * Order inside InputSystem::GetState: the filter runs on each device's state
 * (so it never sees another device's buttons nor an injected press), the
 * devices of the user are merged, then the injection is OR-ed into the merged
 * state. One filter slot: SetStateFilter replaces the previous filter (there
 * is no chain), and InjectButtons replaces a pending press for that user
 * rather than adding to it. Everything here is thread-safe and free of guest
 * memory access so it can be unit-tested on its own.
 */

#include <cstdint>
#include <functional>

#include <rex/input/input.h>

namespace rex::input {

/// Called on the state of one device of a guest user before the devices are
/// merged; `synthetic` is true for keyboard/mouse emulation and stand-ins.
using StateFilter =
    std::function<void(uint32_t user_index, bool synthetic, X_INPUT_GAMEPAD& gamepad)>;

/// Installs (or, with an empty function, removes) the state filter.
void SetStateFilter(StateFilter filter);

/// Holds `buttons` (X_INPUT_GAMEPAD_BUTTON bits) down on `user_index` for the
/// next `polls` calls of ApplyInjectedButtons, replacing any pending press of
/// that user; 0 polls or an empty mask cancels the pending press.
void InjectButtons(uint32_t user_index, uint16_t buttons, uint32_t polls);

/// Polls left on a pending injection for that user (0 = none).
uint32_t PendingInjectedPolls(uint32_t user_index);

/// Runs the filter on one device's state (no-op without a filter).
void ApplyStateFilter(uint32_t user_index, bool synthetic, X_INPUT_GAMEPAD& gamepad);

/// Applies a pending injection to the merged state of a user. Both are
/// called by the input system and exposed so tests can drive them without
/// devices.
void ApplyInjectedButtons(uint32_t user_index, X_INPUT_GAMEPAD& gamepad);

}  // namespace rex::input
