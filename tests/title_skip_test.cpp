// deadlyprem - tests for the intro skip state machine (DPRecomp #19).
// Plain executable, exit code 0 = pass. Target dp_title_skip_test.
#include <cstdint>
#include <cstdio>
#include <initializer_list>

#include "../src/deadlyprem_title_skip.h"

namespace {

int g_failures = 0;

#define CHECK(cond)                                                              \
  do {                                                                           \
    if (!(cond)) {                                                               \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                \
      ++g_failures;                                                              \
    }                                                                            \
  } while (0)

using dp::SkipIntro;
using dp::TitleSkipAction;
using dp::TitleSkipOnInit;
using dp::TitleSkipOnUpdate;
using dp::TitleSkipState;

bool Quiet(const TitleSkipAction& a) {
  return !a.jump_to_title && !a.inject_start && !a.cancel_injection && !a.log_started_pulsing &&
         !a.log_start_left && !a.log_block_mismatch && !a.log_gave_up && !a.log_movie_pulsing &&
         !a.log_movie_left && !a.log_movie_gave_up;  // [new_fix_25092026_glitch]
}

// Replays the measured boot (log 144) with the given level; returns the
// number of synthetic presses issued and whether the jump happened.
struct BootResult {
  bool jumped = false;
  int presses = 0;
  int movie_presses = 0;  // [new_fix_25092026_glitch] Start pressed over the opening movie (state 99)
  int accepted_after = -1;
  bool cancelled = false;
};

// [new_fix_25092026_glitch] `movie_listen_after`: the frame of state 99 from
// which a press skips the opening movie (the game then goes 99 -> 100 -> 0).
BootResult Boot(SkipIntro level, int listen_after_frames = 50, int movie_listen_after = 30) {
  BootResult r;
  TitleSkipState s;
  TitleSkipOnInit(s, 80);
  // loading: state 80, sub-steps climbing
  TitleSkipAction a = TitleSkipOnUpdate(s, level, 80, 80, 0);  // first update frame, still loading
  r.jumped = a.jump_to_title;
  CHECK(!a.inject_start);
  if (!r.jumped) {
    // the game runs its logos; nothing from us until the press start screen
    for (uint32_t st : {81u, 82u, 87u, 88u, 97u, 98u, 99u, 100u})
      CHECK(Quiet(TitleSkipOnUpdate(s, level, st, st, 18)));
  } else {
    CHECK(Quiet(TitleSkipOnUpdate(s, level, 98, 98, 18)));
    // [new_fix_25092026_glitch] the opening movie: Start every 6 frames until
    // the game takes one, then one cancel and the state it went to.
    for (int f = 0; f < 2000; ++f) {
      a = TitleSkipOnUpdate(s, level, 99, 99, 18);
      if (f == 0) CHECK(a.log_movie_pulsing);
      if (a.inject_start) ++r.movie_presses;
      CHECK(!a.log_movie_gave_up);
      if (a.inject_start && f >= movie_listen_after) {
        TitleSkipAction b = TitleSkipOnUpdate(s, level, 100, 100, 18);
        CHECK(b.cancel_injection && b.log_movie_left && b.left_state == 100 && !b.inject_start);
        break;
      }
    }
    CHECK(Quiet(TitleSkipOnUpdate(s, level, 100, 100, 18)));
  }
  // press start screen: the game listens from frame `listen_after_frames`
  bool game_saw_press = false;
  for (int f = 0; f < 2000 && r.accepted_after < 0; ++f) {
    a = TitleSkipOnUpdate(s, level, 0, 0, 18);
    if (a.inject_start) ++r.presses;
    if (f == 0) CHECK(a.log_started_pulsing == (level == SkipIntro::kMenu));
    if (a.inject_start && f >= listen_after_frames) game_saw_press = true;
    if (game_saw_press) {
      // next frame the game is in 106 (then 1, 2)
      TitleSkipAction b = TitleSkipOnUpdate(s, level, 106, 106, 18);
      CHECK(b.cancel_injection && b.log_start_left && b.left_state == 106);
      r.cancelled = b.cancel_injection;
      r.accepted_after = f + 1;
    }
    if (a.log_gave_up) break;
  }
  return r;
}

}  // namespace

