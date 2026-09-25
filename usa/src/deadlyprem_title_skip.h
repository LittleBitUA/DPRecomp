#pragma once
// deadlyprem - intro skip decision logic (DP1 2026-09-14, DPRecomp #19).
//
// Pure state machine driven by the title mode hook in deadlyprem_hooks.cpp:
// the hook reads the game's title state block and passes the numbers in,
// this header decides what to do, the hook does it. No runtime access here,
// so tests/title_skip_test.cpp can walk every path.
//
// The title mode of the game (PAL sub_8241C840 / USA sub_8241BFA0) is a state
// machine; measured boot sequence (logs 144-157): 80 (loading, sub-step 18 =
// layouts ready) -> 81..88 (four publisher logos) -> 97..100 (title
// background fade-in) -> 0 (press start) -> [Start] 106 -> 1 -> 2 (main
// menu). Logo 4 (state 88) ends by setting state = next = 97 and its timer to
// 0; "skip logos" does exactly that on the first update frame of state 80
// (state 97 waits for the layouts itself). "menu" then pulses a synthetic Start on the press
// start screen until the machine leaves state 0: the screen ignores the pad
// while its background movie starts (~50 frames), and the game's pad system
// polls each user twice per frame there, so the press is 2 polls every 6
// frames and never held longer (a held press has no press edge in the
// per-pad poll, which is what picks the active controller).
//
// [new_fix_25092026_glitch] State 99 plays the title's opening movie: the
// xmedia WMV threads start on entering it and exit when the movie ends, and
// only then does the machine move on to 100 and 0. Measured 25.09: 343-348 s
// of movie with both skip levels (and 265 s in a player's log), so "skip the
// intro" still sat through it. A player skips it with Start (probe: one press
// three seconds in, 99 -> 100 -> 0 at once); both logos and menu now pulse
// Start while the machine is in 99, and stop as soon as it leaves.

#include <cstdint>

