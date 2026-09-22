// [NEW FABLE VERSION] 2026-09-22
// Deadly Premonition Recompilation - native renderer: internal resolution rules.
//
// The native renderer can allocate the game's render targets (and the textures
// it resolves them into) larger than the console's, the way DPfix raises
// renderWidth/shadowMapScale/reflectionScale on the PC port. Everything the
// game's shaders do with those targets is expressed in normalized UVs or in
// screen-space constants it uploads itself, so the picture keeps its meaning
// and only the sampling rate changes.
//
// The decisions live here as pure functions so they can be tested without a
// GPU (tests/native_scale_test.cpp, target dp_native_scale_test).
#pragma once

#include <cstdint>

namespace dp::native {

// Highest multiplier the renderer accepts (4x of 1280x720 = 5120x2880).
inline constexpr int32_t kMaxScale = 4;

// Render targets smaller than this in either dimension are not images but
// fixed-function chains: the 256x64 -> 32x32 -> 1x1 luminance ladder that feeds
// the tonemap's auto exposure, and a few small masks. Scaling them would change
// the averages they carry, so they stay at the console's size; every consumer
// reads them through normalized UVs and cannot tell.
inline constexpr uint32_t kScaleMinExtent = 128;

// dp_native_scale as written in the toml -> a usable multiplier.
inline uint32_t ClampScale(int32_t value) {
  if (value < 1) return 1u;
  if (value > kMaxScale) return uint32_t(kMaxScale);
  return uint32_t(value);
}

// The multiplier a target of this guest size is allocated with.
inline uint32_t ScaleForTarget(uint32_t width, uint32_t height, uint32_t scale) {
  return (width >= kScaleMinExtent && height >= kScaleMinExtent) ? scale : 1u;
}

// The multiplier the rasterizer works in when a colour and a depth target are
// bound together. They normally match; if one of them fell back to 1x (out of
// video memory) the smaller one decides, so nothing is ever rasterized outside
// the smaller extent.
inline uint32_t BindScale(bool has_rt, uint32_t rt_scale, bool has_ds, uint32_t ds_scale) {
  if (has_rt && has_ds) return rt_scale < ds_scale ? rt_scale : ds_scale;
  if (has_rt) return rt_scale;
  if (has_ds) return ds_scale;
  return 1u;
}

// Guest viewport rectangle (console pixels) -> host pixels.
struct ViewportRect {
  float left, top, width, height;
  bool operator==(const ViewportRect& o) const {
    return left == o.left && top == o.top && width == o.width && height == o.height;
  }
};
inline ViewportRect ScaleViewport(const ViewportRect& vp, uint32_t scale) {
  const float s = float(scale);
  return {vp.left * s, vp.top * s, vp.width * s, vp.height * s};
}

// Guest scissor rectangle (console pixels) -> host pixels.
struct ScissorRect {
  int32_t left, top, right, bottom;
  bool operator==(const ScissorRect& o) const {
    return left == o.left && top == o.top && right == o.right && bottom == o.bottom;
  }
};
inline ScissorRect ScaleScissor(const ScissorRect& r, uint32_t scale) {
  if (scale == 1) return r;
  const int32_t s = int32_t(scale);
  return {r.left * s, r.top * s, r.right * s, r.bottom * s};
}

// Video memory the scaled colour targets of one frame need, in bytes, for the
// launcher's and the log's benefit: the scene (1024x576) twice (7e3 + the 8888
// alias), the 1280x720 output, the 512x288 reflection and its depth, and the
// 1024x1024 shadow cascade set. Rough by design - it exists to warn, not to
// budget.
inline uint64_t EstimateScaledBytes(uint32_t scale) {
  const uint64_t s2 = uint64_t(scale) * scale;
  const uint64_t scene = 1024ull * 576 * 8 * 2;   // RGBA16F x2
  const uint64_t output = 1280ull * 720 * 4;      // 8888
  const uint64_t depth = 1280ull * 720 * 4;       // D32/D24S8
  const uint64_t reflection = 512ull * 288 * 8 + 512ull * 288 * 4;
  const uint64_t shadows = 1024ull * 1024 * 4 * 6;  // 5 cascades + the 512 set, as R32F
  return (scene + output + depth + reflection + shadows) * s2;
}

}  // namespace dp::native
