// [new_fix_24092026] 2.0.3: how long a host object the renderer stops using must
// stay alive (GitHub issues #33 and #34: DEVICE_HUNG with a DRED page fault on a
// recently freed, unnamed resource = a vertex or index buffer).
//
// Up to 2.0.2 a retired resource went into the list of the frame slot current
// at that moment and was released when that slot came round again, after the
// wait for the frame that last used the slot. That is only right while a frame
// is being recorded. The guest releases buffers between frames too (after Swap
// submitted frame N, before the first draw of N+1) and from its loading
// threads: the resource then went into the NEXT slot, whose wait covers frame
// N-1 only, and was released while frame N, which still draws with it, could be
// running on the GPU.
//
// Now every retired object carries the fence value of the frame being recorded
// (between frames: of the next frame), FenceCounter::ForRetire. Every command list that can reference
// the object was recorded before the retire, so it signals that value or an
// earlier one on the same queue: once the queue's fence reaches the value,
// nothing on the GPU uses the object any more. Shader-visible descriptors wait
// the same way (a slot must not be rewritten while a submitted frame reads it).
//
// Header-only and free of D3D so tests/native_retire_test.cpp can check it.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <utility>
#include <vector>

namespace dp::native {

// The frame fence values. One writer (the recording thread, at submit), readers
// on any thread (a retire). Submit signals the current value and only then
// moves on, so a retire racing a submit reads the value being signalled or the
// next one: both cover every frame recorded before the retire.
class FenceCounter {
 public:
  // The value the frame being recorded (between frames: the next frame) will
  // signal. What is retired now waits for it.
  uint64_t ForRetire() const { return next_.load(std::memory_order_acquire); }
  // Submits the frame being recorded: `signal(value)` queues the GPU signal.
  template <typename Signal>
  uint64_t Submit(Signal&& signal) {
    const uint64_t value = next_.load(std::memory_order_relaxed);
    signal(value);
    next_.store(value + 1, std::memory_order_release);
    return value;
  }
  uint64_t LastSubmitted() const { return next_.load(std::memory_order_acquire) - 1; }

 private:
  std::atomic<uint64_t> next_{1};
};

// Thread-safe: the render thread retires and drains, the guest's loading
// threads retire (D3DResource_Release).
template <typename T>
class RetireQueue {
 public:
  // `fence`: the value the frame being recorded (or, between frames, the next
  // frame) will signal.
  void Push(uint64_t fence, T item) {
    std::lock_guard<std::mutex> lock(mutex_);
    items_.emplace_back(fence, std::move(item));
  }

  // Moves out every item whose fence `completed` has reached. Pushes from
  // several threads can arrive out of fence order, so the whole list is
  // scanned (it holds a few frames' worth of objects). A removed device reports
  // UINT64_MAX as its completed value: everything goes.
  std::vector<T> TakeCompleted(uint64_t completed) {
    std::vector<T> done;
    std::lock_guard<std::mutex> lock(mutex_);
    // Reserved up front: once an item is moved out, nothing below may throw
    // (a half-moved list would hand the same descriptor index out twice).
    done.reserve(items_.size());
    size_t keep = 0;
    for (size_t i = 0; i < items_.size(); ++i) {
      if (items_[i].first <= completed) {
        done.push_back(std::move(items_[i].second));
      } else {
        if (keep != i) items_[keep] = std::move(items_[i]);
        ++keep;
      }
    }
    items_.erase(items_.begin() + ptrdiff_t(keep), items_.end());
    return done;
  }

  size_t size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return items_.size();
  }

 private:
  mutable std::mutex mutex_;
  std::vector<std::pair<uint64_t, T>> items_;
};

}  // namespace dp::native
