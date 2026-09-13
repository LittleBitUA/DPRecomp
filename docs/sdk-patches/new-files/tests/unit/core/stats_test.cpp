/**
 * @file        stats_test.cpp
 * @brief       Unit tests for rex::stats (gauges, providers, snapshot, ticker)
 *
 * DP1 2026-09-13.
 *
 * @license     BSD 3-Clause License
 */

#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <rex/cvar.h>
#include <rex/dbg.h>
#include <rex/stats.h>

REXCVAR_DECLARE(int32_t, log_stats_interval);

namespace {

struct StatsReset {
  StatsReset() { rex::stats::ResetForTesting(); }
  ~StatsReset() { rex::stats::ResetForTesting(); }
};

}  // namespace

TEST_CASE("stats: Set/Get round-trips and overwrites", "[stats]") {
  StatsReset reset;
  int64_t value = -1;
  REQUIRE_FALSE(rex::stats::Get("test/a", &value));
  REQUIRE(rex::stats::Set("test/a", 5));
  REQUIRE(rex::stats::Get("test/a", &value));
  REQUIRE(value == 5);
  REQUIRE(rex::stats::Set("test/a", 7));
  REQUIRE(rex::stats::Get("test/a", &value));
  REQUIRE(value == 7);
  REQUIRE(rex::stats::GaugeCount() == 1);
}

TEST_CASE("stats: equal text from different pointers shares one gauge", "[stats]") {
  StatsReset reset;
  std::string dynamic_name = "test/";
  dynamic_name += "shared";
  REQUIRE(rex::stats::Set("test/shared", 1));
  // The dynamic string must outlive the gauge table for this test, which the
  // fixture's ResetForTesting guarantees (names are cleared on reset).
  REQUIRE(rex::stats::Set(dynamic_name.c_str(), 2));
  REQUIRE(rex::stats::GaugeCount() == 1);
  int64_t value = 0;
  REQUIRE(rex::stats::Get("test/shared", &value));
  REQUIRE(value == 2);
}

TEST_CASE("stats: table full is reported, never overflowed", "[stats]") {
  StatsReset reset;
  // Names must outlive the table; keep them in a static vector.
  static std::vector<std::string> names;
  names.clear();
  names.reserve(rex::stats::kMaxGauges + 1);
  for (size_t i = 0; i <= rex::stats::kMaxGauges; ++i) {
    names.push_back("test/full/" + std::to_string(i));
  }
  for (size_t i = 0; i < rex::stats::kMaxGauges; ++i) {
    REQUIRE(rex::stats::Set(names[i].c_str(), int64_t(i)));
  }
  REQUIRE(rex::stats::GaugeCount() == rex::stats::kMaxGauges);
  REQUIRE_FALSE(rex::stats::Set(names[rex::stats::kMaxGauges].c_str(), 1));
  REQUIRE(rex::stats::GaugeCount() == rex::stats::kMaxGauges);
  // Existing gauges still update when the table is full.
  REQUIRE(rex::stats::Set(names[3].c_str(), 99));
  int64_t value = 0;
  REQUIRE(rex::stats::Get(names[3].c_str(), &value));
  REQUIRE(value == 99);
}

TEST_CASE("stats: null name is rejected", "[stats]") {
  StatsReset reset;
  REQUIRE_FALSE(rex::stats::Set(nullptr, 1));
  REQUIRE_FALSE(rex::stats::Get(nullptr, nullptr));
  REQUIRE(rex::stats::GaugeCount() == 0);
}

TEST_CASE("stats: COUNT_profile_set feeds the gauge table", "[stats]") {
  StatsReset reset;
  COUNT_profile_set("test/profile", 42u);
  int64_t value = 0;
  REQUIRE(rex::stats::Get("test/profile", &value));
  REQUIRE(value == 42);
}

