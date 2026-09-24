// [new_fix_24092026]
// Tests for the native shader cache lookup (src/native/native_cache_path.h,
// GitHub issue #33). Plain executable, exit code 0 = pass. Build target
// dp_native_cache_path_test. Works in a temporary directory it removes.
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

#include "../src/native/native_cache_path.h"

namespace {

int g_failures = 0;

#define CHECK(cond)                                               \
  do {                                                            \
    if (!(cond)) {                                                \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      ++g_failures;                                               \
    }                                                             \
  } while (0)

namespace fs = std::filesystem;
using namespace dp::native;

// Writes `content`; the short labels of the layout cases ("zip", "new", ...)
// get the cache header in front so they count as valid caches.
void Touch(const fs::path& p, const char* content, int age_hours) {
  fs::create_directories(p.parent_path());
  const std::string c = content;
  const bool label = !c.empty() && c.size() <= 3 && c != "DPN";
  std::ofstream(p, std::ios::binary) << (label ? "DPNS0001" + std::string(8, '\0') + c : c);
  fs::last_write_time(p, fs::file_time_type::clock::now() - std::chrono::hours(age_hours));
}

// Each case gets a fresh install directory.
fs::path Fresh(const fs::path& root, const char* name) {
  const fs::path base = root / name;
  fs::remove_all(base);
  fs::create_directories(base);
  return base;
}

void Run(const fs::path& root) {
  {  // Launcher-updated install before the fix: no cache anywhere.
    const fs::path b = Fresh(root, "none");
    CHECK(!FindShaderCache(b).has_value());
  }
  {  // Zip install: native\ only.
    const fs::path b = Fresh(root, "zip");
    Touch(ShaderCacheZipPath(b), "zip", 1);
    CHECK(FindShaderCache(b) == ShaderCacheZipPath(b));
  }
  {  // Updated with an old launcher after the fix: shareable copy only.
    const fs::path b = Fresh(root, "updater");
    Touch(ShaderCacheUpdaterPath(b), "upd", 1);
    CHECK(FindShaderCache(b) == ShaderCacheUpdaterPath(b));
  }
  {  // Stale native\ from an older zip, newer copy from the update: the newer wins.
    const fs::path b = Fresh(root, "stale_zip");
    Touch(ShaderCacheZipPath(b), "old", 72);
    Touch(ShaderCacheUpdaterPath(b), "new", 1);
    CHECK(FindShaderCache(b) == ShaderCacheUpdaterPath(b));
  }
  {  // A newer zip extracted by hand over an install that has an older shareable copy.
    const fs::path b = Fresh(root, "stale_shareable");
    Touch(ShaderCacheZipPath(b), "new", 1);
    Touch(ShaderCacheUpdaterPath(b), "old", 72);
    CHECK(FindShaderCache(b) == ShaderCacheZipPath(b));
  }
  {  // Same time (both from one zip): the zip layout wins the tie.
    const fs::path b = Fresh(root, "tie");
    Touch(ShaderCacheZipPath(b), "a", 5);
    Touch(ShaderCacheUpdaterPath(b), "a", 5);
    fs::last_write_time(ShaderCacheUpdaterPath(b), fs::last_write_time(ShaderCacheZipPath(b)));
    CHECK(FindShaderCache(b) == ShaderCacheZipPath(b));
  }
  {  // A truncated newer copy (interrupted update) loses to the valid older one.
    const fs::path b = Fresh(root, "truncated_newer");
    Touch(ShaderCacheZipPath(b), "DPNS0001........payload", 72);
    Touch(ShaderCacheUpdaterPath(b), "DPN", 1);
    CHECK(FindShaderCache(b) == ShaderCacheZipPath(b));
  }
  {  // An empty file with a valid name is not a cache.
    const fs::path b = Fresh(root, "empty");
    Touch(ShaderCacheUpdaterPath(b), "", 1);
    CHECK(!FindShaderCache(b).has_value());
  }
  {  // Exe folder first; the working directory only when the exe folder has none.
    const fs::path exe = Fresh(root, "exe_dir"), wd = Fresh(root, "work_dir");
    Touch(ShaderCacheZipPath(wd), "wd", 1);
    CHECK(FindShaderCache(exe, wd) == ShaderCacheZipPath(wd));
    Touch(ShaderCacheZipPath(exe), "exe", 72);
    CHECK(FindShaderCache(exe, wd) == ShaderCacheZipPath(exe));
    CHECK(!FindShaderCache(Fresh(root, "exe_empty"), fs::path()).has_value());
  }
  {  // A directory with the cache's name is not a cache.
    const fs::path b = Fresh(root, "dir");
    fs::create_directories(ShaderCacheZipPath(b));
    CHECK(!FindShaderCache(b).has_value());
  }
}

}  // namespace

int main() {
  const fs::path root = fs::temp_directory_path() / "dp_native_cache_path_test";
  fs::remove_all(root);
  Run(root);
  std::error_code ec;
  fs::remove_all(root, ec);
  if (g_failures) {
    std::printf("dp_native_cache_path_test: %d failure(s)\n", g_failures);
    return 1;
  }
  std::printf("dp_native_cache_path_test: all checks passed\n");
  return 0;
}
