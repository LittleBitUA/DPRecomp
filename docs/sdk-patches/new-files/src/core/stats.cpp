/**
 * @file        stats.cpp
 * @brief       Process-wide gauges and the periodic "[stats]" log line
 *
 * DP1 2026-09-13. See include/rex/stats.h.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/stats.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

#include <fmt/format.h>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/platform.h>

#if REX_PLATFORM_WIN32
#include "platform_win.h"
#include <psapi.h>
#endif

REXCVAR_DEFINE_INT32(log_stats_interval, 60, "Log",
                     "Seconds between periodic [stats] lines in the log (guest heaps, kernel "
                     "objects, GPU caches, VRAM, process memory). 0 = off")
    .range(0, 3600)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace rex::stats {

namespace {

// Gauges: a fixed table of (name, value) slots. A slot is claimed once by a
// compare-and-swap on the name pointer and never freed, so readers never see
// a slot change identity. Values are plain atomics. Name lookup compares the
// pointer first (string literals reused from the same call site) and falls
// back to strcmp so two literals with equal text share one slot.
struct GaugeSlot {
  std::atomic<const char*> name{nullptr};
  std::atomic<int64_t> value{0};
};

GaugeSlot g_gauges[kMaxGauges];
std::atomic<size_t> g_gauge_count{0};

GaugeSlot* FindGauge(const char* name) {
  const size_t count = g_gauge_count.load(std::memory_order_acquire);
  for (size_t i = 0; i < count; ++i) {
    const char* slot_name = g_gauges[i].name.load(std::memory_order_acquire);
    if (slot_name == nullptr) {
      // A slot whose index is below count but whose name is not published yet
      // is being claimed by another thread right now; it cannot be ours.
      continue;
    }
    if (slot_name == name || std::strcmp(slot_name, name) == 0) {
      return &g_gauges[i];
    }
  }
  return nullptr;
}

std::mutex g_claim_mutex;

GaugeSlot* ClaimGauge(const char* name) {
  std::lock_guard<std::mutex> lock(g_claim_mutex);
  // Re-check under the lock: another thread may have claimed it meanwhile.
  if (GaugeSlot* existing = FindGauge(name)) {
    return existing;
  }
  const size_t index = g_gauge_count.load(std::memory_order_acquire);
  if (index >= kMaxGauges) {
    return nullptr;
  }
  g_gauges[index].value.store(0, std::memory_order_relaxed);
  g_gauges[index].name.store(name, std::memory_order_release);
  g_gauge_count.store(index + 1, std::memory_order_release);
  return &g_gauges[index];
}

// Providers.
struct ProviderEntry {
  std::string group;
  Provider provider;
};
std::mutex g_provider_mutex;
std::vector<ProviderEntry> g_providers;

// Periodic thread.
std::mutex g_thread_mutex;
std::condition_variable g_thread_cv;
std::thread g_thread;
bool g_thread_running = false;
bool g_thread_stop = false;

#if REX_PLATFORM_WIN32
std::string DescribeProcess() {
  PROCESS_MEMORY_COUNTERS_EX counters = {};
  counters.cb = sizeof(counters);
  std::string out;
  if (GetProcessMemoryInfo(GetCurrentProcess(),
                           reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
                           sizeof(counters))) {
    out += fmt::format("ws {} MB private {} MB peak-ws {} MB", counters.WorkingSetSize >> 20,
                       counters.PrivateUsage >> 20, counters.PeakWorkingSetSize >> 20);
  }
  DWORD handle_count = 0;
  if (GetProcessHandleCount(GetCurrentProcess(), &handle_count)) {
    out += fmt::format("{}handles {}", out.empty() ? "" : " ", handle_count);
  }
  MEMORYSTATUSEX status = {};
  status.dwLength = sizeof(status);
  if (GlobalMemoryStatusEx(&status)) {
    out += fmt::format("{}sys-free {} MB commit-free {} MB", out.empty() ? "" : " ",
                       status.ullAvailPhys >> 20, status.ullAvailPageFile >> 20);
  }
  return out;
}
#endif

void ThreadMain() {
  for (;;) {
    int interval = REXCVAR_GET(log_stats_interval);
    if (interval <= 0) {
      // Disabled: poll the cvar slowly so a hot-reload can turn it on.
      interval = 5;
    }
    {
      std::unique_lock<std::mutex> lock(g_thread_mutex);
      if (g_thread_cv.wait_for(lock, std::chrono::seconds(interval),
                               [] { return g_thread_stop; })) {
        return;
      }
    }
    if (REXCVAR_GET(log_stats_interval) > 0) {
      LogSnapshot();
    }
  }
}

}  // namespace

bool Set(const char* name, int64_t value) {
  if (name == nullptr) {
    return false;
  }
  GaugeSlot* slot = FindGauge(name);
  if (slot == nullptr) {
    slot = ClaimGauge(name);
    if (slot == nullptr) {
      return false;
    }
  }
  slot->value.store(value, std::memory_order_relaxed);
  return true;
}

bool Get(const char* name, int64_t* out_value) {
  if (name == nullptr) {
    return false;
  }
  GaugeSlot* slot = FindGauge(name);
  if (slot == nullptr) {
    return false;
  }
  if (out_value != nullptr) {
    *out_value = slot->value.load(std::memory_order_relaxed);
  }
  return true;
}

size_t GaugeCount() { return g_gauge_count.load(std::memory_order_acquire); }

void RegisterProvider(const char* group, Provider provider) {
  if (group == nullptr || !provider) {
    return;
  }
  std::lock_guard<std::mutex> lock(g_provider_mutex);
  for (auto& entry : g_providers) {
    if (entry.group == group) {
      entry.provider = std::move(provider);
      return;
    }
  }
  g_providers.push_back({group, std::move(provider)});
}

void UnregisterProvider(const char* group) {
  if (group == nullptr) {
    return;
  }
  std::lock_guard<std::mutex> lock(g_provider_mutex);
  for (auto it = g_providers.begin(); it != g_providers.end(); ++it) {
    if (it->group == group) {
      g_providers.erase(it);
      return;
    }
  }
}

std::string FormatSnapshot() {
  std::string out;
  // Copy the provider list so a provider may (un)register providers, and so
  // the mutex is not held while a provider runs.
  std::vector<ProviderEntry> providers;
  {
    std::lock_guard<std::mutex> lock(g_provider_mutex);
    providers = g_providers;
  }
  for (const auto& entry : providers) {
    std::string text = entry.provider();
    if (text.empty()) {
      continue;
    }
    if (!out.empty()) {
      out += " | ";
    }
    out += entry.group;
    out += ": ";
    out += text;
  }
  const size_t count = g_gauge_count.load(std::memory_order_acquire);
  bool first_gauge = true;
  for (size_t i = 0; i < count; ++i) {
    const char* name = g_gauges[i].name.load(std::memory_order_acquire);
    if (name == nullptr) {
      continue;
    }
    if (first_gauge) {
      if (!out.empty()) {
        out += " | ";
      }
      out += "gauges: ";
      first_gauge = false;
    } else {
      out += ' ';
    }
    fmt::format_to(std::back_inserter(out), "{}={}", name,
                   g_gauges[i].value.load(std::memory_order_relaxed));
  }
  return out;
}

void LogSnapshot() { REXLOG_INFO("[stats] {}", FormatSnapshot()); }

void StartPeriodicLogging() {
  std::lock_guard<std::mutex> lock(g_thread_mutex);
  if (g_thread_running) {
    return;
  }
#if REX_PLATFORM_WIN32
  RegisterProvider("proc", DescribeProcess);
#endif
  g_thread_stop = false;
  g_thread_running = true;
  g_thread = std::thread(ThreadMain);
}

void StopPeriodicLogging() {
  std::thread thread;
  {
    std::lock_guard<std::mutex> lock(g_thread_mutex);
    if (!g_thread_running) {
      return;
    }
    g_thread_stop = true;
    g_thread_running = false;
    thread = std::move(g_thread);
  }
  g_thread_cv.notify_all();
  if (thread.joinable()) {
    thread.join();
  }
}

bool IsPeriodicLoggingRunning() {
  std::lock_guard<std::mutex> lock(g_thread_mutex);
  return g_thread_running;
}

void ResetForTesting() {
  StopPeriodicLogging();
  {
    std::lock_guard<std::mutex> lock(g_provider_mutex);
    g_providers.clear();
  }
  std::lock_guard<std::mutex> lock(g_claim_mutex);
  const size_t count = g_gauge_count.load(std::memory_order_acquire);
  // Publish the count first so readers stop scanning before names vanish.
  g_gauge_count.store(0, std::memory_order_release);
  for (size_t i = 0; i < count; ++i) {
    g_gauges[i].name.store(nullptr, std::memory_order_release);
    g_gauges[i].value.store(0, std::memory_order_relaxed);
  }
}

}  // namespace rex::stats
