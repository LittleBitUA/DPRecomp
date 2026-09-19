/**
 ******************************************************************************
 * ReXGlue - content container directory operations                          *
 ******************************************************************************
 */

#include <rex/system/xam/content_dir_ops.h>

#include <algorithm>
#include <thread>

namespace rex {
namespace system {
namespace xam {

namespace {

// True when `dir` no longer exists (the only success condition: a directory
// whose files are gone but whose own entry is still delete-pending cannot be
// recreated under the same name).
bool Gone(const std::filesystem::path& dir) {
  std::error_code ec;
  return std::filesystem::status(dir, ec).type() == std::filesystem::file_type::not_found;
}

}  // namespace

RemoveDirResult RemoveDirWithRetry(const std::filesystem::path& dir,
                                   std::chrono::milliseconds budget,
                                   std::chrono::milliseconds step, const SleepFn& sleep) {
  RemoveDirResult result;
  if (step.count() <= 0) {
    step = std::chrono::milliseconds(1);
  }
  const SleepFn do_sleep = sleep ? sleep : SleepFn([](std::chrono::milliseconds ms) {
    std::this_thread::sleep_for(ms);
  });

  for (;;) {
    ++result.attempts;
    std::error_code ec;
    // remove_all deletes what it can and reports the first failure; the
    // directory itself stays while any file inside is held open elsewhere.
    std::filesystem::remove_all(dir, ec);
    if (ec) {
      result.last_error = ec;
    }
    if (Gone(dir)) {
      result.removed = true;
      result.still_directory = false;
      return result;
    }
    if (!ec) {
      // No error but the entry is still there: delete-pending directory.
      result.last_error = std::make_error_code(std::errc::directory_not_empty);
    }
    if (result.waited >= budget) {
      break;
    }
    const auto wait = std::min(step, budget - result.waited);
    do_sleep(wait);
    result.waited += wait;
  }

  std::error_code ec;
  result.still_directory = std::filesystem::is_directory(dir, ec);
  result.removed = false;
  return result;
}

}  // namespace xam
}  // namespace system
}  // namespace rex
