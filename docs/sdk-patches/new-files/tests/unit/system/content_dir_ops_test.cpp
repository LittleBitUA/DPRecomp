/**
 * Unit tests for RemoveDirWithRetry (DPRecomp #21: save container overwrite
 * while another process holds a file inside the container directory).
 *
 * The "other process" is simulated with a Win32 handle opened WITHOUT
 * FILE_SHARE_DELETE on Windows; elsewhere the retry loop is exercised with an
 * injected sleep only.
 */

#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <rex/system/xam/content_dir_ops.h>

#ifdef _WIN32
#include <windows.h>
#endif

using namespace std::chrono_literals;
using rex::system::xam::RemoveDirResult;
using rex::system::xam::RemoveDirWithRetry;

namespace {

#ifdef _WIN32
unsigned long GetCurrentProcessIdCompat() { return ::GetCurrentProcessId(); }
#else
unsigned long GetCurrentProcessIdCompat() { return 0; }
#endif

std::filesystem::path MakeContainer(const char* tag) {
  static std::atomic<int> counter{0};
  const auto dir = std::filesystem::temp_directory_path() /
                   ("rexglue_content_dir_ops_" + std::string(tag) + "_" +
                    std::to_string(GetCurrentProcessIdCompat()) + "_" +
                    std::to_string(counter++));
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);
  for (const char* name : {"RwPreserve.txt", "__thumbnail.png"}) {
    FILE* f = std::fopen((dir / name).string().c_str(), "wb");
    REQUIRE(f != nullptr);
    std::fputs("save", f);
    std::fclose(f);
  }
  return dir;
}

}  // namespace

TEST_CASE("RemoveDirWithRetry removes a free directory on the first attempt", "[content_dir_ops]") {
  const auto dir = MakeContainer("free");
  std::vector<std::chrono::milliseconds> sleeps;
  const RemoveDirResult r = RemoveDirWithRetry(
      dir, 3000ms, 50ms, [&](std::chrono::milliseconds ms) { sleeps.push_back(ms); });
  CHECK(r.removed);
  CHECK_FALSE(r.still_directory);
  CHECK(r.attempts == 1);
  CHECK(sleeps.empty());
  CHECK(r.waited == 0ms);
  CHECK_FALSE(std::filesystem::exists(dir));
}

TEST_CASE("RemoveDirWithRetry reports a missing directory as removed", "[content_dir_ops]") {
  const auto dir = std::filesystem::temp_directory_path() / "rexglue_content_dir_ops_missing_dir";
  std::filesystem::remove_all(dir);
  const RemoveDirResult r = RemoveDirWithRetry(dir, 100ms, 10ms);
  CHECK(r.removed);
  CHECK(r.attempts == 1);
}

#ifdef _WIN32
namespace {

// Holds `file` open the way a scanner without FILE_SHARE_DELETE does, for
// `hold`, on its own thread.
class ForeignHolder {
 public:
  ForeignHolder(const std::filesystem::path& file, std::chrono::milliseconds hold) {
    handle_ = ::CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    REQUIRE(handle_ != INVALID_HANDLE_VALUE);
    thread_ = std::thread([this, hold] {
      std::this_thread::sleep_for(hold);
      Release();
    });
  }
  ~ForeignHolder() {
    if (thread_.joinable()) thread_.join();
    Release();
  }
  void Release() {
    HANDLE h = handle_.exchange(INVALID_HANDLE_VALUE);
    if (h != INVALID_HANDLE_VALUE) ::CloseHandle(h);
  }

 private:
  std::atomic<HANDLE> handle_{INVALID_HANDLE_VALUE};
  std::thread thread_;
};

}  // namespace

TEST_CASE("RemoveDirWithRetry waits out a foreign handle without share-delete", "[content_dir_ops]") {
  const auto dir = MakeContainer("held");
  // Regression baseline: a single remove_all fails while the file is held.
  {
    ForeignHolder holder(dir / "RwPreserve.txt", 400ms);
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    CHECK((ec || std::filesystem::exists(dir)));
    holder.Release();
  }
  // Recreate what the failed remove_all may have taken.
  std::filesystem::remove_all(dir);
  const auto dir2 = MakeContainer("held2");
  {
    ForeignHolder holder(dir2 / "RwPreserve.txt", 300ms);
    const RemoveDirResult r = RemoveDirWithRetry(dir2, 5000ms, 25ms);
    CHECK(r.removed);
    CHECK(r.attempts > 1);
    CHECK(r.waited >= 25ms);
    CHECK(r.waited < 5000ms);
    CHECK_FALSE(std::filesystem::exists(dir2));
  }
}

TEST_CASE("RemoveDirWithRetry gives up after the budget and leaves a reusable directory",
          "[content_dir_ops]") {
  const auto dir = MakeContainer("stuck");
  {
    ForeignHolder holder(dir / "RwPreserve.txt", 2000ms);
    const RemoveDirResult r = RemoveDirWithRetry(dir, 200ms, 50ms);
    CHECK_FALSE(r.removed);
    CHECK(r.still_directory);
    CHECK(r.attempts >= 4);
    CHECK(r.waited == 200ms);
    CHECK(static_cast<bool>(r.last_error));
    CHECK(std::filesystem::is_directory(dir));
    holder.Release();
  }
  std::filesystem::remove_all(dir);
}
#endif
