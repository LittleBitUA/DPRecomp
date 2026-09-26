// [new_fix_27092026_health] Tests for the native renderer health log helpers
// (src/native/native_health.h). Plain executable, exit code 0 = pass.
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "../src/native/native_health.h"

namespace {
int g_failures = 0;
#define CHECK(cond)                                               \
  do {                                                            \
    if (!(cond)) {                                                \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      ++g_failures;                                               \
    }                                                             \
  } while (0)
bool Has(const char* s, const char* part) { return std::strstr(s, part) != nullptr; }
}  // namespace

int main() {
  using namespace dp::native;
  // The #38 case: the sun cascades 1024x1024 x5 at 3x.
  CHECK(Has(CreateFailureHint(true, 3072, 3072, 5, kHrInvalidArg), "3D texture limit"));
  CHECK(Has(CreateFailureHint(true, 2048, 2048, 5, kHrOutOfMemory), "out of video memory"));  // fits: memory
  CHECK(Has(CreateFailureHint(false, 20480, 1152, 1, kHrInvalidArg), "2D texture limit"));
  CHECK(Has(CreateFailureHint(false, 4096, 4096, 1, kHrOutOfMemory), "out of video memory"));
  CHECK(Has(CreateFailureHint(false, 4096, 4096, 1, kHrDeviceRemoved), "lost"));
  CHECK(Has(CreateFailureHint(false, 4096, 4096, 1, kHrInvalidArg), "rejected"));
  CHECK(Has(CreateFailureHint(false, 64, 64, 1, 0x1234), "unknown"));

  // Limiter: two per key, then silence; bounded keys.
  {
    LogLimiter l(2, 3);
    bool last = false;
    CHECK(l.Allow(0x44D112F0, &last) && !last);
    CHECK(l.Allow(0x44D112F0, &last) && last);
    CHECK(!l.Allow(0x44D112F0));
    for (int i = 0; i < 1000; ++i) CHECK(!l.Allow(0x44D112F0));
    CHECK(l.Allow(1) && l.Allow(2));
    CHECK(!l.Allow(3));  // key cap reached
    CHECK(l.suppressed() == 1002);
  }

  // Churn: five recreations in one frame (#38) -> reported once, on the third.
  {
    ChurnDetector c;
    CHECK(!c.Note(0x44D112F0, 100));
    CHECK(!c.Note(0x44D112F0, 100));
    CHECK(c.Note(0x44D112F0, 100) && c.last_in_frame() == 3);
    CHECK(!c.Note(0x44D112F0, 100));
    for (uint32_t f = 101; f < 5000; ++f) CHECK(!c.Note(0x44D112F0, f));  // reported once only
  }
  // Churn over a window: once per frame for ten frames -> reported.
  {
    ChurnDetector c;
    bool hit = false;
    for (uint32_t f = 0; f < 10; ++f) hit = c.Note(7, f) || hit;
    CHECK(hit && c.last_in_window() == 10);
  }
  // Normal life: a texture recreated once per level load (every few thousand frames) is never reported.
  {
    ChurnDetector c;
    bool hit = false;
    for (uint32_t f = 0; f < 100000; f += 3000) hit = c.Note(9, f) || hit;
    CHECK(!hit);
    // two in one frame (a scale fallback + the resolve) is still fine
    CHECK(!c.Note(10, 5) && !c.Note(10, 5));
  }

  HealthCounters h;
  CHECK(!h.any());
  h.null_binds = 1;
  CHECK(h.any());

  if (g_failures) {
    std::printf("%d check(s) failed\n", g_failures);
    return 1;
  }
  std::printf("native_health_test: all passed\n");
  return 0;
}
