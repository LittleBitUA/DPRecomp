// [new_fix_24092026]
// Tests for the GPU lifetime of retired host objects (src/native/native_retire.h,
// GitHub issues #33 and #34). Plain executable, exit code 0 = pass. Build target
// dp_native_retire_test.
//
// Besides the queue itself, a model of the renderer's frame loop (BeginFrame /
// SubmitFrame in native_renderer.cpp, two frame slots) checks both lifetime
// policies against a GPU that runs behind the CPU: the 2.0.2 per-slot lists
// (kept here only as the model of the bug) must free an object while a frame
// that uses it is still unfinished, and the fence-tagged queue never may. The
// model takes its fence values from the renderer's own FenceCounter, so a wrong
// retire tag in native_retire.h fails here too.
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <memory>
#include <random>
#include <set>
#include <thread>
#include <unordered_map>
#include <vector>

#include "../src/native/native_retire.h"

namespace {

int g_failures = 0;

#define CHECK(cond)                                               \
  do {                                                            \
    if (!(cond)) {                                                \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      ++g_failures;                                               \
    }                                                             \
  } while (0)

using dp::native::FenceCounter;
using dp::native::RetireQueue;

void TestQueueBasics() {
  RetireQueue<int> q;
  CHECK(q.TakeCompleted(100).empty());
  q.Push(5, 1);
  CHECK(q.TakeCompleted(4).empty());
  CHECK(q.size() == 1);
  const auto a = q.TakeCompleted(5);
  CHECK(a.size() == 1 && a[0] == 1);
  CHECK(q.size() == 0);
  // Out of fence order (two threads pushing): each leaves when its own fence is reached.
  q.Push(7, 70);
  q.Push(5, 50);
  q.Push(6, 60);
  const auto b = q.TakeCompleted(5);
  CHECK(b.size() == 1 && b[0] == 50);
  const auto c = q.TakeCompleted(6);
  CHECK(c.size() == 1 && c[0] == 60);
  CHECK(q.size() == 1);
  // A removed device reports UINT64_MAX: everything goes.
  q.Push(1000000, 80);
  CHECK(q.TakeCompleted(UINT64_MAX).size() == 2);
  CHECK(q.size() == 0);
}

// Move-only payloads (the renderer's items hold a ComPtr).
void TestMoveOnly() {
  RetireQueue<std::unique_ptr<int>> q;
  q.Push(2, std::make_unique<int>(2));
  q.Push(1, std::make_unique<int>(1));
  q.Push(3, std::make_unique<int>(3));
  auto d = q.TakeCompleted(2);
  CHECK(d.size() == 2);
  int sum = 0;
  for (auto& p : d) sum += p ? *p : 100;
  CHECK(sum == 3);
  auto e = q.TakeCompleted(3);
  CHECK(e.size() == 1 && e[0] && *e[0] == 3);
}

void TestFenceCounter() {
  FenceCounter f;
  CHECK(f.ForRetire() == 1);  // the first frame signals 1
  uint64_t signalled = 0, seen_inside = 0;
  const uint64_t v = f.Submit([&](uint64_t value) {
    signalled = value;
    seen_inside = f.ForRetire();  // a retire racing the signal: still covers this frame
  });
  CHECK(v == 1 && signalled == 1 && seen_inside == 1);
  CHECK(f.ForRetire() == 2 && f.LastSubmitted() == 1);
}

// Several threads retire while one submits and drains: every item comes out
// exactly once (no loss, no double free of a descriptor index).
void TestThreads() {
  RetireQueue<uint32_t> q;
  FenceCounter fences;
  std::atomic<bool> stop{false};
  constexpr int kThreads = 4, kPerThread = 20000;
  std::vector<std::thread> pushers;
  for (int t = 0; t < kThreads; ++t) {
    pushers.emplace_back([&, t] {
      for (int i = 0; i < kPerThread; ++i) q.Push(fences.ForRetire(), uint32_t(t * kPerThread + i));
    });
  }
  std::set<uint32_t> seen;
  bool dup = false;
  std::thread drainer([&] {
    // After the pushers stop, keep submitting frames until everything is out.
    while (!stop.load() || q.size()) {
      const uint64_t f = fences.Submit([](uint64_t) {});  // a frame is submitted
      for (uint32_t id : q.TakeCompleted(f - 1)) {        // the GPU finished the one before
        if (!seen.insert(id).second) dup = true;
      }
    }
  });
  for (auto& t : pushers) t.join();
  stop = true;
  drainer.join();
  CHECK(!dup);
  CHECK(seen.size() == size_t(kThreads * kPerThread));
  CHECK(q.size() == 0);
}

// ---------------------------------------------------------------------------
// The frame loop model. Objects are vertex/index buffers: a draw of the frame
// being recorded binds them (last use = that frame's fence), the guest
// releases them during a frame or between frames. The GPU finishes submitted
// frames in order, at its own pace; BeginFrame waits for the frame that last
// used the slot, like the renderer.
enum class Policy { kSlotLists202, kFenceQueue203 };

struct FrameModel {
  static constexpr uint32_t kSlots = 2;
  Policy policy;
  FenceCounter fences;     // the renderer's own counter (native_retire.h): the retire tag
  uint64_t submitted = 0;  // the model's own count: frame k signals k (independent of the counter)
  uint64_t completed = 0;
  uint64_t slot_fence[kSlots] = {};
  uint32_t slot = 0;
  bool recording = false;
  std::deque<uint64_t> in_flight;
  std::unordered_map<uint32_t, uint64_t> last_use;  // live and retired objects
  std::vector<uint32_t> live;
  std::vector<uint32_t> slot_lists[kSlots];  // 2.0.2
  RetireQueue<uint32_t> queue;               // 2.0.3
  uint32_t next_id = 1;
  int use_after_free = 0, freed = 0, retired = 0;

