// DP1 (DPRecomp #22): regression tests for the CopyTextureRegion guard math.
// Log 3: a 64x64 R8G8B8A8 copy into a 32x32 level removed the device.
// Log 4: the 1.3.5 guard clamped a legal 4x4 BC3 block copy into a 2x2 level
// down to 2x2, which is unaligned for BC3 and removed the device instead.
// Critic pass on 1.3.6: a numeric BC range 70..89 was five formats too wide
// (BC5_SNORM is 84; 85..89 are B5G6R5, B5G5R5A1, B8G8R8A8, B8G8R8X8,
// R10G10B10_XR_BIAS_A2, all 1x1, two of which this cache creates).
#include <algorithm>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <rex/graphics/texture_copy_clamp.h>

using namespace rex::gpu::texture_copy;

namespace {
Box FullBox(uint32_t w, uint32_t h, uint32_t d = 1) { return Box{0, 0, 0, w, h, d}; }
void ExpectBlock(DXGI_FORMAT f, uint32_t w, uint32_t h) {
  uint32_t bw = 0, bh = 0;
  BlockSizeForDxgiFormat(f, bw, bh);
  INFO("format " << int(f));
  CHECK(bw == w);
  CHECK(bh == h);
}
}  // namespace

TEST_CASE("texture_copy_clamp: block sizes by DXGI format", "[texture_copy_clamp]") {
  // Every BC format, both ranges, first and last of each.
  for (int f = DXGI_FORMAT_BC1_TYPELESS; f <= DXGI_FORMAT_BC5_SNORM; ++f) {
    ExpectBlock(DXGI_FORMAT(f), 4, 4);
  }
  for (int f = DXGI_FORMAT_BC6H_TYPELESS; f <= DXGI_FORMAT_BC7_UNORM_SRGB; ++f) {
    ExpectBlock(DXGI_FORMAT(f), 4, 4);
  }
  CHECK(int(DXGI_FORMAT_BC5_SNORM) == 84);
  CHECK(int(DXGI_FORMAT_BC3_UNORM) == 77);  // the format in the #22 log-4 line
  // The formats right after BC5 are plain 16/32-bit formats; the cache creates
  // B5G6R5 and B5G5R5A1 for k_5_6_5 / k_1_5_5_5 guest textures.
  ExpectBlock(DXGI_FORMAT_B5G6R5_UNORM, 1, 1);
  ExpectBlock(DXGI_FORMAT_B5G5R5A1_UNORM, 1, 1);
  ExpectBlock(DXGI_FORMAT_B8G8R8A8_UNORM, 1, 1);
  ExpectBlock(DXGI_FORMAT_B8G8R8X8_UNORM, 1, 1);
  ExpectBlock(DXGI_FORMAT_R10G10B10_XR_BIAS_A2_UNORM, 1, 1);
  // Between the two BC ranges.
  for (int f = DXGI_FORMAT_B8G8R8A8_TYPELESS; f <= DXGI_FORMAT_B8G8R8X8_UNORM_SRGB; ++f) {
    ExpectBlock(DXGI_FORMAT(f), 1, 1);
  }
  // Packed 4:2:2.
  ExpectBlock(DXGI_FORMAT_R8G8_B8G8_UNORM, 2, 1);
  ExpectBlock(DXGI_FORMAT_G8R8_G8B8_UNORM, 2, 1);
  ExpectBlock(DXGI_FORMAT_YUY2, 2, 1);
  // Plain formats used by the guard's callers and the edges of the enum.
  ExpectBlock(DXGI_FORMAT_R8G8B8A8_TYPELESS, 1, 1);
  ExpectBlock(DXGI_FORMAT_R8G8B8A8_UNORM, 1, 1);
  ExpectBlock(DXGI_FORMAT_R32_FLOAT, 1, 1);
  ExpectBlock(DXGI_FORMAT_R16G16B16A16_FLOAT, 1, 1);
  ExpectBlock(DXGI_FORMAT_UNKNOWN, 1, 1);
  ExpectBlock(DXGI_FORMAT_AYUV, 1, 1);
  ExpectBlock(DXGI_FORMAT_NV12, 1, 1);
}

TEST_CASE("texture_copy_clamp: mip from subresource index", "[texture_copy_clamp]") {
  CHECK(MipFromSubresource(0, 10) == 0);
  CHECK(MipFromSubresource(9, 10) == 9);
  // Slice 1 of a 10-mip array texture, level 3.
  CHECK(MipFromSubresource(13, 10) == 3);
  // Cube face 5, level 2 of a 4-mip texture.
  CHECK(MipFromSubresource(5 * 4 + 2, 4) == 2);
  CHECK(MipFromSubresource(7, 0) == 0);
}

