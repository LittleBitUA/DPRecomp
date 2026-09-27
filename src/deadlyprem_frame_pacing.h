// [new_fix_27092026_i36] Even 60 FPS frame pacing (DPRecomp issue #36).
//
// The game's frame limiter (PAL sub_825224B0, USA the same routine +0x7C28)
// waits until QPC - last >= 16.667 ms, presents, then stores
// last = QPC() *after* the present (r31+512). Every frame therefore starts its
// 16.667 ms from a timestamp that is already a little late (the wait loop's
// overshoot plus the present call), so the average frame is 16.70-16.73 ms
// (measured 07.09, logs 076/084). The 60 FPS tick hands the game whole ticks
// and repays the lag with a double tick every few hundred frames: a visible
// jerk every 5-8 s while York or the camera moves. With vsync on the same
// drift makes a frame miss its refresh now and then (the 33 ms frames).
//
// The fix keeps `last` on an ideal timeline: last = previous last + 1/60 s
// whenever the frame is less than one period late, otherwise it resyncs to
// the real time (loading, alt-tab, a real hitch). The limiter then waits for
// exact 1/60 s deadlines, so frames average 16.667 ms and the tick stays 1.
#pragma once

#include <cstdint>

namespace dp {

struct FramePacer {
  uint64_t ideal = 0;  // QPC value handed to the game last time
  uint64_t rem = 0;    // accumulated freq % 60 remainder, in 1/60 QPC units
  bool have = false;
  uint64_t resyncs = 0;
};

// `now` = the QPC value the game just stored, `freq` = QPC ticks per second.
// Returns the value the game should keep as its frame start.
inline uint64_t PaceFrameStamp(FramePacer& p, uint64_t now, uint64_t freq) {
  const uint64_t step = freq / 60;
  if (step == 0) return now;
  if (p.have) {
    uint64_t candidate = p.ideal + step;
    uint64_t rem = p.rem + freq % 60;
    if (rem >= 60) {
      ++candidate;
      rem -= 60;
    }
    // On time: the limiter waited for `candidate`, and the present did not
    // push us a whole period past it.
    if (now >= candidate && now - candidate < step) {
      p.ideal = candidate;
      p.rem = rem;
      return candidate;
    }
  }
  if (p.have) ++p.resyncs;
  p.ideal = now;
  p.rem = 0;
  p.have = true;
  return now;
}

// The game (or 30 FPS mode) stopped feeding frames through the pacer.
inline void ResetFramePacer(FramePacer& p) {
  p.have = false;
  p.rem = 0;
}

}  // namespace dp
