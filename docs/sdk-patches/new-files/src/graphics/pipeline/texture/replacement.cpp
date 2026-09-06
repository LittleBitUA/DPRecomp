/**
 ******************************************************************************
 * ReXGlue — guest texture dump / host texture replacement (DP1, 2026-09-06)   *
 ******************************************************************************
 * See replacement.h for the design.
 */

#include <rex/graphics/pipeline/texture/replacement.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>

#include <xxhash.h>

#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#include <stb_image.h>

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/graphics/flags.h>
#include <rex/graphics/pipeline/texture/conversion.h>
#include <rex/graphics/pipeline/texture/info.h>
#include <rex/logging.h>

namespace rex::graphics::texture_replacement {

namespace {

// ---------------------------------------------------------------------------
// Minimal PNG writer: uncompressed ("stored") deflate blocks. Files are large
// but every viewer/editor opens them and nothing extra has to be linked.
// ---------------------------------------------------------------------------

uint32_t Crc32(const uint8_t* data, size_t size, uint32_t crc = 0) {
  static uint32_t table[256];
  static bool table_ready = false;
  if (!table_ready) {
    for (uint32_t i = 0; i < 256; ++i) {
      uint32_t c = i;
      for (int k = 0; k < 8; ++k) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
      table[i] = c;
    }
    table_ready = true;
  }
  crc = ~crc;
  for (size_t i = 0; i < size; ++i) crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
  return ~crc;
}

void PutU32BE(std::vector<uint8_t>& out, uint32_t v) {
  out.push_back(uint8_t(v >> 24));
  out.push_back(uint8_t(v >> 16));
  out.push_back(uint8_t(v >> 8));
  out.push_back(uint8_t(v));
}

void PutChunk(std::vector<uint8_t>& out, const char* type, const std::vector<uint8_t>& payload) {
  PutU32BE(out, uint32_t(payload.size()));
  size_t crc_start = out.size();
  out.insert(out.end(), type, type + 4);
  out.insert(out.end(), payload.begin(), payload.end());
  PutU32BE(out, Crc32(out.data() + crc_start, out.size() - crc_start));
}

bool WritePngRGBA(const std::filesystem::path& path, const uint8_t* rgba, uint32_t width,
                  uint32_t height) {
  // Raw scanlines with filter byte 0.
  std::vector<uint8_t> raw;
  raw.reserve(size_t(height) * (size_t(width) * 4 + 1));
  for (uint32_t y = 0; y < height; ++y) {
    raw.push_back(0);
    raw.insert(raw.end(), rgba + size_t(y) * width * 4, rgba + size_t(y + 1) * width * 4);
  }
  // zlib stream: header, stored blocks, adler32.
  std::vector<uint8_t> z;
  z.push_back(0x78);
  z.push_back(0x01);
  size_t pos = 0;
  while (pos < raw.size() || raw.empty()) {
    size_t n = std::min<size_t>(65535, raw.size() - pos);
    bool last = pos + n >= raw.size();
    z.push_back(last ? 1 : 0);
    z.push_back(uint8_t(n));
    z.push_back(uint8_t(n >> 8));
    z.push_back(uint8_t(~n));
    z.push_back(uint8_t((~n) >> 8));
    z.insert(z.end(), raw.begin() + pos, raw.begin() + pos + n);
    pos += n;
    if (raw.empty()) break;
  }
  uint32_t a = 1, b = 0;
  for (uint8_t c : raw) {
    a = (a + c) % 65521;
    b = (b + a) % 65521;
  }
  PutU32BE(z, (b << 16) | a);

  std::vector<uint8_t> png = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
  std::vector<uint8_t> ihdr;
  PutU32BE(ihdr, width);
  PutU32BE(ihdr, height);
  ihdr.push_back(8);  // bit depth
  ihdr.push_back(6);  // RGBA
  ihdr.push_back(0);
  ihdr.push_back(0);
  ihdr.push_back(0);
  PutChunk(png, "IHDR", ihdr);
  PutChunk(png, "IDAT", z);
  PutChunk(png, "IEND", {});
  std::ofstream f(path, std::ios::binary | std::ios::trunc);
  if (!f) return false;
  f.write(reinterpret_cast<const char*>(png.data()), std::streamsize(png.size()));
  return bool(f);
}

// ---------------------------------------------------------------------------
// Guest format decoders (all operate on little-endian block data, i.e. after
// the fetch-constant endian swap has been undone).
// ---------------------------------------------------------------------------

inline void Unpack565(uint16_t c, uint8_t* rgb) {
  rgb[0] = uint8_t(((c >> 11) & 31) * 255 / 31);
  rgb[1] = uint8_t(((c >> 5) & 63) * 255 / 63);
  rgb[2] = uint8_t((c & 31) * 255 / 31);
}

// Decodes one 4x4 BC1 colour block into a 16x4-byte RGBA scratch.
void DecodeBC1Block(const uint8_t* block, uint8_t out[16][4], bool allow_1bit_alpha) {
  uint16_t c0 = uint16_t(block[0] | (block[1] << 8));
  uint16_t c1 = uint16_t(block[2] | (block[3] << 8));
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
  uint32_t bits = uint32_t(block[4]) | (uint32_t(block[5]) << 8) | (uint32_t(block[6]) << 16) |
                  (uint32_t(block[7]) << 24);
  for (int i = 0; i < 16; ++i) {
    const uint8_t* p = palette[(bits >> (i * 2)) & 3];
    std::memcpy(out[i], p, 4);
  }
}

void DecodeBC3Alpha(const uint8_t* block, uint8_t alpha_out[16]) {
  uint8_t a0 = block[0], a1 = block[1];
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

// Copies decoded 4x4 block pixels into the RGBA image, clipping at the edge.
void StoreBlock(uint8_t out[16][4], uint32_t bx, uint32_t by, uint32_t width, uint32_t height,
                std::vector<uint8_t>& rgba) {
  for (uint32_t py = 0; py < 4; ++py) {
    uint32_t y = by * 4 + py;
    if (y >= height) break;
    for (uint32_t px = 0; px < 4; ++px) {
      uint32_t x = bx * 4 + px;
      if (x >= width) break;
      std::memcpy(&rgba[(size_t(y) * width + x) * 4], out[py * 4 + px], 4);
    }
  }
}

}  // namespace

// ---------------------------------------------------------------------------

Registry& Registry::Get() {
  static Registry instance;
  return instance;
}

Registry::Registry() {
  std::string configured = REXCVAR_GET(texture_path);
  root_ = configured.empty() ? (rex::filesystem::GetExecutableFolder() / "textures")
                             : std::filesystem::path(configured);
  dump_enabled_ = REXCVAR_GET(texture_dump);
  dump_dir_ = root_ / "dump";
  std::error_code ec;
  if (dump_enabled_) {
    std::filesystem::create_directories(dump_dir_, ec);
    REXGPU_INFO("Texture dump enabled: {}", dump_dir_.string());
  }
  if (REXCVAR_GET(texture_replacement)) {
    Scan();
  }
}

void Registry::Scan() {
  std::error_code ec;
  if (!std::filesystem::is_directory(root_, ec)) return;
  for (const auto& entry : std::filesystem::directory_iterator(root_, ec)) {
    if (!entry.is_regular_file(ec)) continue;
    const std::filesystem::path& p = entry.path();
    std::string ext = p.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    if (ext != ".png") continue;
    std::string stem = p.stem().string();
    bool overlay = false;
    {
      std::string lower = stem;
      std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
      if (lower.size() > 8 && lower.compare(lower.size() - 8, 8, ".overlay") == 0) {
        overlay = true;
      }
    }
    // Accept "<16 hex>", "<16 hex>_anything" and "<16 hex>.overlay".
    if (stem.size() < 16) continue;
    uint64_t hash = 0;
    bool ok = true;
    for (size_t i = 0; i < 16; ++i) {
      char c = stem[i];
      uint32_t v;
      if (c >= '0' && c <= '9') v = uint32_t(c - '0');
      else if (c >= 'a' && c <= 'f') v = uint32_t(c - 'a' + 10);
      else if (c >= 'A' && c <= 'F') v = uint32_t(c - 'A' + 10);
      else { ok = false; break; }
      hash = (hash << 4) | v;
    }
    if (!ok) continue;
    auto existing = files_.find(hash);
    if (existing != files_.end() && !existing->second.overlay && overlay) continue;
    files_[hash] = File{p, overlay};
  }
  if (!files_.empty()) {
    REXGPU_INFO("Texture replacement: {} file(s) in {}", files_.size(), root_.string());
  }
}

uint64_t Registry::Hash(const GuestBaseView& view) const {
  uint64_t seed = (uint64_t(view.width) << 40) ^ (uint64_t(view.height) << 20) ^
                  uint64_t(view.format);
  return XXH64(view.data, view.size_bytes, seed);
}

const Image* Registry::Find(uint64_t hash, const GuestBaseView* view) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto cached = images_.find(hash);
  if (cached != images_.end()) return cached->second.get();
  auto file = files_.find(hash);
  if (file == files_.end()) return nullptr;

