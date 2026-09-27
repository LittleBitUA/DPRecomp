// [new_fix_27092026_i39] Guest thread stack sizes (DPRecomp issue #39).
//
// The game creates every one of its threads through one XAPI wrapper (PAL
// sub_825C9A20, USA sub_824EBFD0) that hands the requested stack size to
// ExCreateThread unchanged: GameThread 256 KB, PhysicsThread 384 KB,
// RenderThread 128 KB, MapThread 64 KB. The runtime allocates exactly that,
// with no-access guard pages on both ends, like the console.
//
// The game's collision code allocates its scratch arrays on the stack, sized
// by the shape it tests (PAL sub_825AD830: n * 112 bytes, then
// sub_825A9AA8: n * 280 bytes). In #39 one query needed 241 KB + 268 KB on
// GameThread's 256 KB stack and hit the guard page (read of 0x703CFB00, stack
// limit 0x703D0000). Giving the threads more stack than they ask for costs
// only address space in the 0x70000000 stack range (240 MB) and turns that
// crash into a query that completes.
#pragma once

#include <cstdint>

namespace dp {

// Never grow one stack past this (the whole guest stack range is 240 MB and a
// few dozen threads share it). A request already above the cap is kept.
inline constexpr uint32_t kMaxScaledGuestStack = 4u * 1024u * 1024u;
inline constexpr int32_t kMaxGuestStackScale = 16;

// Stack size to hand to ExCreateThread for a guest request of `requested`
// bytes. 0 stays 0 (ExCreateThread then uses the XEX default), scale <= 1
// keeps the console's size. The result is never below `requested`.
inline uint32_t ScaledGuestStackSize(uint32_t requested, int32_t scale) {
  if (requested == 0 || scale <= 1) return requested;
  if (scale > kMaxGuestStackScale) scale = kMaxGuestStackScale;
  const uint64_t grown = uint64_t(requested) * uint64_t(scale);
  const uint64_t capped = grown < kMaxScaledGuestStack ? grown : kMaxScaledGuestStack;
  return capped > requested ? uint32_t(capped) : requested;
}

}  // namespace dp
