// DPRecomp #19 (2026-09-20): audio_channels policy (auto / stereo / surround).
#include <catch2/catch_test_macros.hpp>

#include <rex/audio/output_layout.h>

using rex::audio::OutputLayout;
using rex::audio::ParseOutputLayout;
using rex::audio::ResolveOutputChannels;

TEST_CASE("output layout: parse is case-insensitive and tolerant", "[audio]") {
  CHECK(ParseOutputLayout("auto") == OutputLayout::kAuto);
  CHECK(ParseOutputLayout("stereo") == OutputLayout::kStereo);
  CHECK(ParseOutputLayout("Stereo") == OutputLayout::kStereo);
  CHECK(ParseOutputLayout("2.0") == OutputLayout::kStereo);
  CHECK(ParseOutputLayout("surround") == OutputLayout::kSurround);
  CHECK(ParseOutputLayout("SURROUND") == OutputLayout::kSurround);
  CHECK(ParseOutputLayout("5.1") == OutputLayout::kSurround);
  // a typo in the toml must never change the old behaviour
  CHECK(ParseOutputLayout("") == OutputLayout::kAuto);
  CHECK(ParseOutputLayout("stereophonic") == OutputLayout::kAuto);
  CHECK(ParseOutputLayout("7.1") == OutputLayout::kAuto);
}

TEST_CASE("output layout: auto follows the endpoint (the 1.4.3 behaviour)", "[audio]") {
  CHECK(ResolveOutputChannels(OutputLayout::kAuto, 2) == 2);
  CHECK(ResolveOutputChannels(OutputLayout::kAuto, 1) == 2);   // mono device: fold, then SDL collapses
  CHECK(ResolveOutputChannels(OutputLayout::kAuto, 6) == 6);
  CHECK(ResolveOutputChannels(OutputLayout::kAuto, 8) == 6);   // 7.1: pass the guest's six through
  CHECK(ResolveOutputChannels(OutputLayout::kAuto, 0) == 6);   // unknown: keep the guest layout
}

TEST_CASE("output layout: stereo overrules a 5.1 endpoint (#19: center channel with no speaker)", "[audio]") {
  CHECK(ResolveOutputChannels(OutputLayout::kStereo, 6) == 2);
  CHECK(ResolveOutputChannels(OutputLayout::kStereo, 8) == 2);
  CHECK(ResolveOutputChannels(OutputLayout::kStereo, 2) == 2);
  CHECK(ResolveOutputChannels(OutputLayout::kStereo, 0) == 2);
}

TEST_CASE("output layout: surround always submits the guest's six channels", "[audio]") {
  CHECK(ResolveOutputChannels(OutputLayout::kSurround, 2) == 6);
  CHECK(ResolveOutputChannels(OutputLayout::kSurround, 6) == 6);
  CHECK(ResolveOutputChannels(OutputLayout::kSurround, 0) == 6);
}
