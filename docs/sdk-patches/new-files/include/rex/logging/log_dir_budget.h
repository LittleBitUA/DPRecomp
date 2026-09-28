/**
 * @file        rex/logging/log_dir_budget.h
 *
 * @brief       [new_fix_28092026_upstream] Keeps the logs directory bounded.
 *
 * Port of upstream PruneLogDirectory (rexglue b971840, 2026-09-11), adapted
 * to our rotating file sink: a run is every `<app>_NNN.log` together with its
 * rotated parts `<app>_NNN.1.log` ... `<app>_NNN.K.log`, and the oldest runs
 * are removed as a whole until the directory is under the budget. Minidumps
 * (`<app>_YYYYMMDD_HHMMSS_PID.dmp`, ~40 MB each) are kept to the newest few.
 *
 * Header-only and free of runtime dependencies so the unit tests can drive it
 * against a temporary directory.
 */
#pragma once

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace rex::logging {

// `deadlyprem_012.log` and `deadlyprem_012.3.log` -> 12; anything else -> nullopt.
inline std::optional<int> LogRunNumber(const std::filesystem::path& file, std::string_view prefix) {
  if (file.extension() != ".log") {
    return std::nullopt;
  }
  const std::string stem = file.stem().string();
  if (stem.size() <= prefix.size() || stem.compare(0, prefix.size(), prefix) != 0) {
    return std::nullopt;
  }
  std::string_view digits(stem);
  digits.remove_prefix(prefix.size());
  digits = digits.substr(0, digits.find('.'));
  if (digits.empty()) {
    return std::nullopt;
  }
  int number = 0;
  auto [ptr, ec] = std::from_chars(digits.data(), digits.data() + digits.size(), number);
  if (ec != std::errc() || ptr != digits.data() + digits.size()) {
    return std::nullopt;
  }
  return number;
}

struct PruneResult {
  uint32_t removed_runs = 0;
  uint32_t removed_files = 0;
  uint64_t removed_bytes = 0;
  uint64_t kept_bytes = 0;
};

// Removes whole runs, oldest (lowest number) first, until the run logs take at
// most `budget_bytes`. budget 0 = unlimited. Called before the new run's file
// is created, so nothing removed here is open.
inline PruneResult PruneLogDirectory(const std::filesystem::path& logs_dir, std::string_view app_name,
                                     uint64_t budget_bytes) {
  PruneResult result;
  const std::string prefix = std::string(app_name) + "_";
  std::map<int, std::vector<std::pair<std::filesystem::path, uint64_t>>> runs;
  uint64_t total = 0;
  std::error_code ec;
  for (const auto& entry : std::filesystem::directory_iterator(logs_dir, ec)) {
    std::error_code entry_ec;
    if (!entry.is_regular_file(entry_ec)) {
      continue;
    }
    auto run = LogRunNumber(entry.path(), prefix);
    if (!run) {
      continue;
    }
    const uint64_t bytes = entry.file_size(entry_ec);
    if (entry_ec) {
      continue;
    }
    runs[*run].emplace_back(entry.path(), bytes);
    total += bytes;
  }
  if (budget_bytes != 0) {
    for (const auto& [run, files] : runs) {
      if (total <= budget_bytes) {
        break;
      }
      bool removed_any = false;
      for (const auto& [file, bytes] : files) {
        std::error_code rm_ec;
        if (std::filesystem::remove(file, rm_ec)) {
          total -= bytes;
          result.removed_bytes += bytes;
          ++result.removed_files;
          removed_any = true;
        }
      }
      if (removed_any) {
        ++result.removed_runs;
      }
    }
  }
  result.kept_bytes = total;
  return result;
}

// Keeps the newest `keep` minidumps of the app (by modification time; the
// timestamp in the name breaks ties). keep < 0 = keep all.
inline uint32_t PruneMinidumps(const std::filesystem::path& dump_dir, std::string_view app_name,
                               int keep) {
  if (keep < 0) {
    return 0;
  }
  const std::string prefix = std::string(app_name) + "_";
  struct Dump {
    std::filesystem::path path;
    std::filesystem::file_time_type time;
  };
  std::vector<Dump> dumps;
  std::error_code ec;
  for (const auto& entry : std::filesystem::directory_iterator(dump_dir, ec)) {
    std::error_code entry_ec;
    if (!entry.is_regular_file(entry_ec) || entry.path().extension() != ".dmp") {
      continue;
    }
    const std::string name = entry.path().filename().string();
    if (name.compare(0, prefix.size(), prefix) != 0) {
      continue;
    }
    dumps.push_back({entry.path(), entry.last_write_time(entry_ec)});
  }
  if (dumps.size() <= size_t(keep)) {
    return 0;
  }
  std::sort(dumps.begin(), dumps.end(), [](const Dump& a, const Dump& b) {
    if (a.time != b.time) return a.time > b.time;
    return a.path.filename() > b.path.filename();
  });
  uint32_t removed = 0;
  for (size_t i = size_t(keep); i < dumps.size(); ++i) {
    std::error_code rm_ec;
    if (std::filesystem::remove(dumps[i].path, rm_ec)) {
      ++removed;
    }
  }
  return removed;
}

}  // namespace rex::logging
