// [new_fix_03102026_evict] Host texture eviction (DPRecomp #37, #43).
//
// Host textures were created once per fetch key and never destroyed: the
// shader view heap (8192 slots) filled in 73-80 minutes of play (views 17 ->
// 8192, then "shader view descriptor heap full, the object is not drawn":
// buildings, the map and York himself stopped drawing) and VRAM grew with it
// (#43: 624 -> 1526 MB). The GuestTexture records stay (raw pointers to them
// live in several maps); eviction only drops the host copy (resource + view,
// through the fence retire queue) and marks the texture dirty, so its next
// use creates and uploads it again from guest memory.
//
// Resolve destinations are never evicted: their content exists only on the
// GPU. This header is the pure policy, unit-tested in
// tests/native_evict_test.cpp.
#pragma once

#include <algorithm>
#include <cstdint>
#include <vector>

namespace dp::native {

struct EvictParams {
  uint32_t heap_size = 0;      // shader view slots
  uint32_t high_percent = 75;  // pressure: start evicting above this share of the heap
  uint32_t low_percent = 50;   // ... down to this share
  uint64_t idle_frames = 3600;      // always evict textures unused this long (0 = never by age)
  uint64_t min_idle_frames = 8;     // never evict anything used this recently (frames in flight)
  // Critic pass: spreads a big wave (the first age pass after a scene change)
  // over several passes, and bounds what a pressure pass can do at once.
  uint32_t max_per_pass = 1024;
};

struct EvictCandidate {
  uint32_t id = 0;            // caller's handle
  uint64_t last_used = 0;     // frame number of the last draw that sampled it
  bool resident = false;      // has a host resource and view
  bool pinned = false;        // resolve destination (GPU-only content)
};

// Which resident, unpinned candidates to drop this pass: everything idle for
// idle_frames, then, while more than high_percent of the heap is in use, the
// least recently used until low_percent is reached. One view per texture.
inline std::vector<uint32_t> ChooseEvictions(const std::vector<EvictCandidate>& candidates, uint64_t now_frame,
                                             uint32_t views_in_use, const EvictParams& p) {
  std::vector<const EvictCandidate*> eligible;
  eligible.reserve(candidates.size());
  for (const EvictCandidate& c : candidates) {
    if (!c.resident || c.pinned) continue;
    const uint64_t idle = now_frame >= c.last_used ? now_frame - c.last_used : 0;
    if (idle < p.min_idle_frames) continue;
    eligible.push_back(&c);
  }
  std::sort(eligible.begin(), eligible.end(), [](const EvictCandidate* a, const EvictCandidate* b) {
    if (a->last_used != b->last_used) return a->last_used < b->last_used;
    return a->id < b->id;
  });
  const uint64_t high = uint64_t(p.heap_size) * p.high_percent / 100;
  const uint64_t low = uint64_t(p.heap_size) * p.low_percent / 100;
  const bool pressure = p.heap_size != 0 && views_in_use > high;
  uint64_t in_use = views_in_use;
  std::vector<uint32_t> out;
  for (const EvictCandidate* c : eligible) {
    const uint64_t idle = now_frame - c->last_used;
    const bool by_age = p.idle_frames != 0 && idle >= p.idle_frames;
    const bool by_pressure = pressure && in_use > low;
    if (!by_age && !by_pressure) {
      // Sorted oldest first: nothing later is older, and pressure is relieved.
      break;
    }
    if (p.max_per_pass && out.size() >= p.max_per_pass) break;
    out.push_back(c->id);
    if (in_use) --in_use;
  }
  return out;
}

}  // namespace dp::native
