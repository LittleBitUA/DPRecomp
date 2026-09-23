// [NEW FABLE VERSION] 2026-09-23
// Deadly Premonition Recompilation - texture replacement for the native renderer.
//
// The emulated path replaces guest textures with host PNGs in the SDK's GPU
// plugin (rex::graphics::texture_replacement, pipeline/texture/replacement.cpp):
// the launcher writes textures\<hash>.png and textures\<hash>.overlay.png, the
// keyboard key caps over the button prompt atlas among them. The native
// renderer runs without that plugin, so it matches the same files itself.
//
// Everything here must stay bit-compatible with the SDK: a texture is named by
// XXH64 of its guest base level (tiled, big-endian, exactly
// TextureGuestLayout::base.level_data_extent_bytes) seeded with
// HashSeed(width, height, format); an overlay is composited over the decoded
// guest base upscaled bilinearly to the PNG's size (alpha > 1 replaces, alpha
// == 1 erases to transparent); mips are 2x2 box filtered down to 1x1.
//
// This header holds the pure parts (covered by tests/native_texrep_test.cpp);
// file scanning and PNG decoding live in native_texrep.cpp.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>
#include <string_view>
#include <vector>

namespace dp::native::texrep {

struct Image {
  uint32_t width = 0;
  uint32_t height = 0;
  // Level 0..n-1, RGBA8 tightly packed.
  std::vector<std::vector<uint8_t>> levels;
  uint32_t LevelWidth(uint32_t level) const { return std::max(width >> level, 1u); }
  uint32_t LevelHeight(uint32_t level) const { return std::max(height >> level, 1u); }
};

// Seed of the SDK's Registry::Hash (XXH64 over the base level bytes).
constexpr uint64_t HashSeed(uint32_t width, uint32_t height, uint32_t format) {
  return (uint64_t(width) << 40) ^ (uint64_t(height) << 20) ^ uint64_t(format);
}

// File stem -> hash. Accepts "<16 hex>", "<16 hex>_anything" and
// "<16 hex>.overlay" (case-insensitive), like the SDK's Registry::Scan.
inline bool ParseName(std::string_view stem, uint64_t* hash_out, bool* overlay_out) {
  if (stem.size() < 16) return false;
  uint64_t hash = 0;
  for (size_t i = 0; i < 16; ++i) {
    const char c = stem[i];
    uint32_t v;
    if (c >= '0' && c <= '9') v = uint32_t(c - '0');
    else if (c >= 'a' && c <= 'f') v = uint32_t(c - 'a' + 10);
    else if (c >= 'A' && c <= 'F') v = uint32_t(c - 'A' + 10);
    else return false;
    hash = (hash << 4) | v;
  }
  bool overlay = false;
  if (stem.size() > 8) {
    std::string_view tail = stem.substr(stem.size() - 8);
    static constexpr char kOverlay[] = ".overlay";
    overlay = true;
    for (size_t i = 0; i < 8; ++i) {
      char c = tail[i];
      if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
      if (c != kOverlay[i]) { overlay = false; break; }
    }
  }
  *hash_out = hash;
  *overlay_out = overlay;
  return true;
}

// Upscales the guest RGBA (gw x gh) bilinearly to w x h and composites the
// overlay (w x h, in place) over it: alpha > 1 keeps the overlay pixel, alpha
// == 1 erases to transparent, alpha 0 shows the guest pixel.
inline void ComposeOverlay(const uint8_t* guest, uint32_t gw, uint32_t gh, uint8_t* overlay, uint32_t w,
                           uint32_t h) {
  std::vector<uint8_t> base(size_t(w) * h * 4);
  for (uint32_t y = 0; y < h; ++y) {
    const float sy = (float(y) + 0.5f) * float(gh) / float(h) - 0.5f;
    const int y0 = std::clamp(int(std::floor(sy)), 0, int(gh) - 1);
    const int y1 = std::min(y0 + 1, int(gh) - 1);
    const float fy = std::clamp(sy - float(y0), 0.0f, 1.0f);
    for (uint32_t x = 0; x < w; ++x) {
      const float sx = (float(x) + 0.5f) * float(gw) / float(w) - 0.5f;
      const int x0 = std::clamp(int(std::floor(sx)), 0, int(gw) - 1);
      const int x1 = std::min(x0 + 1, int(gw) - 1);
      const float fx = std::clamp(sx - float(x0), 0.0f, 1.0f);
      for (int c = 0; c < 4; ++c) {
        const float p00 = guest[(size_t(y0) * gw + x0) * 4 + c];
        const float p01 = guest[(size_t(y0) * gw + x1) * 4 + c];
        const float p10 = guest[(size_t(y1) * gw + x0) * 4 + c];
        const float p11 = guest[(size_t(y1) * gw + x1) * 4 + c];
        const float v = (p00 * (1 - fx) + p01 * fx) * (1 - fy) + (p10 * (1 - fx) + p11 * fx) * fy;
        base[(size_t(y) * w + x) * 4 + c] = uint8_t(std::clamp(int(v + 0.5f), 0, 255));
      }
    }
  }
  for (size_t i = 0; i < size_t(w) * h; ++i) {
    const uint8_t a = overlay[i * 4 + 3];
    if (a > 1) continue;
    if (a == 1) std::memset(&overlay[i * 4], 0, 4);
    else std::memcpy(&overlay[i * 4], &base[i * 4], 4);
  }
}

// Appends 2x2 box-filtered levels down to 1x1.
inline void BuildMips(Image& image) {
  while (image.LevelWidth(uint32_t(image.levels.size() - 1)) > 1 ||
         image.LevelHeight(uint32_t(image.levels.size() - 1)) > 1) {
    const uint32_t level = uint32_t(image.levels.size());
    const uint32_t sw = image.LevelWidth(level - 1), sh = image.LevelHeight(level - 1);
    const uint32_t dw = image.LevelWidth(level), dh = image.LevelHeight(level);
    const std::vector<uint8_t>& src = image.levels.back();
    std::vector<uint8_t> dst(size_t(dw) * dh * 4);
    for (uint32_t y = 0; y < dh; ++y) {
      const uint32_t sy0 = std::min(y * 2, sh - 1), sy1 = std::min(y * 2 + 1, sh - 1);
      for (uint32_t x = 0; x < dw; ++x) {
        const uint32_t sx0 = std::min(x * 2, sw - 1), sx1 = std::min(x * 2 + 1, sw - 1);
        for (uint32_t c = 0; c < 4; ++c) {
          const uint32_t sum = src[(size_t(sy0) * sw + sx0) * 4 + c] + src[(size_t(sy0) * sw + sx1) * 4 + c] +
                               src[(size_t(sy1) * sw + sx0) * 4 + c] + src[(size_t(sy1) * sw + sx1) * 4 + c];
          dst[(size_t(y) * dw + x) * 4 + c] = uint8_t((sum + 2) / 4);
        }
      }
    }
    image.levels.push_back(std::move(dst));
  }
}

// ---- guest base level decoding (host byte order, linear blocks) ----------

enum class Codec { k8888, k8, kDXT1, kDXT2_3, kDXT4_5, kDXT5A };

inline void Unpack565(uint16_t c, uint8_t* rgb) {
  rgb[0] = uint8_t(((c >> 11) & 31) * 255 / 31);
  rgb[1] = uint8_t(((c >> 5) & 63) * 255 / 63);
  rgb[2] = uint8_t((c & 31) * 255 / 31);
}

inline void DecodeBC1Block(const uint8_t* block, uint8_t out[16][4], bool allow_1bit_alpha) {
  const uint16_t c0 = uint16_t(block[0] | (block[1] << 8));
  const uint16_t c1 = uint16_t(block[2] | (block[3] << 8));
  uint8_t palette[4][4];
  Unpack565(c0, palette[0]);
  Unpack565(c1, palette[1]);
  palette[0][3] = palette[1][3] = 255;
  if (c0 > c1 || !allow_1bit_alpha) {
    for (int i = 0; i < 3; ++i) {
      palette[2][i] = uint8_t((2 * palette[0][i] + palette[1][i]) / 3);
      palette[3][i] = uint8_t((palette[0][i] + 2 * palette[1][i]) / 3);
    }
    palette[2][3] = palette[3][3] = 255;
  } else {
    for (int i = 0; i < 3; ++i) {
      palette[2][i] = uint8_t((palette[0][i] + palette[1][i]) / 2);
      palette[3][i] = 0;
    }
    palette[2][3] = 255;
    palette[3][3] = 0;
  }
  const uint32_t bits = uint32_t(block[4]) | (uint32_t(block[5]) << 8) | (uint32_t(block[6]) << 16) |
                        (uint32_t(block[7]) << 24);
  for (int i = 0; i < 16; ++i) std::memcpy(out[i], palette[(bits >> (i * 2)) & 3], 4);
}

inline void DecodeBC3Alpha(const uint8_t* block, uint8_t alpha_out[16]) {
  const uint8_t a0 = block[0], a1 = block[1];
  uint8_t palette[8];
  palette[0] = a0;
  palette[1] = a1;
  if (a0 > a1) {
    for (int i = 1; i < 7; ++i) palette[i + 1] = uint8_t(((7 - i) * a0 + i * a1) / 7);
  } else {
    for (int i = 1; i < 5; ++i) palette[i + 1] = uint8_t(((5 - i) * a0 + i * a1) / 5);
    palette[6] = 0;
    palette[7] = 255;
  }
  uint64_t bits = 0;
  for (int i = 0; i < 6; ++i) bits |= uint64_t(block[2 + i]) << (i * 8);
  for (int i = 0; i < 16; ++i) alpha_out[i] = palette[(bits >> (i * 3)) & 7];
}

// `linear` = blocks_y rows of `row_pitch` bytes, already untiled and endian
// swapped. Writes width x height RGBA8.
inline bool DecodeBase(Codec codec, const uint8_t* linear, size_t row_pitch, uint32_t width, uint32_t height,
                       std::vector<uint8_t>& rgba) {
  rgba.assign(size_t(width) * height * 4, 0);
  auto store = [&](uint8_t out[16][4], uint32_t bx, uint32_t by) {
    for (uint32_t py = 0; py < 4; ++py) {
      const uint32_t y = by * 4 + py;
      if (y >= height) break;
      for (uint32_t px = 0; px < 4; ++px) {
        const uint32_t x = bx * 4 + px;
        if (x >= width) break;
        std::memcpy(&rgba[(size_t(y) * width + x) * 4], out[py * 4 + px], 4);
      }
    }
  };
  const uint32_t bx_count = (width + 3) / 4, by_count = (height + 3) / 4;
  switch (codec) {
    case Codec::k8888:
      for (uint32_t y = 0; y < height; ++y) std::memcpy(&rgba[size_t(y) * width * 4], linear + y * row_pitch, size_t(width) * 4);
      return true;
    case Codec::k8:
      for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t x = 0; x < width; ++x) {
          uint8_t* p = &rgba[(size_t(y) * width + x) * 4];
          p[0] = p[1] = p[2] = linear[y * row_pitch + x];
          p[3] = 255;
        }
      }
      return true;
    case Codec::kDXT1:
      for (uint32_t by = 0; by < by_count; ++by) {
        for (uint32_t bx = 0; bx < bx_count; ++bx) {
          uint8_t out[16][4];
          DecodeBC1Block(linear + by * row_pitch + size_t(bx) * 8, out, true);
          store(out, bx, by);
        }
      }
      return true;
    case Codec::kDXT2_3:
      for (uint32_t by = 0; by < by_count; ++by) {
        for (uint32_t bx = 0; bx < bx_count; ++bx) {
          const uint8_t* block = linear + by * row_pitch + size_t(bx) * 16;
          uint8_t out[16][4];
          DecodeBC1Block(block + 8, out, false);
          for (int i = 0; i < 16; ++i) out[i][3] = uint8_t(((block[i / 2] >> ((i & 1) * 4)) & 0xF) * 17);
          store(out, bx, by);
        }
      }
      return true;
    case Codec::kDXT4_5:
      for (uint32_t by = 0; by < by_count; ++by) {
        for (uint32_t bx = 0; bx < bx_count; ++bx) {
          const uint8_t* block = linear + by * row_pitch + size_t(bx) * 16;
          uint8_t out[16][4];
          DecodeBC1Block(block + 8, out, false);
          uint8_t alpha[16];
          DecodeBC3Alpha(block, alpha);
          for (int i = 0; i < 16; ++i) out[i][3] = alpha[i];
          store(out, bx, by);
        }
      }
      return true;
    case Codec::kDXT5A:
      for (uint32_t by = 0; by < by_count; ++by) {
        for (uint32_t bx = 0; bx < bx_count; ++bx) {
          uint8_t alpha[16];
          DecodeBC3Alpha(linear + by * row_pitch + size_t(bx) * 8, alpha);
          uint8_t out[16][4];
          for (int i = 0; i < 16; ++i) {
            out[i][0] = out[i][1] = out[i][2] = alpha[i];
            out[i][3] = 255;
          }
          store(out, bx, by);
        }
      }
      return true;
  }
  return false;
}

// ---- file registry (native_texrep.cpp) ------------------------------------

// True when the replacement folder holds at least one <hash>.png. Scans once.
bool Active();
// XXH64(data, size, HashSeed(width, height, format)).
uint64_t Hash(const uint8_t* data, size_t size, uint32_t width, uint32_t height, uint32_t format);
// The decoded replacement for `hash` (cached, including failures), or nullptr.
// `decode_guest` fills the guest base level as width x height RGBA8 (needed
// only for overlays) and returns false when the format is not supported.
using GuestDecoder = std::function<bool(std::vector<uint8_t>& rgba, uint32_t& width, uint32_t& height)>;
const Image* Find(uint64_t hash, const GuestDecoder& decode_guest);

}  // namespace dp::native::texrep