TEST_CASE("stats: snapshot lists providers in registration order, then gauges", "[stats]") {
  StatsReset reset;
  REQUIRE(rex::stats::FormatSnapshot().empty());
  rex::stats::RegisterProvider("beta", [] { return std::string("b=1"); });
  rex::stats::RegisterProvider("alpha", [] { return std::string("a=1"); });
  rex::stats::RegisterProvider("empty", [] { return std::string(); });
  rex::stats::Set("test/x", 1);
  rex::stats::Set("test/y", -2);
  REQUIRE(rex::stats::FormatSnapshot() == "beta: b=1 | alpha: a=1 | gauges: test/x=1 test/y=-2");

  // Re-registering replaces in place; unregistering removes the group.
  rex::stats::RegisterProvider("beta", [] { return std::string("b=2"); });
  rex::stats::UnregisterProvider("alpha");
  REQUIRE(rex::stats::FormatSnapshot() == "beta: b=2 | gauges: test/x=1 test/y=-2");
  rex::stats::UnregisterProvider("does-not-exist");
  REQUIRE(rex::stats::FormatSnapshot() == "beta: b=2 | gauges: test/x=1 test/y=-2");
}

TEST_CASE("stats: a provider may unregister itself while a snapshot runs", "[stats]") {
  StatsReset reset;
  rex::stats::RegisterProvider("self", [] {
    rex::stats::UnregisterProvider("self");
    return std::string("once");
  });
  REQUIRE(rex::stats::FormatSnapshot() == "self: once");
  REQUIRE(rex::stats::FormatSnapshot().empty());
}

TEST_CASE("stats: concurrent Set from many threads keeps one slot per name", "[stats]") {
  StatsReset reset;
  constexpr int kThreads = 8;
  constexpr int kIterations = 2000;
  static const char* const kNames[] = {"test/t0", "test/t1", "test/t2", "test/t3"};
  std::atomic<bool> go{false};
  std::vector<std::thread> threads;
  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back([&, t] {
      while (!go.load()) {
        std::this_thread::yield();
      }
      for (int i = 0; i < kIterations; ++i) {
        REQUIRE(rex::stats::Set(kNames[(t + i) % 4], i));
      }
    });
  }
  go.store(true);
  for (auto& thread : threads) {
    thread.join();
  }
  REQUIRE(rex::stats::GaugeCount() == 4);
  // Each thread's last write to a given name happens at one of the final four
  // iterations, and any thread may be the last writer: the value is in
  // [kIterations - 4, kIterations - 1], never torn, never lost.
  for (const char* name : kNames) {
    int64_t value = -1;
    REQUIRE(rex::stats::Get(name, &value));
    REQUIRE(value >= kIterations - 4);
    REQUIRE(value <= kIterations - 1);
  }
}

TEST_CASE("stats: periodic thread starts, honours the interval cvar, and stops", "[stats]") {
  StatsReset reset;
  REQUIRE_FALSE(rex::stats::IsPeriodicLoggingRunning());
  std::atomic<int> calls{0};
  rex::stats::RegisterProvider("tick", [&] {
    ++calls;
    return std::string("x");
  });

  REXCVAR_SET(log_stats_interval, 1);
  rex::stats::StartPeriodicLogging();
  REQUIRE(rex::stats::IsPeriodicLoggingRunning());
  rex::stats::StartPeriodicLogging();  // idempotent
  std::this_thread::sleep_for(std::chrono::milliseconds(2600));
  rex::stats::StopPeriodicLogging();
  REQUIRE_FALSE(rex::stats::IsPeriodicLoggingRunning());
  rex::stats::StopPeriodicLogging();  // idempotent
  const int observed = calls.load();
  REQUIRE(observed >= 2);
  REQUIRE(observed <= 3);

  // Disabled: no snapshot is produced, but the thread keeps running so a
  // hot-reload can turn it back on.
  calls.store(0);
  REXCVAR_SET(log_stats_interval, 0);
  rex::stats::StartPeriodicLogging();
  std::this_thread::sleep_for(std::chrono::milliseconds(1500));
  REQUIRE(rex::stats::IsPeriodicLoggingRunning());
  rex::stats::StopPeriodicLogging();
  REQUIRE(calls.load() == 0);
  REXCVAR_SET(log_stats_interval, 60);
}