  explicit FrameModel(Policy p) : policy(p) {}

  void GpuFinishOne() {
    if (in_flight.empty()) return;
    completed = in_flight.front();
    in_flight.pop_front();
  }
  void Free(uint32_t id) {
    ++freed;
    if (last_use[id] > completed) ++use_after_free;  // a submitted frame still reads it
    last_use.erase(id);
  }
  void BeginFrame() {
    if (recording) return;
    while (completed < slot_fence[slot]) GpuFinishOne();  // WaitForSingleObject
    if (policy == Policy::kSlotLists202) {
      for (uint32_t id : slot_lists[slot]) Free(id);
      slot_lists[slot].clear();
    } else {
      for (uint32_t id : queue.TakeCompleted(completed)) Free(id);
    }
    recording = true;
  }
  void Draw(std::mt19937& rng) {
    BeginFrame();
    if (live.empty() || rng() % 4 == 0) {
      live.push_back(next_id);
      last_use[next_id++] = 0;
    }
    last_use[live[rng() % live.size()]] = submitted + 1;  // bound by this frame (it will signal submitted + 1)
  }
  void SubmitFrame() {
    if (!recording) return;
    slot_fence[slot] = fences.Submit([&](uint64_t value) { in_flight.push_back(value); });
    ++submitted;
    slot = (slot + 1) % kSlots;
    recording = false;
  }
  void Release(std::mt19937& rng) {  // D3DResource_Release of a buffer
    if (live.empty()) return;
    const size_t i = rng() % live.size();
    const uint32_t id = live[i];
    live[i] = live.back();
    live.pop_back();
    ++retired;
    if (policy == Policy::kSlotLists202) {
      slot_lists[slot].push_back(id);  // g.retired[g.frame_index]
    } else {
      queue.Push(fences.ForRetire(), id);  // RetireResource: RetireFence()
    }
  }
  void Finish() {
    SubmitFrame();
    while (!in_flight.empty()) GpuFinishOne();
    BeginFrame();  // one more frame start drains what is complete
    SubmitFrame();
    while (!in_flight.empty()) GpuFinishOne();
    BeginFrame();
  }
};

// Issue #34 in five steps: frame 1 draws with buffer B, Swap submits it, the
// guest releases B before the first draw of frame 2, the GPU has not finished
// frame 1 when frame 2 starts.
void TestIssue34Sequence(Policy policy, bool expect_uaf) {
  FrameModel m(policy);
  std::mt19937 rng(1);
  m.Draw(rng);                 // frame 1 binds the only object (id 1)
  CHECK(m.last_use[1] == 1);
  m.SubmitFrame();             // Swap; the GPU has not run it yet
  m.Release(rng);              // between frames
  m.Draw(rng);                 // frame 2 starts: slot 1 was never used, no wait
  CHECK(m.completed == 0);
  CHECK((m.use_after_free > 0) == expect_uaf);
  m.Finish();
  CHECK(m.freed == 1);         // and it is freed in the end, not leaked
  CHECK(m.last_use.size() == m.live.size());
}

struct Totals {
  int uaf = 0, freed = 0, retired = 0, leaked = 0;
};

// Random interleavings: draws, Swaps, releases during and between frames, a
// GPU that runs up to several frames behind (the wait at BeginFrame bounds it).
Totals RunRandom(Policy policy, uint32_t seed) {
  FrameModel m(policy);
  std::mt19937 rng(seed);
  for (int frame = 0; frame < 400; ++frame) {
    const int draws = 1 + int(rng() % 12);
    for (int d = 0; d < draws; ++d) {
      m.Draw(rng);
      if (rng() % 7 == 0) m.Release(rng);     // during the frame
      if (rng() % 5 == 0) m.GpuFinishOne();
    }
    m.SubmitFrame();
    const int idle = int(rng() % 4);
    for (int i = 0; i < idle; ++i) {
      if (rng() % 2) m.Release(rng);          // between frames (loading threads, frame start)
      if (rng() % 3 == 0) m.GpuFinishOne();
    }
  }
  m.Finish();
  Totals t;
  t.uaf = m.use_after_free;
  t.freed = m.freed;
  t.retired = m.retired;
  t.leaked = m.retired - m.freed;
  return t;
}

void TestRandom() {
  Totals old202, new203;
  for (uint32_t seed = 1; seed <= 300; ++seed) {
    const Totals a = RunRandom(Policy::kSlotLists202, seed);
    const Totals b = RunRandom(Policy::kFenceQueue203, seed);
    old202.uaf += a.uaf;
    old202.freed += a.freed;
    new203.uaf += b.uaf;
    new203.freed += b.freed;
    new203.retired += b.retired;
    new203.leaked += b.leaked;
  }
  std::printf("model: 2.0.2 policy %d of %d frees while a submitted frame still used the object; "
              "2.0.3 %d of %d (leaked %d)\n",
              old202.uaf, old202.freed, new203.uaf, new203.freed, new203.leaked);
  CHECK(old202.uaf > 0);   // the model reproduces the 2.0.2 bug
  CHECK(new203.uaf == 0);  // the fix never frees early
  CHECK(new203.leaked == 0 && new203.freed == new203.retired);
}

}  // namespace

int main() {
  TestQueueBasics();
  TestMoveOnly();
  TestFenceCounter();
  TestThreads();
  TestIssue34Sequence(Policy::kSlotLists202, true);
  TestIssue34Sequence(Policy::kFenceQueue203, false);
  TestRandom();
  if (g_failures) {
    std::printf("%d check(s) failed\n", g_failures);
    return 1;
  }
  std::printf("native_retire_test: all passed\n");
  return 0;
}
