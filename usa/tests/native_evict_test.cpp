// [new_fix_03102026_evict]
// Tests for host texture eviction (src/native/native_evict.h). Plain
// executable, exit code 0 = pass. Build target dp_native_evict_test.
#include <cstdint>
#include <cstdio>
#include <vector>

#include "../src/native/native_evict.h"

namespace {

int g_failures = 0;

#define CHECK(cond)                                               \
  do {                                                            \
    if (!(cond)) {                                                \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      ++g_failures;                                               \
    }                                                             \
  } while (0)

using dp::native::ChooseEvictions;
using dp::native::EvictCandidate;
using dp::native::EvictParams;

EvictCandidate C(uint32_t id, uint64_t last_used, bool resident = true, bool pinned = false) {
  EvictCandidate c;
  c.id = id;
  c.last_used = last_used;
  c.resident = resident;
  c.pinned = pinned;
  return c;
}

bool Contains(const std::vector<uint32_t>& v, uint32_t id) {
  for (uint32_t x : v) {
    if (x == id) return true;
  }
  return false;
}

}  // namespace

int main() {
  EvictParams p;
  p.heap_size = 100;
  p.high_percent = 75;
  p.low_percent = 50;
  p.idle_frames = 3600;
  p.min_idle_frames = 8;

  // Low pressure, nothing old: nothing goes.
  {
    std::vector<EvictCandidate> c = {C(1, 9990), C(2, 9000), C(3, 8000)};
    CHECK(ChooseEvictions(c, 10000, 40, p).empty());
  }

  // Age: idle >= 3600 frames goes even without pressure, the rest stays.
  {
    std::vector<EvictCandidate> c = {C(1, 6000), C(2, 6400), C(3, 6401), C(4, 9999)};
    auto e = ChooseEvictions(c, 10000, 10, p);
    CHECK(e.size() == 2);
    CHECK(Contains(e, 1) && Contains(e, 2));
    CHECK(!Contains(e, 3) && !Contains(e, 4));
  }

  // Pressure: 80 of 100 in use -> least recently used until 50, oldest first.
  {
    std::vector<EvictCandidate> c;
    for (uint32_t i = 0; i < 80; ++i) c.push_back(C(i, 9000 + i));  // all younger than idle_frames
    auto e = ChooseEvictions(c, 10000, 80, p);
    CHECK(e.size() == 30);
    CHECK(e.front() == 0);
    CHECK(e.back() == 29);
  }

  // Pressure exactly at the threshold (75) does not trigger; 76 does.
  {
    std::vector<EvictCandidate> c;
    for (uint32_t i = 0; i < 76; ++i) c.push_back(C(i, 9000 + i));
    CHECK(ChooseEvictions(c, 10000, 75, p).empty());
    CHECK(ChooseEvictions(c, 10000, 76, p).size() == 26);
  }

  // Resolve destinations (pinned), non-resident entries and anything used in
  // the last 8 frames are never chosen, whatever the pressure or age.
  {
    std::vector<EvictCandidate> c = {C(1, 0, true, true), C(2, 0, false), C(3, 9995), C(4, 9992), C(5, 0)};
    auto e = ChooseEvictions(c, 10000, 100, p);
    CHECK(!Contains(e, 1));
    CHECK(!Contains(e, 2));
    CHECK(!Contains(e, 3));  // idle 5 < 8
    CHECK(Contains(e, 4));   // idle 8: allowed under pressure
    CHECK(Contains(e, 5));
  }

  // Pressure cannot evict below what exists: stops when candidates run out.
  {
    std::vector<EvictCandidate> c = {C(1, 100), C(2, 200)};
    auto e = ChooseEvictions(c, 10000, 99, p);
    CHECK(e.size() == 2);
  }

  // idle_frames 0 disables age eviction; heap_size 0 disables pressure.
  {
    EvictParams q = p;
    q.idle_frames = 0;
    std::vector<EvictCandidate> c = {C(1, 0), C(2, 1)};
    CHECK(ChooseEvictions(c, 1000000, 10, q).empty());
    q.heap_size = 0;
    CHECK(ChooseEvictions(c, 1000000, 1000, q).empty());
  }

  // #43 shape: a long session keeps adding textures; with eviction the views
  // in use never reach the heap size.
  {
    EvictParams s = p;
    s.heap_size = 8192;
    std::vector<EvictCandidate> pool;
    uint32_t in_use = 17;
    uint32_t peak = 0;
    for (uint64_t frame = 0; frame < 263000; ++frame) {
      // ~1 new texture every 30 frames (17 -> 8192 in 252562 frames in the log),
      // and a working set of the newest 300 that keep being drawn.
      if (frame % 30 == 0) {
        pool.push_back(C(uint32_t(pool.size()), frame));
        ++in_use;
      }
      const size_t n = pool.size();
      for (size_t i = n > 300 ? n - 300 : 0; i < n; ++i) pool[i].last_used = frame;
      if (frame % 30 == 0) {
        auto e = ChooseEvictions(pool, frame, in_use, s);
        for (uint32_t id : e) {
          pool[id].resident = false;
          --in_use;
        }
      }
      if (in_use > peak) peak = in_use;
    }
    std::printf("#43 shape: %zu textures created, peak %u views in use of 8192\n", pool.size(), peak);
    CHECK(peak < 8192);
    CHECK(peak <= 8192 * 75 / 100 + 1);
  }

  // A big wave is spread over passes: at most max_per_pass at once.
  {
    EvictParams q = p;
    q.heap_size = 100000;
    q.max_per_pass = 1024;
    std::vector<EvictCandidate> c;
    for (uint32_t i = 0; i < 5000; ++i) c.push_back(C(i, 0));
    auto e = ChooseEvictions(c, 100000, 10, q);
    CHECK(e.size() == 1024);
    CHECK(e.front() == 0);
    q.max_per_pass = 0;  // 0 = unlimited
    CHECK(ChooseEvictions(c, 100000, 10, q).size() == 5000);
  }

  // The bug itself: the same session without eviction (as up to 2.0.11)
  // needs more views than the heap holds.
  {
    uint32_t in_use = 17;
    for (uint64_t frame = 0; frame < 263000; ++frame) {
      if (frame % 30 == 0) ++in_use;
    }
    std::printf("#43 shape without eviction: %u views needed of 8192\n", in_use);
    CHECK(in_use > 8192);
  }

  if (g_failures == 0) std::printf("dp_native_evict_test: all passed\n");
  return g_failures == 0 ? 0 : 1;
}
