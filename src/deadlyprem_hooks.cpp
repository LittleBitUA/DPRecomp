// deadlyprem - mid-asm hooks (DP1, 2026-09-06).
//
// 60 FPS: port of ehw's Xenia patch for the USA XEX (494F07D4, DPRecomp
// issue #3) to the PAL XEX (4D5607D1). The patched instructions sit in the
// frame pacing / vblank gate routine (PAL sub_825224B0); the PAL copy of that
// routine was located by matching the instruction pattern (same +0x28 / +0x94
// offsets):
//   USA 0x8252A27C  fadds f13,f0,f29 -> fmr f13,f0    PAL 0x82522654
//   USA 0x8252A2A4  bge   -> b +0x10 (skip the gate)  PAL 0x8252267C -> 0x8252268C
//   USA 0x8252A310  subf r8,r11,r10 -> li r8,1        PAL 0x825226E8
// Instead of patching bytes, the codegen emits calls to the hooks below at
// those addresses (see [[midasm_hook]] in deadlyprem_config.toml), and a cvar
// keeps the original behaviour reachable without a rebuild.
//
// How the routine works (constants read from the XEX, 2026-09-06):
//   * frame limiter: elapsed = QPC delta in ms; if elapsed < 16.667 ms then
//     Sleep((16.667 - elapsed) + 11.667) and re-check -> ~28 ms = the 30 FPS
//     cap. Hook 1 drops the +11.667 so the limiter targets 16.667 ms.
//   * vblank gate: wait until (vblank_now - vblank_last) >= 2. Hook 2 skips it.
//   * logic tick: r8 = vblank_now - vblank_last, converted to float, clamped
//     to [1, 4] and multiplied into the game's delta-time global (units of
//     1/60 s; 2 at 30 FPS). ehw's original `li r8,1` pins it to exactly one
//     vblank, which decouples game time from real time: every frame longer
//     than 16.67 ms (Sleep overshoot, heavy cutscenes) slows the game while
//     the streamed audio keeps real time -> animation drifts behind the audio
//     (DPRecomp #12). Hook 3 (2026-09-06, v1.1) therefore derives the tick from
//     the host clock instead: tick = frame_time / 16.667 ms, so a 50 FPS frame
//     advances 1.2 ticks and a 60 FPS frame exactly 1.0.
//     PAL 0x82522704 (USA 0x8252A32C) = `stfs f0,-11344(r24)`, right after the
//     integer -> float conversion of the vblank delta; the hook overrides f0.

#include <algorithm>
#include <chrono>
#include <cmath>

#include <rex/cvar.h>
#include <rex/logging.h>

#include "deadlyprem_pch.h"  // PPCRegister / PPCContext (generated/default is on the include path)

