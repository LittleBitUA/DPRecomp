// [new_fix_24092026] Where the native renderer's shader cache is looked up
// (GitHub issue #33).
//
// The release zip ships it as native\dp_native_shaders.bin. The launcher's
// built-in updater up to 2.0.1 copies a fixed list of files plus the folders
// prompts\, textures\ and userdata\cache\shaders\shareable\ - never native\.
// A player who updated with the launcher therefore had no cache at all (every
// draw skipped: black screen from the start) or the cache of the last zip they
// extracted by hand (shaders missing or outdated). The updater that performs an
// update is the OLD launcher, so a fixed launcher alone cannot reach them:
// from 2.0.2 on the zip also carries a copy in the shareable folder, which
// those updaters do copy, and the renderer takes the newer of the two files.
//
// Header-only and free of D3D so tests/native_cache_path_test.cpp can check it.
#pragma once

#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <system_error>

namespace dp::native {

inline constexpr const char* kShaderCacheName = "dp_native_shaders.bin";

// The places the cache can be, relative to the install directory, in the order
// that wins a tie.
inline std::filesystem::path ShaderCacheZipPath(const std::filesystem::path& base) {
  return base / "native" / kShaderCacheName;
}
inline std::filesystem::path ShaderCacheUpdaterPath(const std::filesystem::path& base) {
  return base / "userdata" / "cache" / "shaders" / "shareable" / kShaderCacheName;
}

// A file that starts with the cache's header (magic + two counts). A truncated
// or empty copy (interrupted update, full disk) must not win the lookup: the
// renderer would reject it and skip every draw.
inline bool LooksLikeShaderCache(const std::filesystem::path& p) {
  std::ifstream in(p, std::ios::binary);
  char head[16] = {};
  if (!in.read(head, sizeof(head))) return false;
  return std::memcmp(head, "DPNS0001", 8) == 0;
}

// The valid candidate with the latest modification time (the zip layout wins a
// tie); nothing when neither is valid. A stale native\ file left by an older
// zip loses to the copy a newer update delivered to the shareable folder.
inline std::optional<std::filesystem::path> FindShaderCache(const std::filesystem::path& base) {
  const std::filesystem::path candidates[] = {ShaderCacheZipPath(base), ShaderCacheUpdaterPath(base)};
  std::optional<std::filesystem::path> best;
  std::filesystem::file_time_type best_time{};
  for (const auto& c : candidates) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(c, ec) || ec) continue;
    if (!LooksLikeShaderCache(c)) continue;
    const auto t = std::filesystem::last_write_time(c, ec);
    if (ec) continue;
    if (!best || t > best_time) {
      best = c;
      best_time = t;
    }
  }
  return best;
}

// The exe folder first, then the working directory when it differs (a player's
// launcher sets it to the install folder; development runs use the dist).
inline std::optional<std::filesystem::path> FindShaderCache(const std::filesystem::path& exe_folder,
                                                            const std::filesystem::path& working_dir) {
  if (auto found = FindShaderCache(exe_folder)) return found;
  std::error_code ec;
  if (working_dir.empty() || std::filesystem::equivalent(exe_folder, working_dir, ec)) return std::nullopt;
  return FindShaderCache(working_dir);
}

// The working directory without the throwing overload (empty on failure).
inline std::filesystem::path WorkingDirOrEmpty() {
  std::error_code ec;
  auto p = std::filesystem::current_path(ec);
  return ec ? std::filesystem::path() : p;
}

}  // namespace dp::native