TEST_CASE("texture_copy_clamp: level extent rounds BC levels up to whole blocks",
          "[texture_copy_clamp]") {
  // Log 4: 512x512 BC3, 10 mips. Level 8 is 2x2 texels = one 4x4 block,
  // level 9 is 1x1 texels = one 4x4 block.
  Extent l8 = LevelExtent(512, 512, 1, 8, false, 4, 4);
  CHECK(l8.width == 4);
  CHECK(l8.height == 4);
  CHECK(l8.depth == 1);
  Extent l9 = LevelExtent(512, 512, 1, 9, false, 4, 4);
  CHECK(l9.width == 4);
  CHECK(l9.height == 4);
  // Level 0 of 512 stays 512.
  Extent l0 = LevelExtent(512, 512, 1, 0, false, 4, 4);
  CHECK(l0.width == 512);
  // A non-block format is not rounded: level 5 of 1024 is 32, and the 2x2 /
  // 1x1 tail of a B5G6R5 texture stays 2x2 / 1x1.
  Extent p5 = LevelExtent(1024, 1024, 1, 5, false, 1, 1);
  CHECK(p5.width == 32);
  CHECK(p5.height == 32);
  Extent t = LevelExtent(64, 64, 1, 5, false, 1, 1);
  CHECK(t.width == 2);
  CHECK(t.height == 2);
  // 3D textures shrink depth with the mip, 2D arrays do not.
  Extent v = LevelExtent(64, 64, 16, 2, true, 1, 1);
  CHECK(v.depth == 4);
  Extent a = LevelExtent(64, 64, 16, 2, false, 1, 1);
  CHECK(a.depth == 1);
  // Never below one texel/block even past the last mip.
  Extent tiny = LevelExtent(4, 4, 4, 10, true, 1, 1);
  CHECK(tiny.width == 1);
  CHECK(tiny.height == 1);
  CHECK(tiny.depth == 1);
  // 2x1 packed: a 1-wide level is one 2-wide block.
  Extent yuv = LevelExtent(8, 8, 1, 3, false, 2, 1);
  CHECK(yuv.width == 2);
  CHECK(yuv.height == 1);
}

TEST_CASE("texture_copy_clamp: a 4x4 BC block into a 2x2 level is legal and untouched",
          "[texture_copy_clamp]") {
  // Exactly the log-4 copies: box [0,4)x[8,12) into level 8 and [0,4)x[4,8)
  // into level 9 of the 512x512 BC3 texture.
  uint32_t bw, bh;
  BlockSizeForDxgiFormat(DXGI_FORMAT_BC3_UNORM, bw, bh);
  Box b8{0, 8, 0, 4, 12, 1};
  const Box before8 = b8;
  CHECK_FALSE(ClampBoxToLevel(b8, LevelExtent(512, 512, 1, 8, false, bw, bh)));
  CHECK(b8.right == before8.right);
  CHECK(b8.bottom == before8.bottom);
  Box b9{0, 4, 0, 4, 8, 1};
  CHECK_FALSE(ClampBoxToLevel(b9, LevelExtent(512, 512, 1, 9, false, bw, bh)));
  CHECK(b9.Width() == 4);
  CHECK(b9.Height() == 4);
  // Same for the BC1 256x256 9-mip texture from the local repro (log 127).
  BlockSizeForDxgiFormat(DXGI_FORMAT_BC1_UNORM, bw, bh);
  Box c8{0, 4, 0, 4, 8, 1};
  CHECK_FALSE(ClampBoxToLevel(c8, LevelExtent(256, 256, 1, 8, false, bw, bh)));
}

TEST_CASE("texture_copy_clamp: a 4x4 copy into a 2x2 B5G6R5 level is NOT legal and is clamped",
          "[texture_copy_clamp]") {
  // The critic's case: with the wrong BC range B5G6R5 (85) was treated as
  // 4x4 and this copy passed untouched, which removes the device.
  uint32_t bw, bh;
  BlockSizeForDxgiFormat(DXGI_FORMAT_B5G6R5_UNORM, bw, bh);
  Box b = FullBox(4, 4);
  CHECK(ClampBoxToLevel(b, LevelExtent(64, 64, 1, 5, false, bw, bh)));
  CHECK(b.Width() == 2);
  CHECK(b.Height() == 2);
  BlockSizeForDxgiFormat(DXGI_FORMAT_B5G5R5A1_UNORM, bw, bh);
  Box c = FullBox(64, 64);
  CHECK(ClampBoxToLevel(c, LevelExtent(64, 64, 1, 5, false, bw, bh)));
  CHECK(c.Width() == 2);
}

