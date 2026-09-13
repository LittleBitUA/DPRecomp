/**
 * @file        stats.h
 * @brief       Process-wide gauges and periodic "[stats]" log line
 *
 * DP1 2026-09-13: every subsystem that already publishes a Tracy plot through
 * COUNT_profile_set also lands here, so the numbers exist in release builds
 * and in the log file, not only in a live profiler. On top of the gauges,
 * subsystems register providers that are evaluated on demand (guest heap
 * usage, kernel object / thread counts, VRAM budget, process memory). A
 * background thread prints one "[stats]" line every log_stats_interval
 * seconds. The thread never touches the game thread, so the line keeps
 * coming while the game is hung, which is exactly when it is needed.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

namespace rex::stats {

// Maximum number of distinct gauge names. Names past the limit are dropped
// (Set() returns false) rather than growing an unbounded table from hot paths.
constexpr size_t kMaxGauges = 128;

// Stores the latest value for `name`. Lock-free after the first call for a
// given name; `name` must outlive the process (a string literal). Safe from
// any thread. Returns false only when the gauge table is full.
bool Set(const char* name, int64_t value);

// Reads a gauge. Returns false when no such gauge was ever set.
bool Get(const char* name, int64_t* out_value);

// Number of gauges currently defined.
size_t GaugeCount();

// A provider produces the text for one named group in the stats line, e.g.
// "heaps" -> "v00 12/256 MB, v40 3/1024 MB". It runs on the stats thread, so
// it must never block on a lock the game can hold indefinitely: use try_lock
// and report "busy" instead. An empty result omits the group.
using Provider = std::function<std::string()>;

// Registers (or replaces) the provider for `group`. Group order in the line
// follows registration order.
void RegisterProvider(const char* group, Provider provider);
void UnregisterProvider(const char* group);

// Builds the line body: provider groups first, then every gauge in the order
// it was first set, "name=value" separated by spaces. No leading "[stats]".
std::string FormatSnapshot();

// Logs "[stats] <snapshot>" at INFO on the core category, right now.
void LogSnapshot();

// Starts the background thread that calls LogSnapshot() every
// log_stats_interval seconds (cvar, 0 disables; re-read every tick so it can
// be hot-reloaded). Idempotent. Stop() joins the thread; both are safe to call
// repeatedly.
void StartPeriodicLogging();
void StopPeriodicLogging();
bool IsPeriodicLoggingRunning();

// Test hook: drops every gauge and provider. Not for production code.
void ResetForTesting();

}  // namespace rex::stats
