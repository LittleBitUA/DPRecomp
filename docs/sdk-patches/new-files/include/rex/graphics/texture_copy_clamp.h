// DP1 2026-09-13 (DPRecomp #22): pure math behind the CopyTextureRegion
// guard in the D3D12 texture cache. Kept free of d3d12.h (only the DXGI
// format enum) so it can be unit tested: a copy whose source box is larger
// than the destination mip level is an invalid call that removes the device,
// but for block-compressed formats the level is addressed in whole blocks (a
// 4x4 BC block legally lands in a 2x2 or 1x1 level), and a clamped box must
// stay block-aligned or the copy is just as invalid (1.3.5 regression, log 4
// of #22).
#pragma once

#include <algorithm>
#include <cstdint>

#include <dxgiformat.h>

#include <rex/graphics/host_texture_extent.h>

namespace rex::gpu::texture_copy {

struct Box {
  uint32_t left, top, front, right, bottom, back;
  uint32_t Width() const { return right - left; }
  uint32_t Height() const { return bottom - top; }
  uint32_t Depth() const { return back - front; }
};

struct Extent {
  uint32_t width, height, depth;
};

// Block size of a DXGI format as D3D12 addresses copies: BC1..BC7 are 4x4
// blocks, the packed 4:2:2 formats (R8G8_B8G8, G8R8_G8B8, YUY2) are 2x1,
// everything else is 1x1. Named enum values, not numbers: BC5_SNORM is 84 and
// B5G6R5_UNORM (a format this cache creates for 16-bit guest textures) is 85.
inline void BlockSizeForDxgiFormat(DXGI_FORMAT dxgi_format, uint32_t& block_width,
                                   uint32_t& block_height) {
  if ((dxgi_format >= DXGI_FORMAT_BC1_TYPELESS && dxgi_format <= DXGI_FORMAT_BC5_SNORM) ||
      (dxgi_format >= DXGI_FORMAT_BC6H_TYPELESS && dxgi_format <= DXGI_FORMAT_BC7_UNORM_SRGB)) {
    block_width = block_height = 4;
    return;
  }
  if (dxgi_format == DXGI_FORMAT_R8G8_B8G8_UNORM || dxgi_format == DXGI_FORMAT_G8R8_G8B8_UNORM ||
      dxgi_format == DXGI_FORMAT_YUY2) {
    block_width = 2;
    block_height = 1;
    return;
  }
  block_width = block_height = 1;
}

inline uint32_t AlignUp(uint32_t value, uint32_t alignment) {
  return (value + alignment - 1) / alignment * alignment;
}

// Mip level of a subresource index of a non-planar texture (index = mip +
// slice * mip_levels). mip_levels == 0 cannot come back from GetDesc() of a
// created resource; treated as a single level.
inline uint32_t MipFromSubresource(uint32_t subresource, uint32_t mip_levels) {
  return mip_levels ? subresource % mip_levels : 0;
}

// Extent of a mip level of a resource, the way D3D12 addresses it: the
// texel size shifted down by the mip, never below one, and for block formats
// rounded up to whole blocks (the last partial block is still a full block).
inline Extent LevelExtent(uint32_t width, uint32_t height, uint32_t depth, uint32_t mip,
                          bool is_3d, uint32_t block_width, uint32_t block_height) {
  Extent e;
  e.width = AlignUp(std::max(width >> mip, uint32_t(1)), block_width);
  e.height = AlignUp(std::max(height >> mip, uint32_t(1)), block_height);
  e.depth = is_3d ? std::max(depth >> mip, uint32_t(1)) : 1;
  return e;
}

// True when the box does not fit in the level (and the copy would remove the
// device); the box is then shrunk in place to the level extent. The block
// extent is already whole blocks, so a box that was block-aligned stays
// block-aligned after clamping. A box that fits is left untouched.
inline bool ClampBoxToLevel(Box& box, const Extent& level) {
  const uint32_t w = box.Width(), h = box.Height(), d = box.Depth();
  if (w <= level.width && h <= level.height && d <= level.depth) {
    return false;
  }
  box.right = box.left + std::min(w, level.width);
  box.bottom = box.top + std::min(h, level.height);
  box.back = box.front + std::min(d, level.depth);
  return true;
}

// Dedupe key for "report once per texture and level" logging: the level is
// mixed in with a multiplier so two hashes differing only in their top byte
// cannot collide across levels.
inline uint64_t ReportKey(uint64_t texture_hash, uint32_t level) {
  return texture_hash ^ (uint64_t(level) + 1) * UINT64_C(0x9E3779B97F4A7C15);
}

}  // namespace rex::gpu::texture_copy
