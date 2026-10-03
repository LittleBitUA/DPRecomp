// [new_fix_03102026_effects] Fixed-step effects at 60 FPS.
//
// The effect update (PAL sub_82548180 / USA its copy, merge point PAL
// 0x8254824C / USA 0x8254FE74) normally advances an effect by the game's
// logic tick (float at 0x842AD3B0, in 1/60 s units: 2.0 at the console's 30
// FPS). Effects with bit 1 of their flags at +604 (+0x25C; set for effect
// types 0x1E and 0x2A..0x2E: muzzle flashes, splashes, hits) take a constant
// 1.0 instead, which on the console makes them run at half the speed of the
// others, on purpose. In the 60 FPS mode the tick is 1.0 per frame, so that
// constant 1.0 arrives twice as often: those effects played twice as fast as
// on the console. Giving them half the tick restores the console's pace:
// exactly 1.0 at 30 FPS (tick 2), 0.5 at 60 FPS (tick 1).
#pragma once

#include <cstdint>

namespace dp {

inline constexpr uint32_t kEffectFlagsOffset = 604;    // +0x25C
inline constexpr uint32_t kEffectFixedStepBit = 0x2;   // rlwinm r9,r10,0,30,30
inline constexpr uint32_t kGameTickAddress = 0x842AD3B0;

inline bool IsFixedStepEffect(uint32_t flags) { return (flags & kEffectFixedStepBit) != 0; }

// The delta a fixed-step effect gets for a frame whose logic tick is `tick`.
inline double FixedStepEffectDelta(double tick) { return tick * 0.5; }

}  // namespace dp
