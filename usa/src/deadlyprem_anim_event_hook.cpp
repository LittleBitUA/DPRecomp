// DP1 2026-09-07 (DPRecomp #17, #18): animation keyframe events fire once.
//
// Every motion (.XAM) carries a list of keyframe events (0x34 bytes each:
// +0xA start frame, +0xC end frame, +0xE type, +0x10 sound cue, +0x16 item,
// +0x18 bone / hand slot ...). Each game update, after the motion player has
// advanced its frame position, the animation controller (PAL sub_8253D568,
// two inlined copies, and the standalone sub_8253A0C8 used for layered
// motions) "picks" up to two events into this+0x2e8/+0x2ec by comparing the
// previous and current frame with each event's start and end:
//   A  prev < start <= cur          (start crossed)
//   B  prev < end   <= cur          (end crossed)
//   C  start <= cur <= end          (inside the range)
//   D  motion ended and start + 1 == frame count
// plus wrap-around variants of A/B when the motion looped. The picked slots
// are cleared every update and nothing remembers what already fired, so the
// handlers (play sound cue, attach / detach an item to the hand, pick up /
// put down, footstep, spawn effect) run for every pick.
//
// On the console the logic tick is 2 (30 fps), so an event with end = start
// + 1 is usually crossed start-and-end by the same update: one pick. At 60
// fps the tick is 1: update N crosses the start (A), update N+1 crosses the
// end (B) - two picks, two updates apart. That is the doubled melee whoosh /
// reload click / door knock / car door (#17) and the prop attach event that
// toggles itself right back (coffee cup vs milk pitcher, the cigarette jar
// lid, #18). "Not 100% of the time" = it depends on where the crossing lands.
//
// Fix: for the one-shot event types, only let a pick through when it comes
// from clause A (or its wrap-around variant) or D. Range events consumed as
// state by the game (loop sounds, "is event X active" queries) keep the full
// A/B/C behaviour because their types are not in the one-shot set.
//
// Hook points (mid-asm, before the `stwx r11,r7,rTHIS` that copies the picked
// entry into the controller; true = skip the copy and the count increment):
//   PAL 0x8253A2A8 -> 0x8253A2B8 (standalone, this = r30, flags = r28)
//   PAL 0x8253D9B4 -> 0x8253D9C4 (main update copy 1, this = r28, flags = r25)
//   PAL 0x8253DED8 -> 0x8253DEE8 (main update copy 2)
//   USA = PAL + 0x7C28: 0x82541ED0 -> 0x82541EE0, 0x825455DC -> 0x825455EC,
//   0x82545B00 -> 0x82545B10.
// Registers at all six: r11 = event entry, f30 = previous frame, f31 = current
// frame, flags register bit 0x20 = motion ended during this advance.

#include <cstdint>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/memory/utils.h>
#include <rex/system/kernel_state.h>

#include "deadlyprem_pch.h"

REXCVAR_DEFINE_BOOL(dp_anim_event_dedupe, false, "DP1",
                    "Fire one-shot animation keyframe events (sounds, item attach / detach, "
                    "pick up / put down, footsteps, effects) once per keyframe crossing. "
                    "At 60 FPS the game otherwise triggers them twice (DPRecomp #17, #18)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(dp_anim_event_log, false, "DP1",
                    "Log every animation event pick the dedupe hook sees (type, start, end, "
                    "prev / cur frame, kept or skipped)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace {

bool IsOneShotType(int type) {
  switch (type) {
    case 0x01:  // footstep (bone index at +0x18)
    case 0x02:  // fixed sound cues (object handler)
    case 0x03:  // pick up item
    case 0x04:  // put down item
    case 0x05:  // play sound cue from +0x10
    case 0x06:
    case 0x07:
    case 0x08:
    case 0x09:
    case 0x0A:
    case 0x0B:  // fixed sound cues
    case 0x12:  // fixed sound cue
    case 0x14:  // attach item to hand
    case 0x15:  // detach item from hand
    case 0x16:
    case 0x17:  // effect on bone
    case 0x18:  // effect / model from +0x12/+0x14/+0x16
    case 0x1A:  // effect on bone
    case 0x1B:  // hat on (NT_HAT)
    case 0x1C:  // hat off (PN_HANDR)
    case 0x28:  // create item model on PN_FOOD
    case 0x2B:  // pick up / put down variant
    case 0x2F:  // sound on random
    case 0x36:  // play sound cue from +0x10
      return true;
    default:
      return false;
  }
}

int16_t LoadS16(uint8_t* p) { return rex::memory::load_and_swap<int16_t>(p); }

// Returns true when the pick must be skipped.
bool AnimEventPick(const char* site, uint32_t self, uint32_t entry, double prev, double cur,
                   uint32_t flags) {
  if (!REXCVAR_GET(dp_anim_event_dedupe) || entry == 0) {
    return false;
  }
  auto* memory = REX_KERNEL_MEMORY();
  if (!memory) {
    return false;
  }
  uint8_t* e = memory->TranslateVirtual<uint8_t*>(entry);
  if (!e) {
    return false;
  }
  const int start = LoadS16(e + 0xA);
  const int end = LoadS16(e + 0xC);
  const int type = LoadS16(e + 0xE);
  bool skip = false;
  if (IsOneShotType(type) && end != start) {
    const double s = double(start);
    // Clause A, or its wrap-around variant when the motion looped back.
    const bool start_crossed = (prev <= cur) ? (prev < s && s <= cur) : (s > prev || s <= cur);
    // Clause D: the motion ended in this update - the dispatcher already
    // blocks any further pick of this motion afterwards.
    const bool ended = (flags & 0x20) != 0;
    skip = !start_crossed && !ended;
  }
  if (REXCVAR_GET(dp_anim_event_log)) {
    uint8_t* self_p = memory->TranslateVirtual<uint8_t*>(self);
    const int count = self_p ? int(self_p[0x18]) : -1;
    const uint32_t motion = self_p ? rex::memory::load_and_swap<uint32_t>(self_p + 0x22c) : 0;
    REXLOG_INFO("DP anim event [{}] ctrl {:08X} motion {:08X} slot {} entry {:08X}: type 0x{:02X} start {} "
                "end {} prev {:.2f} cur {:.2f} flags 0x{:X} -> {}",
                site, self, motion, count, entry, type, start, end, prev, cur, flags,
                skip ? "SKIP" : "keep");
  }
  return skip;
}

}  // namespace

// Standalone dispatcher (layered motions): this = r30, flags = r28.
bool DPAnimEventPickHook(PPCRegister& r11, PPCRegister& f30, PPCRegister& f31, PPCRegister& r28,
                         PPCRegister& r30) {
  return AnimEventPick("layer", r30.u32, r11.u32, f30.f64, f31.f64, r28.u32);
}

// Main update, both inlined copies: this = r28, flags = r25.
bool DPAnimEventPickHookMain(PPCRegister& r11, PPCRegister& f30, PPCRegister& f31,
                             PPCRegister& r25, PPCRegister& r28) {
  return AnimEventPick("main", r28.u32, r11.u32, f30.f64, f31.f64, r25.u32);
}
bool DPAnimEventPickHookMain2(PPCRegister& r11, PPCRegister& f30, PPCRegister& f31,
                              PPCRegister& r25, PPCRegister& r28) {
  return AnimEventPick("main2", r28.u32, r11.u32, f30.f64, f31.f64, r25.u32);
}
