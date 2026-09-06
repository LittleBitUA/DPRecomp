// deadlyprem - mid-asm hooks (DP1, 2026-09-06).
//
// 60 FPS: port of ehw's Xenia patch for the USA XEX (494F07D4, DPRecomp
// issue #3) to the PAL XEX (4D5607D1). The three patched instructions sit in
// the frame pacing / vblank gate routine; the PAL copy of that routine was
// located by matching the instruction pattern (same +0x28 / +0x94 offsets):
//   USA 0x8252A27C  fadds f13,f0,f29 -> fmr f13,f0    PAL 0x82522654
//   USA 0x8252A2A4  bge   -> b +0x10 (skip the gate)  PAL 0x8252267C -> 0x8252268C
//   USA 0x8252A310  subf r8,r11,r10 -> li r8,1        PAL 0x825226E8
// Instead of patching bytes, the codegen emits calls to the hooks below at
// those addresses (see [[midasm_hook]] in deadlyprem_config.toml), and a cvar
// keeps the original behaviour reachable without a rebuild.

#include <rex/cvar.h>

#include "deadlyprem_pch.h"  // PPCRegister / PPCContext (generated/default is on the include path)

REXCVAR_DEFINE_BOOL(dp_60fps, true, "DP1",
                    "60 FPS: bypass the vblank-count gate, pin the logic delta to one vblank "
                    "and keep the in-game timer at real time (ehw's patch, DPRecomp #3). "
                    "Needs a 60 Hz guest video mode (video_mode_refresh_rate = 60).")
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

// PAL 0x825226E8, after `subf r8,r11,r10`: equivalent of `li r8,1`.
void DP60FpsDeltaHook(PPCRegister& r8) {
  if (REXCVAR_GET(dp_60fps)) {
    r8.u64 = 1;
  }
}