int main() {
  // off: nothing ever happens, including on the press start screen.
  {
    BootResult r = Boot(SkipIntro::kOff);
    CHECK(!r.jumped);
    CHECK(r.presses == 0);
  }
  // logos: the jump, Start over the opening movie only, no press on the press
  // start screen.
  {
    BootResult r = Boot(SkipIntro::kLogos);
    CHECK(r.jumped);
    CHECK(r.presses == 0);
    CHECK(r.movie_presses == 6);  // frames 0, 6, ..., 30  [new_fix_25092026_glitch]
  }
  // menu: the jump, presses every 6 frames (2 polls each), accepted once the
  // game listens (frame 50 -> press at 54), then cancelled and silent.
  {
    BootResult r = Boot(SkipIntro::kMenu);
    CHECK(r.jumped);
    CHECK(r.accepted_after == 55);
    CHECK(r.presses == 10);  // frames 0, 6, ..., 54
    CHECK(r.cancelled);
    CHECK(r.movie_presses == 6);  // [new_fix_25092026_glitch]
  }
  // off: the opening movie plays untouched.
  {
    BootResult r = Boot(SkipIntro::kOff);
    CHECK(r.movie_presses == 0);
  }
  // [new_fix_25092026_glitch] A movie that never takes Start: 15 s of presses,
  // one cancel and "gave up", then silence in 99 (and nothing else is broken:
  // the press start screen afterwards is still pressed in menu mode).
  {
    TitleSkipState s;
    TitleSkipOnInit(s, 80);
    TitleSkipOnUpdate(s, SkipIntro::kMenu, 80, 80, 18);
    int presses = 0, gave_up = 0, cancels = 0;
    for (int f = 0; f < 3000; ++f) {
      TitleSkipAction a = TitleSkipOnUpdate(s, SkipIntro::kMenu, 99, 99, 18);
      presses += a.inject_start;
      gave_up += a.log_movie_gave_up;
      cancels += a.cancel_injection;
    }
    CHECK(presses == 150);  // 900 / 6
    CHECK(gave_up == 1);
    CHECK(cancels == 1);
    CHECK(Quiet(TitleSkipOnUpdate(s, SkipIntro::kMenu, 100, 100, 18)));
    CHECK(TitleSkipOnUpdate(s, SkipIntro::kMenu, 0, 0, 18).inject_start);
  }
  // [new_fix_25092026_glitch] Only after our own jump: the logos played by the
  // game (a logo already queued as next, or skip switched on late) leave the
  // movie alone; and once left, 99 is never pressed again this entry.
  {
    TitleSkipState s;
    TitleSkipOnInit(s, 80);
    CHECK(Quiet(TitleSkipOnUpdate(s, SkipIntro::kLogos, 80, 82, 18)));  // no jump
    for (int f = 0; f < 50; ++f) CHECK(Quiet(TitleSkipOnUpdate(s, SkipIntro::kLogos, 99, 99, 18)));
    TitleSkipState t;
    TitleSkipOnInit(t, 80);
    CHECK(TitleSkipOnUpdate(t, SkipIntro::kLogos, 80, 80, 18).jump_to_title);
    CHECK(TitleSkipOnUpdate(t, SkipIntro::kLogos, 99, 99, 18).inject_start);
    CHECK(TitleSkipOnUpdate(t, SkipIntro::kLogos, 0, 0, 18).log_movie_left);
    for (int f = 0; f < 50; ++f) CHECK(Quiet(TitleSkipOnUpdate(t, SkipIntro::kLogos, 99, 99, 18)));
  }
  // [new_fix_25092026_glitch] Returning to the title from the game (start
  // state 0) does not touch a movie either.
  {
    TitleSkipState s;
    TitleSkipOnInit(s, 0);
    CHECK(Quiet(TitleSkipOnUpdate(s, SkipIntro::kMenu, 0, 0, 18)));  // the start state validates the block
    for (int f = 0; f < 50; ++f) CHECK(Quiet(TitleSkipOnUpdate(s, SkipIntro::kMenu, 99, 99, 18)));
  }
  // After acceptance nothing more this entry: the game returning to the
  // press start screen (attract demo over, sign-in detour) is NOT pressed
  // again, and no wrap-around of the frame counter.
  {
    TitleSkipState s;
    TitleSkipOnInit(s, 80);
    TitleSkipOnUpdate(s, SkipIntro::kMenu, 80, 80, 18);
    TitleSkipOnUpdate(s, SkipIntro::kMenu, 0, 0, 18);
    TitleSkipAction b = TitleSkipOnUpdate(s, SkipIntro::kMenu, 106, 106, 18);
    CHECK(b.cancel_injection);
    for (int i = 0; i < 5000; ++i) CHECK(Quiet(TitleSkipOnUpdate(s, SkipIntro::kMenu, 0, 0, 18)));
  }
  // The game leaving state 0 on its own (attract demo after 60 s) ends the
  // pulsing too, with the state it went to.
  {
    TitleSkipState s;
    TitleSkipOnInit(s, 80);
    TitleSkipOnUpdate(s, SkipIntro::kMenu, 80, 80, 18);
    TitleSkipOnUpdate(s, SkipIntro::kMenu, 0, 0, 18);
    TitleSkipAction b = TitleSkipOnUpdate(s, SkipIntro::kMenu, 96, 97, 18);
    CHECK(b.cancel_injection && b.log_start_left && b.left_state == 96);
    CHECK(Quiet(TitleSkipOnUpdate(s, SkipIntro::kMenu, 0, 0, 18)));
  }
  // The cap: a screen that never listens gets 30 s of presses, then one
  // cancel + "gave up", then silence (no wrap).
  {
    TitleSkipState s;
    TitleSkipOnInit(s, 80);
    TitleSkipOnUpdate(s, SkipIntro::kMenu, 80, 80, 18);
    int presses = 0, gave_up = 0, cancels = 0;
    for (int f = 0; f < 4000; ++f) {
      TitleSkipAction a = TitleSkipOnUpdate(s, SkipIntro::kMenu, 0, 0, 18);
      presses += a.inject_start;
      gave_up += a.log_gave_up;
      cancels += a.cancel_injection;
    }
    CHECK(presses == 300);  // 1800 / 6
    CHECK(gave_up == 1);
    CHECK(cancels == 1);
  }
  // Returning to the title from the game (start state 0): nothing, even
  // with menu on and the screen sitting in state 0.
  {
    TitleSkipState s;
    TitleSkipOnInit(s, 0);
    for (int f = 0; f < 200; ++f) CHECK(Quiet(TitleSkipOnUpdate(s, SkipIntro::kMenu, 0, 0, 18)));
    CHECK(Quiet(TitleSkipOnUpdate(s, SkipIntro::kMenu, 80, 80, 18)));
  }
  // Hot-reload: menu switched on while already on the press start screen
  // presses from that frame on; off switched on mid-pulse stops pressing
  // (no cancel is needed: the injection expires by itself).
  {
    TitleSkipState s;
    TitleSkipOnInit(s, 80);
    CHECK(Quiet(TitleSkipOnUpdate(s, SkipIntro::kOff, 80, 80, 0)));  // validates the block
    for (int f = 0; f < 10; ++f) CHECK(Quiet(TitleSkipOnUpdate(s, SkipIntro::kOff, 0, 0, 18)));
    TitleSkipAction a = TitleSkipOnUpdate(s, SkipIntro::kMenu, 0, 0, 18);
    CHECK(a.log_started_pulsing && a.inject_start);
    CHECK(Quiet(TitleSkipOnUpdate(s, SkipIntro::kOff, 0, 0, 18)));
  }
  // Wrong block address: the state read never equals the start state ->
  // one warning per entry, never any write.
  {
    TitleSkipState s;
    TitleSkipOnInit(s, 80);
    TitleSkipAction a = TitleSkipOnUpdate(s, SkipIntro::kMenu, 12345, 0, 18);
    CHECK(a.log_block_mismatch && !a.jump_to_title);
    for (int f = 0; f < 100; ++f) CHECK(Quiet(TitleSkipOnUpdate(s, SkipIntro::kMenu, 12345, 0, 18)));
    // a second title entry warns again
    TitleSkipOnInit(s, 80);
    CHECK(TitleSkipOnUpdate(s, SkipIntro::kMenu, 12345, 0, 18).log_block_mismatch);
  }
  // No init seen (hook attached late): nothing.
  {
    TitleSkipState s;
    CHECK(Quiet(TitleSkipOnUpdate(s, SkipIntro::kMenu, 80, 80, 18)));
  }
  // Jump only once per entry, only from 80/80 (a logo already queued as
  // next is left alone), whatever the load step.
  {
    TitleSkipState s;
    TitleSkipOnInit(s, 80);
    CHECK(TitleSkipOnUpdate(s, SkipIntro::kLogos, 80, 80, 3).jump_to_title);
    CHECK(Quiet(TitleSkipOnUpdate(s, SkipIntro::kLogos, 80, 80, 18)));
    TitleSkipState s2;
    TitleSkipOnInit(s2, 80);
    CHECK(Quiet(TitleSkipOnUpdate(s2, SkipIntro::kLogos, 80, 82, 18)));
  }
  // [new_fix_25092026_glitch] 99 -> 0 in one step (no frame of 100, or a hot
  // reload: logos, then off in 99, then menu in 0): one update says both
  // "movie over" (cancel) and "first press-start press". Applied in the hook's
  // order the press survives. Injector model: a cancel clears what is armed, a
  // press arms kTitleStartPulsePolls polls. Critic finding 25.09: the hook
  // used to press first, so the cancel erased the press.
  for (bool hot_reload : {false, true}) {
    TitleSkipState s;
    TitleSkipOnInit(s, 80);
    CHECK(TitleSkipOnUpdate(s, SkipIntro::kLogos, 80, 80, 0).jump_to_title);
    CHECK(TitleSkipOnUpdate(s, SkipIntro::kLogos, 99, 99, 18).inject_start);
    if (hot_reload) CHECK(Quiet(TitleSkipOnUpdate(s, SkipIntro::kOff, 99, 99, 18)));
    const TitleSkipAction a = TitleSkipOnUpdate(s, SkipIntro::kMenu, 0, 0, 18);
    CHECK(a.cancel_injection && a.log_movie_left && a.left_state == 0);
    CHECK(a.inject_start && a.log_started_pulsing);
    uint32_t armed = 2;  // the movie's last press, still armed
    int order = 0, cancel_at = -1, press_at = -1;
    dp::ApplyTitleSkipPad(
        a, [&] { armed = dp::kTitleStartPulsePolls; press_at = order++; },
        [&] { armed = 0; cancel_at = order++; });
    CHECK(armed == dp::kTitleStartPulsePolls && cancel_at == 0 && press_at == 1);
  }
  // Neither give-up carries a press, so cancel-first never re-arms one there.
  {
    TitleSkipState s;
    TitleSkipOnInit(s, 80);
    TitleSkipOnUpdate(s, SkipIntro::kMenu, 80, 80, 0);
    int gave_up = 0;
    for (int f = 0; f < 1000; ++f) {
      const TitleSkipAction a = TitleSkipOnUpdate(s, SkipIntro::kMenu, 99, 99, 18);
      if (a.log_movie_gave_up) {
        ++gave_up;
        CHECK(a.cancel_injection && !a.inject_start);
      }
    }
    CHECK(gave_up == 1);
    gave_up = 0;
    for (int f = 0; f < 2000; ++f) {
      const TitleSkipAction a = TitleSkipOnUpdate(s, SkipIntro::kMenu, 0, 0, 18);
      if (a.log_gave_up) {
        ++gave_up;
        CHECK(a.cancel_injection && !a.inject_start);
      }
    }
    CHECK(gave_up == 1);
  }
  // No press and no cancel: the helper does nothing.
  {
    int calls = 0;
    dp::ApplyTitleSkipPad(TitleSkipAction{}, [&] { ++calls; }, [&] { ++calls; });
    CHECK(calls == 0);
  }

  // Parsing: exact names only.
  {
    bool ok = false;
    CHECK(dp::ParseSkipIntro("off", &ok) == SkipIntro::kOff && ok);
    CHECK(dp::ParseSkipIntro("logos", &ok) == SkipIntro::kLogos && ok);
    CHECK(dp::ParseSkipIntro("menu", &ok) == SkipIntro::kMenu && ok);
    CHECK(dp::ParseSkipIntro("no", &ok) == SkipIntro::kOff && !ok);
    CHECK(dp::ParseSkipIntro("none", &ok) == SkipIntro::kOff && !ok);
    CHECK(dp::ParseSkipIntro("Menu", &ok) == SkipIntro::kOff && !ok);
    CHECK(dp::ParseSkipIntro("", &ok) == SkipIntro::kOff && !ok);
    CHECK(dp::ParseSkipIntro(nullptr, &ok) == SkipIntro::kOff && !ok);
  }

  if (g_failures) {
    std::printf("%d failure(s)\n", g_failures);
    return 1;
  }
  std::printf("title_skip_test: all checks passed\n");
  return 0;
}
