// [new_fix_27092026_i36]
// Tests for even 60 FPS frame pacing (src/deadlyprem_frame_pacing.h). Plain
// executable, exit code 0 = pass. Build target dp_frame_pacing_test.
//
// The simulation reproduces the game's limiter: wait until
// QPC - last >= 1/60 s (with a small overshoot), present (a variable cost),
// store last = QPC. It then feeds the resulting frame times to the 60 FPS
// integer tick (round the accumulated real time, >= 1) and counts double
// ticks, which are the visible jerks of #36.
#include <cmath>
#include <cstdint>
#include <cstdio>

#include "../src/deadlyprem_frame_pacing.h"

namespace {

int g_failures = 0;

#define CHECK(cond)                                               \
  do {                                                            \
    if (!(cond)) {                                                \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      ++g_failures;                                               \
    }                                                             \
  } while (0)

constexpr uint64_t kFreq = 10000000;  // 10 MHz, the usual Windows QPC rate

// Deterministic pseudo-random 0..1.
double Rand(uint32_t& s) {
  s = s * 1664525u + 1013904223u;
  return double(s >> 8) / double(1u << 24);
}

struct Result {
  double avg_ms;
  int double_ticks;
  uint64_t resyncs;
};

Result Simulate(bool paced, int frames, double hitch_every = 0) {
  dp::FramePacer pacer;
  uint32_t seed = 12345;
  const double threshold_ticks = 16.66666603088379 * 1e-3 * double(kFreq);  // float 16.667 ms
  uint64_t clock = 1000000;
  uint64_t last = clock;
  uint64_t prev_hook = clock;
  double acc = 0;
  int doubles = 0;
  double sum_ms = 0;
  for (int i = 0; i < frames; ++i) {
    // wait loop: Sleep(0) polling, overshoot 0..0.1 ms past the threshold
    const uint64_t due = last + uint64_t(std::ceil(threshold_ticks));
    if (clock < due) clock = due;
    clock += uint64_t(Rand(seed) * 0.1e-3 * kFreq);
    // present: 0.2..0.5 ms, and an occasional 40 ms hitch
    clock += uint64_t((0.2e-3 + Rand(seed) * 0.3e-3) * kFreq);
    if (hitch_every > 0 && i % int(hitch_every) == int(hitch_every) - 1) clock += 400000;
    const uint64_t stored = clock;
    last = paced ? dp::PaceFrameStamp(pacer, stored, kFreq) : stored;
    // the tick hook measures host frame time right after this
    const double frame_ms = double(clock - prev_hook) * 1000.0 / double(kFreq);
    prev_hook = clock;
    if (i == 0) continue;
    sum_ms += frame_ms;
    acc += frame_ms / (1000.0 / 60.0);
    double tick = std::floor(acc + 0.5);
    if (tick < 1) tick = 1;
    if (tick > 4) tick = 4;
    acc -= tick;
    if (acc > 2) acc = 2;
    if (acc < -2) acc = -2;
    if (tick >= 2) ++doubles;
  }
  return {sum_ms / (frames - 1), doubles, pacer.resyncs};
}

}  // namespace

int main() {
  // Unpaced: the drift of #36. Average above 16.7 ms, a double tick every
  // few hundred frames.
  const Result old = Simulate(false, 6000);
  CHECK(old.avg_ms > 16.69);
  CHECK(old.double_ticks >= 10);

  // Paced: exactly 1/60 s on average, no double ticks.
  const Result now = Simulate(true, 6000);
  CHECK(std::fabs(now.avg_ms - 1000.0 / 60.0) < 0.001);
  CHECK(now.double_ticks == 0);
  CHECK(now.resyncs == 0);

  // A real hitch (40 ms every 1000 frames) resyncs once each and still
  // produces the catch-up tick it needs, then the pacing is even again.
  const Result hitch = Simulate(true, 6000, 1000);
  CHECK(hitch.resyncs == 6);
  CHECK(hitch.double_ticks <= 6 * 2);

  // Unit behaviour.
  dp::FramePacer p;
  const uint64_t step = kFreq / 60;  // 166666, remainder 40
  CHECK(dp::PaceFrameStamp(p, 1000, kFreq) == 1000);           // first frame: real time
  CHECK(dp::PaceFrameStamp(p, 1000 + step + 500, kFreq) == 1000 + step);  // late by 0.05 ms: ideal
  CHECK(dp::PaceFrameStamp(p, 1000 + 2 * step + 50, kFreq) == 1000 + 2 * step + 1);  // remainder carried (80 >= 60)
  CHECK(p.resyncs == 0);
  // Early (the game did not wait, e.g. a mode switch): resync.
  CHECK(dp::PaceFrameStamp(p, 1000 + 2 * step + 10, kFreq) == 1000 + 2 * step + 10);
  CHECK(p.resyncs == 1);
  // More than a period late: resync.
  const uint64_t base = p.ideal;
  CHECK(dp::PaceFrameStamp(p, base + 3 * step, kFreq) == base + 3 * step);
  CHECK(p.resyncs == 2);
  // Reset: the next frame is taken as is and does not count as a resync.
  dp::ResetFramePacer(p);
  CHECK(dp::PaceFrameStamp(p, 5, kFreq) == 5);
  CHECK(p.resyncs == 2);
  // Zero frequency never divides by zero.
  dp::FramePacer z;
  CHECK(dp::PaceFrameStamp(z, 77, 0) == 77);

  std::printf("unpaced: avg %.4f ms, %d double ticks; paced: avg %.4f ms, %d double ticks; "
              "hitches: %llu resyncs, %d double ticks\n",
              old.avg_ms, old.double_ticks, now.avg_ms, now.double_ticks,
              (unsigned long long)hitch.resyncs, hitch.double_ticks);
  if (g_failures == 0) std::printf("dp_frame_pacing_test: all passed\n");
  return g_failures == 0 ? 0 : 1;
}
