#pragma once
/**
 ******************************************************************************
 * ReXGlue — guest texture dump / host texture replacement (DP1, 2026-09-06)   *
 ******************************************************************************
 *
 * Textures are identified by a 64-bit hash of the guest base-level bytes
 * (mixed with width, height and format) so the id is stable across runs and
 * independent of the guest address the game streamed the texture to.
 *
 *   texture_dump = true            -> every 2D texture the game loads is written
 *                                     to <texture_path>/dump/<hash>_<w>x<h>_<fmt>.png
 *   <texture_path>/<hash>.png       -> replaces the guest texture on the host
 *                                     (RGBA8, any size, mips generated here).
 *   <texture_path>/<hash>.overlay.png -> composited over the decoded guest
 *                                     texture where the PNG alpha > 1; alpha == 1
 *                                     erases to transparent (the
 *                                     guest image is upscaled to the PNG size
 *                                     first), so only the touched cells of an
 *                                     atlas need to be shipped.
 *
 * <texture_path> defaults to "textures" next to the executable.
 */

#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <rex/graphics/xenos.h>

namespace rex::graphics::texture_replacement {

struct Image {
  uint32_t width = 0;
  uint32_t height = 0;
  // Level 0..n-1, RGBA8 tightly packed (row pitch = LevelWidth(level) * 4).
  std::vector<std::vector<uint8_t>> levels;
  uint32_t LevelWidth(uint32_t level) const { return std::max(width >> level, 1u); }
  uint32_t LevelHeight(uint32_t level) const { return std::max(height >> level, 1u); }
};

// Base level of a guest texture as it sits in physical memory.
struct GuestBaseView {
  const uint8_t* data = nullptr;
  uint32_t guest_address = 0;  // physical base address (for log correlation)
  uint32_t size_bytes = 0;  // TextureGuestLayout::Level::level_data_extent_bytes
  uint32_t width = 0;
  uint32_t height = 0;
  xenos::TextureFormat format = xenos::TextureFormat::k_8_8_8_8;
  xenos::Endian endian = xenos::Endian::kNone;
  bool tiled = false;
  uint32_t row_pitch_bytes = 0;  // TextureGuestLayout::Level::row_pitch_bytes
  uint32_t x_extent_blocks = 0;
  uint32_t y_extent_blocks = 0;
};

class Registry {
 public:
  static Registry& Get();

  // False when neither dumping nor any replacement file is present; callers
  // skip hashing entirely in that case so the feature costs nothing.
  bool active() const { return dump_enabled_ || !files_.empty(); }
  bool dump_enabled() const { return dump_enabled_; }
  bool has_replacements() const { return !files_.empty(); }

  uint64_t Hash(const GuestBaseView& view) const;

  // Decoded replacement for the hash, or nullptr. Decoding happens on first
  // use and the result (including failure) is cached. `view` is needed for
  // overlays (the guest base level is decoded and composited under them).
  const Image* Find(uint64_t hash, const GuestBaseView* view);

  // Writes the base level as PNG once per hash. Unsupported formats are
  // logged once and skipped.
  void Dump(uint64_t hash, const GuestBaseView& view);

  // Decodes the guest base level to RGBA8. Returns false for unsupported
  // formats. Exposed for tools/tests.
  static bool DecodeGuestBase(const GuestBaseView& view, std::vector<uint8_t>& rgba_out);

 private:
  Registry();
  void Scan();

  std::filesystem::path root_;
  std::filesystem::path dump_dir_;
  bool dump_enabled_ = false;
  std::mutex mutex_;
  struct File {
    std::filesystem::path path;
    bool overlay = false;
  };
  std::unordered_map<uint64_t, File> files_;
  std::unordered_map<uint64_t, std::unique_ptr<Image>> images_;
  std::unordered_set<uint64_t> dumped_;
  std::unordered_set<uint32_t> unsupported_formats_logged_;
};

}  // namespace rex::graphics::texture_replacement
