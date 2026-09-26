#pragma once
// [new_fix_27092026_health] Native renderer "graphics health" log (user 27.09:
// «зроби більше логу, щоб такі технічні проблеми з графікою виловити»).
//
// Issue #38 showed the gap: a host texture that could not be created at 3x was
// logged ~15 000 times (every frame, five times), but the log never said the
// two facts that explained it - "this texture is recreated five times per
// frame" and "a 3D texture larger than 2048 per side cannot exist in D3D12".
// The helpers here turn such cases into one readable line each, plus counters
// for the periodic statistics line. Pure logic: tests/native_health_test.cpp.

#include <cstdint>
#include <unordered_map>

namespace dp::native {

// D3D12 hard limits (D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION /
// D3D12_REQ_TEXTURE3D_U_V_OR_W_DIMENSION / D3D12_REQ_TEXTURE2D_ARRAY_AXIS_DIMENSION).
inline constexpr uint32_t kD3D12MaxTexture2D = 16384;
inline constexpr uint32_t kD3D12MaxTexture3D = 2048;
inline constexpr uint32_t kD3D12MaxArraySize = 2048;
inline constexpr int32_t kHrOutOfMemory = static_cast<int32_t>(0x8007000E);  // E_OUTOFMEMORY
inline constexpr int32_t kHrInvalidArg = static_cast<int32_t>(0x80070057);   // E_INVALIDARG
inline constexpr int32_t kHrDeviceRemoved = static_cast<int32_t>(0x887A0005);  // DXGI_ERROR_DEVICE_REMOVED

// Why a CreateCommittedResource most likely failed, in words for the log.
inline const char* CreateFailureHint(bool is_3d, uint32_t width, uint32_t height, uint32_t depth_or_array,
                                     int32_t hr) {
  if (is_3d && (width > kD3D12MaxTexture3D || height > kD3D12MaxTexture3D || depth_or_array > kD3D12MaxTexture3D))
    return "larger than the D3D12 3D texture limit (2048 per side)";
  if (!is_3d && (width > kD3D12MaxTexture2D || height > kD3D12MaxTexture2D))
    return "larger than the D3D12 2D texture limit (16384 per side)";
  if (!is_3d && depth_or_array > kD3D12MaxArraySize) return "more array slices than D3D12 allows (2048)";
  if (hr == kHrOutOfMemory) return "out of video memory";
  if (hr == kHrDeviceRemoved) return "the GPU device was lost";
  if (hr == kHrInvalidArg) return "the driver rejected the format / size / flags combination";
  return "unknown reason (see the HRESULT)";
}

// At most `per_key` messages for one key (a guest address), and at most
// `max_keys` distinct keys, so a broken resource cannot flood the log.
class LogLimiter {
 public:
  explicit LogLimiter(uint32_t per_key = 2, uint32_t max_keys = 256) : per_key_(per_key), max_keys_(max_keys) {}
  // true = log this one; `last` is set when it is the last message for the key.
  bool Allow(uint64_t key, bool* last = nullptr) {
    auto it = counts_.find(key);
    if (it == counts_.end()) {
      if (counts_.size() >= max_keys_) {
        ++suppressed_;
        return false;
      }
      it = counts_.emplace(key, 0u).first;
    }
    if (it->second >= per_key_) {
      ++suppressed_;
      return false;
    }
    ++it->second;
    if (last) *last = it->second == per_key_;
    return true;
  }
  uint64_t suppressed() const { return suppressed_; }

 private:
  uint32_t per_key_, max_keys_;
  uint64_t suppressed_ = 0;
  std::unordered_map<uint64_t, uint32_t> counts_;
};

// A resource that is destroyed and created again over and over (the #38
// pattern: five times per frame) is a bug, never a workload. Reports a key
// once, the first time it is recreated `per_frame` times within one frame or
// `per_window` times within `window` frames.
class ChurnDetector {
 public:
  ChurnDetector(uint32_t per_frame = 3, uint32_t per_window = 10, uint32_t window = 60)
      : per_frame_(per_frame), per_window_(per_window), window_(window) {}
  // true exactly once per key: when it first crosses a threshold.
  bool Note(uint64_t key, uint32_t frame) {
    Entry& e = entries_[key];
    if (e.reported) return false;
    if (e.frame != frame) {
      e.frame = frame;
      e.in_frame = 0;
    }
    if (e.window_start == UINT32_MAX || frame - e.window_start >= window_) {
      e.window_start = frame;
      e.in_window = 0;
    }
    ++e.in_frame;
    ++e.in_window;
    if (e.in_frame >= per_frame_ || e.in_window >= per_window_) {
      e.reported = true;
      last_in_frame_ = e.in_frame;
      last_in_window_ = e.in_window;
      return true;
    }
    return false;
  }
  uint32_t last_in_frame() const { return last_in_frame_; }
  uint32_t last_in_window() const { return last_in_window_; }

 private:
  struct Entry {
    uint32_t frame = UINT32_MAX, in_frame = 0;
    uint32_t window_start = UINT32_MAX, in_window = 0;
    bool reported = false;
  };
  uint32_t per_frame_, per_window_, window_;
  uint32_t last_in_frame_ = 0, last_in_window_ = 0;
  std::unordered_map<uint64_t, Entry> entries_;
};

// Counters for the statistics line (reset by each summary).
struct HealthCounters {
  uint32_t create_failures = 0;   // host resources that could not be created
  uint32_t fallbacks_1x = 0;      // ... and were created again at 1x
  uint32_t scale_capped = 0;      // 3D textures kept below the D3D12 limit (lower scale than asked)
  uint32_t recreated = 0;         // resolve destinations destroyed and created again
  uint32_t null_binds = 0;        // typed fetch constants with no registered texture (sampled as zeros)
  uint32_t upload_failures = 0;   // guest textures that failed to upload (draw sampled zeros)
  bool any() const {
    return create_failures || fallbacks_1x || scale_capped || recreated || null_binds || upload_failures;
  }
};

}  // namespace dp::native