namespace dp {

enum class SkipIntro : uint8_t { kOff = 0, kLogos = 1, kMenu = 2 };

constexpr uint32_t kTitlePhaseInit = 0;
constexpr uint32_t kTitlePhaseUpdate = 1;
constexpr uint32_t kTitleStateBoot = 80;         // start state when the game boots
constexpr uint32_t kTitleStateTitleFadeIn = 97;  // what logo 4 hands over to
constexpr uint32_t kTitleStatePressStart = 0;
constexpr uint32_t kTitleStateStartAccepted = 106;
constexpr uint32_t kTitleLoadStepDone = 18;
constexpr uint32_t kTitleStartPulsePeriod = 6;    // frames between synthetic presses
constexpr uint32_t kTitleStartPulsePolls = 2;     // polls per press (two polls per frame)
constexpr uint32_t kTitleStartPulseMaxFrames = 1800;  // 30 s, then give up
constexpr uint32_t kTitleStateOpeningMovie = 99;      // [new_fix_25092026_glitch]
constexpr uint32_t kTitleMoviePulseMaxFrames = 900;   // [new_fix_25092026_glitch] 15 s, then give up

struct TitleSkipState {
  uint32_t start_state = 0xFFFFFFFFu;  // *(params) recorded at phase 0
  bool block_ok = false;               // block address validated this entry
  bool warned_block = false;           // one warning per title entry
  bool logos_skipped = false;
  bool start_pulsing = false;          // on the press start screen, pressing
  bool start_done = false;             // left state 0 once; never press again this entry
  uint32_t pulse_frames = 0;
  // [new_fix_25092026_glitch] the opening movie (state 99)
  bool movie_pulsing = false;
  bool movie_done = false;             // left 99 once (or gave up); never press there again
  uint32_t movie_frames = 0;
};

struct TitleSkipAction {
  bool jump_to_title = false;      // write state = next = 97, timer = 0
  bool inject_start = false;       // InjectButtons(START, kTitleStartPulsePolls)
  bool cancel_injection = false;   // InjectButtons(0, 0)
  bool log_started_pulsing = false;
  bool log_start_left = false;     // state left 0: `left_state` says where to
  bool log_block_mismatch = false;
  bool log_gave_up = false;        // pulse cap reached
  uint32_t left_state = 0;
  uint32_t pulse_frames = 0;
  // [new_fix_25092026_glitch] the opening movie (state 99)
  bool log_movie_pulsing = false;  // started pressing Start over the movie
  bool log_movie_left = false;     // the movie ended or was skipped: `movie_frames` of pressing
  bool log_movie_gave_up = false;  // movie pulse cap reached
  uint32_t movie_frames = 0;
};

// Phase 0 (init): remember the requested start state, forget the last entry.
inline void TitleSkipOnInit(TitleSkipState& s, uint32_t start_state) {
  s = TitleSkipState{};
  s.start_state = start_state;
}

// Phase 1 (update), once per frame, before the game's own dispatcher runs.
// `state`/`next`/`load_step` come from the title block.
inline TitleSkipAction TitleSkipOnUpdate(TitleSkipState& s, SkipIntro level, uint32_t state,
                                         uint32_t next, uint32_t load_step) {
  TitleSkipAction a;
  if (s.start_state == 0xFFFFFFFFu) return a;  // no init seen (hook attached late)
  // The block address is trusted only once the state read from it is the
  // start state the init phase was given.
  if (!s.block_ok) {
    if (state == s.start_state) {
      s.block_ok = true;
    } else {
      if (!s.warned_block) {
        s.warned_block = true;
        a.log_block_mismatch = true;
      }
      return a;
    }
  }
  if (level == SkipIntro::kOff) return a;
  // Only a boot (start state 80) shows the logos; returning to the title from
  // the game starts elsewhere and has nothing to skip.
  if (s.start_state != kTitleStateBoot) return a;

  // Not gated on the load step: on a cold start the logos begin at step 16
  // (USA log 159: 80 -> 81 at step 16, 18 arrived during logo 2), and state
  // 97 itself idles until step 18 before moving on.
  (void)load_step;
  if (!s.logos_skipped && state == kTitleStateBoot && next == kTitleStateBoot) {
    s.logos_skipped = true;
    a.jump_to_title = true;
    return a;
  }
  // [new_fix_25092026_glitch] the opening movie (see the header comment): only
  // after our own jump to 97, pressed like the press start screen, stopped
  // for good once the machine leaves 99 or after the cap.
  if (s.logos_skipped && !s.movie_done) {
    if (state == kTitleStateOpeningMovie && next == kTitleStateOpeningMovie) {
      if (!s.movie_pulsing) {
        s.movie_pulsing = true;
        s.movie_frames = 0;
        a.log_movie_pulsing = true;
      }
      if (s.movie_frames % kTitleStartPulsePeriod == 0) a.inject_start = true;
      ++s.movie_frames;
      if (s.movie_frames >= kTitleMoviePulseMaxFrames) {
        s.movie_done = true;
        a.log_movie_gave_up = true;
        a.inject_start = false;  // giving up: no last press, whatever the constants
        a.cancel_injection = true;
        a.movie_frames = s.movie_frames;
      }
      return a;
    }
    if (s.movie_pulsing) {
      s.movie_done = true;
      a.cancel_injection = true;
      a.log_movie_left = true;
      a.left_state = state;
      a.movie_frames = s.movie_frames;
    }
  }
  if (level != SkipIntro::kMenu || s.start_done) return a;

  if (state == kTitleStatePressStart && next == kTitleStatePressStart) {
    if (!s.start_pulsing) {
      s.start_pulsing = true;
      s.pulse_frames = 0;
      a.log_started_pulsing = true;
    }
    if (s.pulse_frames < kTitleStartPulseMaxFrames) {
      if (s.pulse_frames % kTitleStartPulsePeriod == 0) a.inject_start = true;
      ++s.pulse_frames;
      if (s.pulse_frames == kTitleStartPulseMaxFrames) {
        a.log_gave_up = true;
        a.inject_start = false;  // [new_fix_25092026_glitch] giving up: no last press
        a.cancel_injection = true;
        s.start_done = true;  // stop for good this entry
      }
    }
  } else if (s.start_pulsing) {
    // Left state 0 (106 = accepted; anything else = the game moved on by
    // itself, e.g. the attract demo). Either way: stop, once, for this entry.
    s.start_done = true;
    a.cancel_injection = true;
    a.log_start_left = true;
    a.left_state = state;
    a.pulse_frames = s.pulse_frames;
  }
  return a;
}

// Exact names only; anything else is off and `recognised` says so.
inline SkipIntro ParseSkipIntro(const char* v, bool* recognised = nullptr) {
  auto eq = [](const char* a, const char* b) {
    while (*a && *a == *b) { ++a; ++b; }
    return *a == *b;
  };
  SkipIntro r = SkipIntro::kOff;
  bool ok = v != nullptr;
  if (!v) {
  } else if (eq(v, "off")) r = SkipIntro::kOff;
  else if (eq(v, "logos")) r = SkipIntro::kLogos;
  else if (eq(v, "menu")) r = SkipIntro::kMenu;
  else ok = false;
  if (recognised) *recognised = ok;
  return r;
}

// [new_fix_25092026_glitch] How the hook applies an action's pad requests: the
// cancel first, then the press. One update can carry both (the machine goes
// 99 -> 0 in one step, or a hot reload lands in 99 and then 0: "movie over" and
// the first press-start press); pressing first let the cancel wipe that press.
// A give-up never carries a press, so this order never re-arms one.
template <class Inject, class Cancel>
inline void ApplyTitleSkipPad(const TitleSkipAction& a, Inject&& inject, Cancel&& cancel) {
  if (a.cancel_injection) cancel();
  if (a.inject_start) inject();
}

}  // namespace dp