TEST_CASE("texture_copy_clamp: the log-3 64x64 into 32x32 copy is clamped",
          "[texture_copy_clamp]") {
  // R8G8B8A8: level 5 of a 1024 texture is 32x32, the source box is 64x64.
  uint32_t bw, bh;
  BlockSizeForDxgiFormat(DXGI_FORMAT_R8G8B8A8_TYPELESS, bw, bh);
  Box b = FullBox(64, 64);
  Extent level = LevelExtent(1024, 1024, 1, 5, false, bw, bh);
  CHECK(ClampBoxToLevel(b, level));
  CHECK(b.left == 0);
  CHECK(b.top == 0);
  CHECK(b.Width() == 32);
  CHECK(b.Height() == 32);
  CHECK(b.Depth() == 1);
  // A box with an offset keeps its origin.
  Box off{16, 32, 0, 80, 96, 1};
  CHECK(ClampBoxToLevel(off, level));
  CHECK(off.left == 16);
  CHECK(off.top == 32);
  CHECK(off.right == 48);
  CHECK(off.bottom == 64);
}

TEST_CASE("texture_copy_clamp: a clamped BC box stays block-aligned", "[texture_copy_clamp]") {
  // Two blocks wide (8 texels) into a level that is one block (4 texels, e.g.
  // level 7 of 512 = 4x4): clamp to 4, not to some unaligned width.
  Box b = FullBox(8, 8);
  Extent level = LevelExtent(512, 512, 1, 7, false, 4, 4);
  CHECK(ClampBoxToLevel(b, level));
  CHECK(b.Width() == 4);
  CHECK(b.Height() == 4);
  CHECK(b.Width() % 4 == 0);
  // 12x4 into a level of 6 texels (level 4 of 96): 6 rounds to 8 blocks-wise.
  Box c = FullBox(12, 4);
  Extent l6 = LevelExtent(96, 4, 1, 4, false, 4, 4);
  CHECK(l6.width == 8);
  CHECK(ClampBoxToLevel(c, l6));
  CHECK(c.Width() == 8);
  CHECK(c.Height() == 4);
}

TEST_CASE("texture_copy_clamp: fitting boxes are never touched", "[texture_copy_clamp]") {
  Box exact = FullBox(32, 32);
  CHECK_FALSE(ClampBoxToLevel(exact, Extent{32, 32, 1}));
  CHECK(exact.Width() == 32);
  Box smaller = FullBox(16, 8, 1);
  CHECK_FALSE(ClampBoxToLevel(smaller, Extent{32, 32, 1}));
  CHECK(smaller.Width() == 16);
  CHECK(smaller.Height() == 8);
  // Depth is checked too: a 4-deep copy into a 2-deep 3D level is clamped.
  Box deep = FullBox(8, 8, 4);
  CHECK(ClampBoxToLevel(deep, Extent{8, 8, 2}));
  CHECK(deep.Depth() == 2);
  CHECK(deep.Width() == 8);
}

TEST_CASE("texture_copy_clamp: report keys do not collide across levels", "[texture_copy_clamp]") {
  // Two hashes that differ only in the top byte, all levels 0..15: no key
  // may repeat (the 1.3.5 key was hash ^ (level << 56), which collides).
  const uint64_t a = 0x0123456789ABCDEFull, b = a ^ (UINT64_C(1) << 56);
  std::vector<uint64_t> keys;
  for (uint32_t level = 0; level < 16; ++level) {
    keys.push_back(ReportKey(a, level));
    keys.push_back(ReportKey(b, level));
  }
  std::sort(keys.begin(), keys.end());
  CHECK(std::adjacent_find(keys.begin(), keys.end()) == keys.end());
}

TEST_CASE("texture_copy_clamp: host resource extent applies the draw resolution scale",
          "[texture_copy_clamp]") {
  // #22 log 5: guest 32x32 k_8_8_8_8, scaled_resolve, 2x scale. The loader
  // uploads 64x64, so the resource (CreateTexture and the 3D-as-2D wrapper
  // alike) must be 64x64; the wrapper used to be 32x32.
  Extent2 scaled = HostResourceExtent(32, 32, true, 2, 2);
  CHECK(scaled.width == 64);
  CHECK(scaled.height == 64);
  // The upload of that size fits the scaled resource and not the unscaled one.
  Box upload = FullBox(64, 64);
  CHECK_FALSE(ClampBoxToLevel(upload, LevelExtent(scaled.width, scaled.height, 1, 0, false, 1, 1)));
  Box upload_unscaled = FullBox(64, 64);
  CHECK(ClampBoxToLevel(upload_unscaled, LevelExtent(32, 32, 1, 0, false, 1, 1)));
  // Not scaled: guest size as is, whatever the draw scale.
  Extent2 plain = HostResourceExtent(32, 32, false, 3, 3);
  CHECK(plain.width == 32);
  CHECK(plain.height == 32);
  // Identity: scaled_resolve at scale 1 is the guest size (scaled_resolve can
  // only be set when the scale is above 1, but the rule must hold anyway).
  Extent2 one = HostResourceExtent(32, 32, true, 1, 1);
  CHECK(one.width == 32);
  CHECK(one.height == 32);
  // Non-uniform scale.
  Extent2 wide = HostResourceExtent(640, 480, true, 3, 2);
  CHECK(wide.width == 1920);
  CHECK(wide.height == 960);
}
