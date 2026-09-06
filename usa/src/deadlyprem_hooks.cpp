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

#include <rex/cvar.h>

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

// PAL 0x82522704, before `stfs f0,-11344(r24)`: f0 holds the vblank delta as
// a float. Replace it with the real frame time in 1/60 s units.
void DP60FpsTickHook(PPCRegister& f0) {
  using clock = std::chrono::steady_clock;
  static clock::time_point last_frame{};
  static bool have_last = false;
  const clock::time_point now = clock::now();
  if (!REXCVAR_GET(dp_60fps)) {
    // 30 FPS mode: keep the game's own vblank-count tick, just track time.
    last_frame = now;
    have_last = true;
    return;
  }
  double tick = 1.0;
  if (have_last) {
    const double frame_ms =
        std::chrono::duration<double, std::milli>(now - last_frame).count();
    // Loading screens / alt-tab / debugger stalls: a very long gap is a hitch,
    // not game time to catch up on (the game clamps to 4 ticks anyway).
    if (frame_ms > 0.0 && frame_ms < 500.0) {
      tick = frame_ms / (1000.0 / 60.0);
    }
  }
  last_frame = now;
  have_last = true;
  const double tick_max = REXCVAR_GET(dp_60fps_tick_max);
  const double tick_min = std::min(REXCVAR_GET(dp_60fps_tick_min), tick_max);
  tick = std::clamp(tick, tick_min, tick_max);
  f0.f64 = tick;
}