REXCVAR_DEFINE_BOOL(dp_60fps, true, "DP1",
                    "60 FPS: bypass the vblank-count gate, keep the in-game frame limiter at "
                    "16.67 ms and derive the logic tick from real frame time (ehw's patch, "
                    "DPRecomp #3, tick fix #12). Needs a 60 Hz guest video mode "
                    "(video_mode_refresh_rate = 60).")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_DOUBLE(dp_60fps_tick_min, 0.5, "DP1",
                      "Lower clamp of the real-time logic tick in 1/60 s units. 1.0 = never "
                      "slower than one tick per frame (v1.1; frames shorter than 16.67 ms then "
                      "ran the game ahead of the audio, #12), 0.5 = follow real time (v1.2)")
    .range(0.1, 1.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_DOUBLE(dp_60fps_tick_max, 4.0, "DP1",
                      "Upper clamp of the real-time logic tick in 1/60 s units (the game itself "
                      "clamps to [1, 4]); frames longer than this are treated as a hitch")
    .range(1.0, 4.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(dp_60fps_stats, false, "DP1",
                    "Log frame time / game-time-vs-real-time statistics every 600 frames "
                    "(diagnostics for #12)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(dp_60fps_integer_tick, true, "DP1",
                    "60 FPS: feed the game whole 1/60 s ticks (1, sometimes 2) with an error "
                    "accumulator, so game time tracks real time exactly while the tick stays "
                    "an integer like the console's vblank count (the game's scripted scenes "
                    "misbehave with fractional ticks, and this is also what the PC port does "
                    "in cutscenes). false = fractional real-time tick clamped to "
                    "[dp_60fps_tick_min, dp_60fps_tick_max] (1.1/1.2 behaviour)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

// PAL 0x82522654, after `fadds f13,f0,f29`: equivalent of `fmr f13,f0`.
void DP60FpsTimerHook(PPCRegister& f13, PPCRegister& f0) {
  if (REXCVAR_GET(dp_60fps)) {
    f13.f64 = f0.f64;
  }
}

// PAL 0x8252267C, before `bge cr6,0x8252268C`: true = take the branch
// unconditionally (equivalent of replacing bge with b).
bool DP60FpsVblankGateHook() {
  return REXCVAR_GET(dp_60fps);
}

// PAL 0x8252271C (USA 0x8252A344), before `bge cr6,<skip fmr f0,f28>`: the game
// clamps the tick it just stored to >= 1.0 (f28 = DAT_82000B94 = 1.0f). With
// the real-time tick that turns the symmetric frame-time jitter (13..20 ms
// around a 16.7 ms mean) into a systematic speed-up: short frames are rounded
// up to 1.0, long frames count in full, measured +0.2..0.7 % here and more on
// jittery machines - the animation runs ahead of the streamed audio over a
// long cutscene (DPRecomp #12, 1.1 and 1.2). Take the branch unconditionally
// in 60 FPS mode; DP60FpsTickHook has already applied [tick_min, tick_max].
bool DP60FpsTickClampHook() {
  return REXCVAR_GET(dp_60fps);
}

// PAL 0x82522704, before `stfs f0,-11344(r24)`: f0 holds the vblank delta as
// a float. Replace it with the real frame time in 1/60 s units.
void DP60FpsTickHook(PPCRegister& f0) {
  using clock = std::chrono::steady_clock;
  static clock::time_point last_frame{};
  static bool have_last = false;
  static double accumulator = 0.0;  // real time not yet handed to the game, in ticks
  const clock::time_point now = clock::now();
  if (!REXCVAR_GET(dp_60fps)) {
    // 30 FPS mode: keep the game's own vblank-count tick, just track time.
    last_frame = now;
    have_last = true;
    accumulator = 0.0;
    return;
  }
  const double raw_frame_ms =
      have_last ? std::chrono::duration<double, std::milli>(now - last_frame).count() : 0.0;
  last_frame = now;
  have_last = true;
  // Loading screens / alt-tab / debugger stalls: a very long gap is a hitch,
  // not game time to catch up on (the game clamps to 4 ticks anyway).
  const bool frame_valid = raw_frame_ms > 0.0 && raw_frame_ms < 500.0;
  const double real_ticks = frame_valid ? raw_frame_ms / (1000.0 / 60.0) : 1.0;

  const double tick_max = REXCVAR_GET(dp_60fps_tick_max);
  double tick;
  if (REXCVAR_GET(dp_60fps_integer_tick)) {
    // Whole ticks, like the console's vblank delta: hand out round(accumulated
    // real time), never less than 1 (the game never ran slower than one tick
    // per frame), at most tick_max; the remainder carries over, so a 16.72 ms
    // average frame gives 1, 1, 1, ... and a 2 every few hundred frames
    // instead of a permanent 0.3 % speed-up.
    accumulator += real_ticks;
    tick = std::floor(accumulator + 0.5);
    tick = std::clamp(tick, 1.0, std::max(1.0, tick_max));
    accumulator -= tick;
    // Never bank more than a couple of ticks of debt or credit (hitches).
    accumulator = std::clamp(accumulator, -2.0, 2.0);
  } else {
    const double tick_min = std::min(REXCVAR_GET(dp_60fps_tick_min), tick_max);
    tick = std::clamp(real_ticks, tick_min, tick_max);
    accumulator = 0.0;
  }
  f0.f64 = tick;

  // Diagnostics (#12): game time vs real time.
  if (REXCVAR_GET(dp_60fps_stats) && frame_valid) {
    static uint32_t n = 0;
    static double sum_ms = 0.0, min_ms = 1e9, max_ms = 0.0, sum_tick = 0.0;
    static uint32_t sub_one = 0, multi = 0;
    ++n;
    sum_ms += raw_frame_ms;
    min_ms = std::min(min_ms, raw_frame_ms);
    max_ms = std::max(max_ms, raw_frame_ms);
    sum_tick += tick;
    if (tick < 1.0) ++sub_one;
    if (tick >= 2.0) ++multi;
    if (n >= 600) {
      REXLOG_INFO(
          "DP60 stats: {} frames avg {:.3f} ms (min {:.2f} max {:.2f}), tick<1 on {} frames, "
          "tick>=2 on {} frames, accumulator {:+.3f}; game/real {:.4f}",
          n, sum_ms / n, min_ms, max_ms, sub_one, multi, accumulator,
          sum_tick * (1000.0 / 60.0) / sum_ms);
      n = 0;
      sum_ms = 0.0;
      min_ms = 1e9;
      max_ms = 0.0;
      sum_tick = 0.0;
      sub_one = 0;
      multi = 0;
    }
  }
}