  std::unique_ptr<Image> image;
  std::ifstream f(file->second.path, std::ios::binary);
  std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  int w = 0, h = 0, comp = 0;
  stbi_uc* pixels =
      bytes.empty() ? nullptr : stbi_load_from_memory(bytes.data(), int(bytes.size()), &w, &h, &comp, 4);
  std::vector<uint8_t> guest_rgba;
  bool overlay_ok = true;
  if (pixels && w > 0 && h > 0 && file->second.overlay) {
    // Overlay: decode the guest base level, upscale it to the overlay size
    // (bilinear) and punch the overlay through where its alpha is > 0.
    overlay_ok = view != nullptr && DecodeGuestBase(*view, guest_rgba);
    if (overlay_ok) {
      const uint32_t gw = view->width, gh = view->height;
      std::vector<uint8_t> base(size_t(w) * h * 4);
      for (int y = 0; y < h; ++y) {
        const float sy = (float(y) + 0.5f) * float(gh) / float(h) - 0.5f;
        const int y0 = std::clamp(int(std::floor(sy)), 0, int(gh) - 1);
        const int y1 = std::min(y0 + 1, int(gh) - 1);
        const float fy = std::clamp(sy - float(y0), 0.0f, 1.0f);
        for (int x = 0; x < w; ++x) {
          const float sx = (float(x) + 0.5f) * float(gw) / float(w) - 0.5f;
          const int x0 = std::clamp(int(std::floor(sx)), 0, int(gw) - 1);
          const int x1 = std::min(x0 + 1, int(gw) - 1);
          const float fx = std::clamp(sx - float(x0), 0.0f, 1.0f);
          for (int c = 0; c < 4; ++c) {
            const float p00 = guest_rgba[(size_t(y0) * gw + x0) * 4 + c];
            const float p01 = guest_rgba[(size_t(y0) * gw + x1) * 4 + c];
            const float p10 = guest_rgba[(size_t(y1) * gw + x0) * 4 + c];
            const float p11 = guest_rgba[(size_t(y1) * gw + x1) * 4 + c];
            const float v = (p00 * (1 - fx) + p01 * fx) * (1 - fy) + (p10 * (1 - fx) + p11 * fx) * fy;
            base[(size_t(y) * w + x) * 4 + c] = uint8_t(std::clamp(int(v + 0.5f), 0, 255));
          }
        }
      }
      // alpha > 1: overlay pixel replaces the original; alpha == 1: erase to
      // transparent (lets an overlay clear a cell it does not fully cover).
      for (size_t i = 0; i < size_t(w) * h; ++i) {
        const uint8_t a = pixels[i * 4 + 3];
        if (a > 1) {
          std::memcpy(&base[i * 4], &pixels[i * 4], 4);
        } else if (a == 1) {
          std::memset(&base[i * 4], 0, 4);
        }
      }
      std::memcpy(pixels, base.data(), base.size());
    } else {
      REXGPU_WARN("Texture replacement: overlay {} needs a decodable guest texture (format not supported)",
                  file->second.path.filename().string());
    }
  }
  if (pixels && w > 0 && h > 0 && overlay_ok) {
    image = std::make_unique<Image>();
    image->width = uint32_t(w);
    image->height = uint32_t(h);
    image->levels.emplace_back(pixels, pixels + size_t(w) * h * 4);
    // Box-filtered mip chain down to 1x1.
    while (image->LevelWidth(uint32_t(image->levels.size() - 1)) > 1 ||
           image->LevelHeight(uint32_t(image->levels.size() - 1)) > 1) {
      uint32_t level = uint32_t(image->levels.size());
      uint32_t sw = image->LevelWidth(level - 1), sh = image->LevelHeight(level - 1);
      uint32_t dw = image->LevelWidth(level), dh = image->LevelHeight(level);
      const std::vector<uint8_t>& src = image->levels.back();
      std::vector<uint8_t> dst(size_t(dw) * dh * 4);
      for (uint32_t y = 0; y < dh; ++y) {
        uint32_t sy0 = std::min(y * 2, sh - 1), sy1 = std::min(y * 2 + 1, sh - 1);
        for (uint32_t x = 0; x < dw; ++x) {
          uint32_t sx0 = std::min(x * 2, sw - 1), sx1 = std::min(x * 2 + 1, sw - 1);
          for (uint32_t c = 0; c < 4; ++c) {
            uint32_t sum = src[(size_t(sy0) * sw + sx0) * 4 + c] +
                           src[(size_t(sy0) * sw + sx1) * 4 + c] +
                           src[(size_t(sy1) * sw + sx0) * 4 + c] +
                           src[(size_t(sy1) * sw + sx1) * 4 + c];
            dst[(size_t(y) * dw + x) * 4 + c] = uint8_t((sum + 2) / 4);
          }
        }
      }
      image->levels.push_back(std::move(dst));
    }
    REXGPU_INFO("Texture replacement: {:016X} <- {} ({}x{}, {} mips{})", hash,
                file->second.path.filename().string(), w, h, image->levels.size(),
                file->second.overlay ? ", overlay" : "");
  } else if (!pixels) {
    REXGPU_WARN("Texture replacement: failed to decode {}", file->second.path.string());
  }
  if (pixels) stbi_image_free(pixels);
  const Image* result = image.get();
  images_.emplace(hash, std::move(image));
  return result;
}

bool Registry::DecodeGuestBase(const GuestBaseView& view, std::vector<uint8_t>& rgba_out) {
  const FormatInfo* info = FormatInfo::Get(view.format);
  if (!info || !view.data || !view.width || !view.height) return false;
  uint32_t bw = info->block_width, bh = info->block_height;
  uint32_t bpb = info->bytes_per_block();
  uint32_t blocks_x = (view.width + bw - 1) / bw;
  uint32_t blocks_y = (view.height + bh - 1) / bh;
  if (!bpb || !blocks_x || !blocks_y) return false;

  switch (view.format) {
    case xenos::TextureFormat::k_8_8_8_8:
    case xenos::TextureFormat::k_8:
    case xenos::TextureFormat::k_DXT1:
    case xenos::TextureFormat::k_DXT2_3:
    case xenos::TextureFormat::k_DXT4_5:
    case xenos::TextureFormat::k_DXT5A:
      break;
    default:
      return false;
  }

  // 1. Linearize blocks (untile if needed), keeping the guest byte order.
  std::vector<uint8_t> linear(size_t(blocks_x) * blocks_y * bpb);
  if (view.tiled) {
    texture_conversion::UntileInfo untile{};
    untile.offset_x = 0;
    untile.offset_y = 0;
    untile.width = blocks_x;
    untile.height = blocks_y;
    untile.input_pitch = view.row_pitch_bytes / bpb;  // pitch in blocks for tiled
    untile.output_pitch = blocks_x;                    // in blocks
    untile.input_format_info = info;
    untile.output_format_info = info;
    untile.copy_callback = [](void* dst, const void* src, size_t n) { std::memcpy(dst, src, n); };
    // Guard against a pitch smaller than the visible width.
    if (untile.input_pitch < blocks_x) return false;
    // The tiled extent is rounded to 32x32 blocks by the layout code; the
    // maximum offset Untile can produce stays inside x_extent*y_extent.
    if (size_t(view.x_extent_blocks) * view.y_extent_blocks * bpb > view.size_bytes) return false;
    texture_conversion::Untile(linear.data(), view.data, &untile);
  } else {
    if (size_t(view.row_pitch_bytes) * (blocks_y - 1) + size_t(blocks_x) * bpb > view.size_bytes) {
      return false;
    }
    for (uint32_t y = 0; y < blocks_y; ++y) {
      std::memcpy(&linear[size_t(y) * blocks_x * bpb], view.data + size_t(y) * view.row_pitch_bytes,
                  size_t(blocks_x) * bpb);
    }
  }
  // 2. Undo the endian swap so blocks are little-endian as on the host.
  texture_conversion::CopySwapBlock(view.endian, linear.data(), linear.data(), linear.size());

  // 3. Decode.
  rgba_out.assign(size_t(view.width) * view.height * 4, 0);
  switch (view.format) {
    case xenos::TextureFormat::k_8_8_8_8:
      for (uint32_t y = 0; y < view.height; ++y) {
        std::memcpy(&rgba_out[size_t(y) * view.width * 4], &linear[size_t(y) * blocks_x * 4],
                    size_t(view.width) * 4);
      }
      break;
    case xenos::TextureFormat::k_8:
      for (uint32_t y = 0; y < view.height; ++y) {
        for (uint32_t x = 0; x < view.width; ++x) {
          uint8_t v = linear[size_t(y) * blocks_x + x];
          uint8_t* p = &rgba_out[(size_t(y) * view.width + x) * 4];
          p[0] = p[1] = p[2] = v;
          p[3] = 255;
        }
      }
      break;
    case xenos::TextureFormat::k_DXT1:
      for (uint32_t by = 0; by < blocks_y; ++by) {
        for (uint32_t bx = 0; bx < blocks_x; ++bx) {
          uint8_t out[16][4];
          DecodeBC1Block(&linear[(size_t(by) * blocks_x + bx) * 8], out, true);
          StoreBlock(out, bx, by, view.width, view.height, rgba_out);
        }
      }
      break;
    case xenos::TextureFormat::k_DXT2_3:
      for (uint32_t by = 0; by < blocks_y; ++by) {
        for (uint32_t bx = 0; bx < blocks_x; ++bx) {
          const uint8_t* block = &linear[(size_t(by) * blocks_x + bx) * 16];
          uint8_t out[16][4];
          DecodeBC1Block(block + 8, out, false);
          for (int i = 0; i < 16; ++i) {
            uint8_t a4 = (block[i / 2] >> ((i & 1) * 4)) & 0xF;
            out[i][3] = uint8_t(a4 * 17);
          }
          StoreBlock(out, bx, by, view.width, view.height, rgba_out);
        }
      }
      break;
    case xenos::TextureFormat::k_DXT4_5:
      for (uint32_t by = 0; by < blocks_y; ++by) {
        for (uint32_t bx = 0; bx < blocks_x; ++bx) {
          const uint8_t* block = &linear[(size_t(by) * blocks_x + bx) * 16];
          uint8_t out[16][4];
          DecodeBC1Block(block + 8, out, false);
          uint8_t alpha[16];
          DecodeBC3Alpha(block, alpha);
          for (int i = 0; i < 16; ++i) out[i][3] = alpha[i];
          StoreBlock(out, bx, by, view.width, view.height, rgba_out);
        }
      }
      break;
    case xenos::TextureFormat::k_DXT5A:
      for (uint32_t by = 0; by < blocks_y; ++by) {
        for (uint32_t bx = 0; bx < blocks_x; ++bx) {
          uint8_t alpha[16];
          DecodeBC3Alpha(&linear[(size_t(by) * blocks_x + bx) * 8], alpha);
          uint8_t out[16][4];
          for (int i = 0; i < 16; ++i) {
            out[i][0] = out[i][1] = out[i][2] = alpha[i];
            out[i][3] = 255;
          }
          StoreBlock(out, bx, by, view.width, view.height, rgba_out);
        }
      }
      break;
    default:
      return false;
  }
  return true;
}

void Registry::Dump(uint64_t hash, const GuestBaseView& view) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!dumped_.insert(hash).second) return;
  }
  std::vector<uint8_t> rgba;
  const FormatInfo* info = FormatInfo::Get(view.format);
  const char* format_name = info ? info->name : "unknown";
  if (!DecodeGuestBase(view, rgba)) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (unsupported_formats_logged_.insert(uint32_t(view.format)).second) {
      REXGPU_WARN("Texture dump: format {} not supported (first seen {}x{}, hash {:016X})",
                  format_name, view.width, view.height, hash);
    }
    return;
  }
  char name[96];
  std::snprintf(name, sizeof(name), "%016llX_%ux%u_%s.png", (unsigned long long)hash, view.width,
                view.height, format_name);
  std::filesystem::path path = dump_dir_ / name;
  if (WritePngRGBA(path, rgba.data(), view.width, view.height)) {
    REXGPU_INFO("Texture dump: {} @ {:08X} tiled={} endian={}", name, view.guest_address, view.tiled,
                uint32_t(view.endian));
  } else {
    REXGPU_WARN("Texture dump: failed to write {}", path.string());
  }
}

}  // namespace rex::graphics::texture_replacement
