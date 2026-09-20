// Output channel layout policy (header-only, no device state; unit-tested).
//
// DPRecomp #19 (2026-09-20): a Windows endpoint configured as 5.1 with stereo
// speakers or headphones behind it receives six discrete channels, the
// dialogue lives in the center channel, and nothing plays it. The user gets
// to overrule the endpoint: "stereo" always runs the runtime's own 5.1 -> 2.0
// fold, "surround" always submits the guest's six channels, "auto" keeps the
// old behaviour (follow what the endpoint reports).
#pragma once

#include <cstdint>
#include <string_view>

namespace rex::audio {

enum class OutputLayout { kAuto, kStereo, kSurround };

/// Parses the `audio_channels` cvar. Case-insensitive; anything unknown is
/// treated as auto so a typo in the toml can never silence the game.
inline OutputLayout ParseOutputLayout(std::string_view value) {
  auto eq = [&](std::string_view word) {
    if (value.size() != word.size()) return false;
    for (size_t i = 0; i < word.size(); ++i) {
      char c = value[i];
      if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
      if (c != word[i]) return false;
    }
    return true;
  };
  if (eq("stereo") || eq("2") || eq("2.0")) return OutputLayout::kStereo;
  if (eq("surround") || eq("6") || eq("5.1")) return OutputLayout::kSurround;
  return OutputLayout::kAuto;
}

/// How many channels the driver submits to the device stream, given the
/// user's choice and what the endpoint reports (0 = unknown). The guest always
/// renders six; the only other answer is two, which selects the stereo fold.
inline uint8_t ResolveOutputChannels(OutputLayout layout, int endpoint_channels) {
  switch (layout) {
    case OutputLayout::kStereo:
      return 2;
    case OutputLayout::kSurround:
      return 6;
    case OutputLayout::kAuto:
    default:
      // An unknown endpoint keeps the guest layout, the device converts.
      return (endpoint_channels > 0 && endpoint_channels <= 2) ? 2 : 6;
  }
}

}  // namespace rex::audio
