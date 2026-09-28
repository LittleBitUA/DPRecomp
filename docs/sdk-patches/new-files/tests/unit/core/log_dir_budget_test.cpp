// [new_fix_28092026_upstream] Logs directory budget (port of upstream b971840
// adapted to the rotating sink) and minidump pruning.
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

#include <catch2/catch_test_macros.hpp>

#include <rex/logging/log_dir_budget.h>

using namespace rex::logging;
namespace fs = std::filesystem;

namespace {

struct TempDir {
  fs::path path;
  explicit TempDir(const char* name) {
    path = fs::temp_directory_path() / name;
    fs::remove_all(path);
    fs::create_directories(path);
  }
  ~TempDir() {
    std::error_code ec;
    fs::remove_all(path, ec);
  }
};

void WriteBytes(const fs::path& p, size_t n) {
  std::ofstream f(p, std::ios::binary);
  std::string s(n, 'x');
  f.write(s.data(), std::streamsize(s.size()));
}

}  // namespace

TEST_CASE("log_dir_budget: run numbers include rotated parts", "[log_dir_budget]") {
  CHECK(LogRunNumber("deadlyprem_012.log", "deadlyprem_") == 12);
  CHECK(LogRunNumber("deadlyprem_012.3.log", "deadlyprem_") == 12);
  CHECK(LogRunNumber("deadlyprem_001.1.log", "deadlyprem_") == 1);
  CHECK_FALSE(LogRunNumber("deadlyprem_abc.log", "deadlyprem_").has_value());
  CHECK_FALSE(LogRunNumber("deadlyprem_.log", "deadlyprem_").has_value());
  CHECK_FALSE(LogRunNumber("deadlyprem_012.txt", "deadlyprem_").has_value());
  CHECK_FALSE(LogRunNumber("other_012.log", "deadlyprem_").has_value());
  CHECK_FALSE(LogRunNumber("deadlyprem_20260927_163826_26824.dmp", "deadlyprem_").has_value());
}

TEST_CASE("log_dir_budget: oldest whole runs go first", "[log_dir_budget]") {
  TempDir dir("rex_log_dir_budget_runs");
  // run 1: 300 bytes in two parts, run 2: 300, run 3: 100
  WriteBytes(dir.path / "deadlyprem_001.log", 100);
  WriteBytes(dir.path / "deadlyprem_001.1.log", 200);
  WriteBytes(dir.path / "deadlyprem_002.log", 300);
  WriteBytes(dir.path / "deadlyprem_003.log", 100);
  WriteBytes(dir.path / "notes.txt", 5000);                       // not ours
  WriteBytes(dir.path / "deadlyprem_20260927_1_2.dmp", 5000);    // a dump, not a run

  auto r = PruneLogDirectory(dir.path, "deadlyprem", 450);
  CHECK(r.removed_runs == 1);
  CHECK(r.removed_files == 2);
  CHECK(r.removed_bytes == 300);
  CHECK(r.kept_bytes == 400);
  CHECK_FALSE(fs::exists(dir.path / "deadlyprem_001.log"));
  CHECK_FALSE(fs::exists(dir.path / "deadlyprem_001.1.log"));
  CHECK(fs::exists(dir.path / "deadlyprem_002.log"));
  CHECK(fs::exists(dir.path / "deadlyprem_003.log"));
  CHECK(fs::exists(dir.path / "notes.txt"));
  CHECK(fs::exists(dir.path / "deadlyprem_20260927_1_2.dmp"));
}

TEST_CASE("log_dir_budget: numeric order, not name order; 0 = unlimited", "[log_dir_budget]") {
  TempDir dir("rex_log_dir_budget_order");
  WriteBytes(dir.path / "deadlyprem_999.log", 100);
  WriteBytes(dir.path / "deadlyprem_1000.log", 100);
  auto keep_all = PruneLogDirectory(dir.path, "deadlyprem", 0);
  CHECK(keep_all.removed_files == 0);
  CHECK(keep_all.kept_bytes == 200);
  auto r = PruneLogDirectory(dir.path, "deadlyprem", 150);
  CHECK(r.removed_runs == 1);
  CHECK_FALSE(fs::exists(dir.path / "deadlyprem_999.log"));
  CHECK(fs::exists(dir.path / "deadlyprem_1000.log"));
}

TEST_CASE("log_dir_budget: missing directory is harmless", "[log_dir_budget]") {
  auto r = PruneLogDirectory(fs::temp_directory_path() / "rex_log_dir_budget_missing_xyz", "deadlyprem", 1);
  CHECK(r.removed_files == 0);
  CHECK(PruneMinidumps(fs::temp_directory_path() / "rex_log_dir_budget_missing_xyz", "deadlyprem", 1) == 0);
}

TEST_CASE("log_dir_budget: minidumps keep the newest", "[log_dir_budget]") {
  TempDir dir("rex_log_dir_budget_dumps");
  const auto now = fs::file_time_type::clock::now();
  for (int i = 0; i < 5; ++i) {
    auto p = dir.path / ("deadlyprem_2026092" + std::to_string(i) + "_120000_1.dmp");
    WriteBytes(p, 10);
    fs::last_write_time(p, now - std::chrono::hours(10 - i));  // i = 4 is the newest
  }
  WriteBytes(dir.path / "other_20260920_120000_1.dmp", 10);
  CHECK(PruneMinidumps(dir.path, "deadlyprem", -1) == 0);
  CHECK(PruneMinidumps(dir.path, "deadlyprem", 2) == 3);
  CHECK(fs::exists(dir.path / "deadlyprem_20260924_120000_1.dmp"));
  CHECK(fs::exists(dir.path / "deadlyprem_20260923_120000_1.dmp"));
  CHECK_FALSE(fs::exists(dir.path / "deadlyprem_20260922_120000_1.dmp"));
  CHECK_FALSE(fs::exists(dir.path / "deadlyprem_20260920_120000_1.dmp"));
  CHECK(fs::exists(dir.path / "other_20260920_120000_1.dmp"));
  CHECK(PruneMinidumps(dir.path, "deadlyprem", 2) == 0);
}
