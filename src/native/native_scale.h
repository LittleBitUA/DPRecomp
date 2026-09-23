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

// [NEW FABLE VERSION] 2026-09-23: XDK D3DDevice_Resolve flag bits. 0x70 is
// the fragment (MSAA sample) select the XDK fills in itself (0x10 / 0x50 /
// 0x70 for 1x / 2x / 4x), the clears are 0x100 / 0x200. DP1 passes only 0x00,
// 0x10 and 0x14, i.e. it never clears on resolve.
constexpr uint32_t kResolveDepthStencil = 0x4;
constexpr uint32_t kResolveClearTarget = 0x100;
constexpr uint32_t kResolveClearDepthStencil = 0x200;
constexpr bool ResolveIsDepth(uint32_t flags) { return (flags & kResolveDepthStencil) != 0; }
constexpr bool ResolveClearsTarget(uint32_t flags) { return (flags & kResolveClearTarget) != 0; }
constexpr bool ResolveClearsDepth(uint32_t flags) { return (flags & kResolveClearDepthStencil) != 0; }

// [NEW FABLE VERSION] 2026-09-23: guest D3DFORMAT of an EDRAM surface (texture
// format = bits 0-5, numbers from rexglue-sdk include/rex/graphics/xenos.h
// TextureFormat) -> the host format class. 31 is k_16_16_FLOAT and 32 is
// k_16_16_16_16_FLOAT: DP1's post chain (half/quarter copies, glow, DoF) uses
// 32 (D3DFORMAT 0x1A22AB60), which used to fall through to RGBA8 while 31 was
// taken for it, so the whole HDR post chain ran in 8-bit UNORM.
enum class SurfaceClass : uint8_t { kRgba8, kRgba16F, kRg16F, kR32F, kR8, kDepth24, kDepthF24 };
constexpr SurfaceClass ClassifySurfaceFormat(uint32_t guest_format) {
  switch (guest_format & 0x3F) {
    case 22: return SurfaceClass::kDepth24;   // k_24_8 (D24S8)
    case 23: return SurfaceClass::kDepthF24;  // k_24_8_FLOAT (D24FS8)
    case 26:                                  // k_16_16_16_16 (fixed -32..32: a float superset)
    case 29:                                  // k_16_16_16_16_EXPAND
    case 32:                                  // k_16_16_16_16_FLOAT
    case 63:                                  // k_2_10_10_10_FLOAT_EDRAM (7e3): float superset
      return SurfaceClass::kRgba16F;
    case 31: return SurfaceClass::kRg16F;     // k_16_16_FLOAT
    case 36: return SurfaceClass::kR32F;      // k_32_FLOAT
    case 2: return SurfaceClass::kR8;         // k_8
    default: return SurfaceClass::kRgba8;     // k_8_8_8_8 (6) and anything unmapped
  }
}
constexpr bool IsKnownSurfaceFormat(uint32_t guest_format) {
  switch (guest_format & 0x3F) {
    case 2: case 6: case 22: case 23: case 26: case 29: case 31: case 32: case 36: case 63: return true;
    default: return false;
  }
}
// 64 bits per pixel in EDRAM (a tile then holds 40x16 pixels instead of 80x16).
constexpr bool SurfaceIs64bpp(uint32_t guest_format) {
  switch (guest_format & 0x3F) {
    case 21: case 26: case 29: case 32: case 37: return true;  // 16_16_16_16 variants, k_32_32_FLOAT
    default: return false;
  }
}

// [NEW FABLE VERSION] 2026-09-23: EDRAM geometry. 2048 tiles of 80x16 samples
// at 32 bpp; a surface at `base` covers pitch x rows tiles from there (1x MSAA,
// which is all DP1 uses).
inline constexpr uint32_t kEdramTiles = 2048;
constexpr uint32_t EdramTileSpan(uint32_t width, uint32_t height, bool bpp64) {
  return (((bpp64 ? width * 2 : width) + 79) / 80) * ((height + 15) / 16);
}
constexpr bool EdramOverlap(uint32_t base_a, uint32_t span_a, uint32_t base_b, uint32_t span_b) {
  return base_a < base_b + span_b && base_b < base_a + span_a;
}
// What re-binding surface `dst` means for its content when `src` wrote the same
// tiles last. Only an exact re-bind (same base, size and host scale) maps pixel
// to pixel. Same texture format: the tiles' bits, a copy. 7e3 -> 8888: the
// console would read raw 7e3 bits; the emulator (and so the look players know)
// converts to saturate(value), which is what DP1's tone map needs where the
// scene's alpha is below 1. 8888 -> 7e3 is a bit reinterpretation on the
// console that DP1 only follows with full overdraw (the sky fill): left alone.
enum class EdramTransfer : uint8_t { kNone, kCopy, kSaturate7e3 };
constexpr EdramTransfer EdramTransferKind(uint32_t src_format, uint32_t dst_format, bool same_geometry) {
  if (!same_geometry) return EdramTransfer::kNone;
  const uint32_t s = src_format & 0x3F, d = dst_format & 0x3F;
  if (s == d) return EdramTransfer::kCopy;
  if (s == 63 && d == 6) return EdramTransfer::kSaturate7e3;
  return EdramTransfer::kNone;
}

// [NEW FABLE VERSION] 2026-09-24: guest memory holds one content, the newest
// writer's. A texture view samples the resolve that wrote its memory when that
// resolve is newer than the last CPU write of it we saw and the two views agree
// on size and format (dp_native_resolve_alias).
constexpr bool ResolveIsNewestWriter(uint64_t view_cpu_seq, uint64_t resolve_seq, bool same_shape) {
  return same_shape && resolve_seq > view_cpu_seq;
}

}  // namespace dp::native
