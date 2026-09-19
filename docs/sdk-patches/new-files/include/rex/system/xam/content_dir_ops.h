/**
 ******************************************************************************
 * ReXGlue - content container directory operations                          *
 ******************************************************************************
 *
 * Host-side helpers behind XamContentCreate(CREATE_ALWAYS) / XamContentDelete.
 * Pure std::filesystem, no kernel state: unit-tested on their own.
 *
 * Why this exists (DPRecomp #21, 2026-09-19): a save container is a host
 * directory. Overwriting it means removing the old directory first, and on
 * Windows that fails for a few seconds whenever another process holds a file
 * inside it (real-time antivirus scanning the 9 MB save it just saw, a search
 * indexer, a backup tool). A single std::filesystem::remove_all then returns
 * an error, the runtime reported ERROR_ACCESS_DENIED to the game, and Deadly
 * Premonition's save routine maps every error except "device not connected"
 * and "write protected" to "keep waiting" - the "Saving" screen never ends.
 */

#pragma once

#include <chrono>
#include <filesystem>
#include <functional>
#include <system_error>

namespace rex {
namespace system {
namespace xam {

struct RemoveDirResult {
  /// The directory is gone (removed here, or it did not exist).
  bool removed = false;
  /// The directory still exists and is a directory (a caller may reuse it).
  bool still_directory = false;
  /// remove_all attempts made (1 = no retry was needed).
  int attempts = 0;
  /// Last error reported by the filesystem, if any.
  std::error_code last_error;
  /// Total time spent waiting between attempts.
  std::chrono::milliseconds waited{0};
};

/// Sleep hook (injected by tests; default = std::this_thread::sleep_for).
using SleepFn = std::function<void(std::chrono::milliseconds)>;

/// Removes `dir` recursively. On failure retries every `step` until `budget`
/// has elapsed, then gives up and reports what is left. Never throws.
RemoveDirResult RemoveDirWithRetry(const std::filesystem::path& dir,
                                   std::chrono::milliseconds budget,
                                   std::chrono::milliseconds step,
                                   const SleepFn& sleep = SleepFn());

}  // namespace xam
}  // namespace system
}  // namespace rex
