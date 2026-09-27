// [new_fix_27092026_i39]
// Tests for guest thread stack sizes (src/deadlyprem_thread_stack.h). Plain
// executable, exit code 0 = pass. Build target dp_thread_stack_test.
#include <cstdint>
#include <cstdio>

#include "../src/deadlyprem_thread_stack.h"

namespace {

int g_failures = 0;

#define CHECK(cond)                                               \
  do {                                                            \
    if (!(cond)) {                                                \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      ++g_failures;                                               \
    }                                                             \
  } while (0)

constexpr uint32_t KB = 1024u;

}  // namespace

int main() {
  using dp::ScaledGuestStackSize;

  // The game's own requests at the default scale 4.
  CHECK(ScaledGuestStackSize(256 * KB, 4) == 1024 * KB);  // GameThread
  CHECK(ScaledGuestStackSize(384 * KB, 4) == 1536 * KB);  // PhysicsThread
  CHECK(ScaledGuestStackSize(128 * KB, 4) == 512 * KB);   // RenderThread
  CHECK(ScaledGuestStackSize(64 * KB, 4) == 256 * KB);    // MapThread

  // #39: GameThread had 241 KB in use when sub_825A9AA8 asked for another
  // 957 * 280 bytes. 256 KB could not hold it; the scaled stack can.
  const uint32_t in_use = 0x70410000u - 0x703D3B00u;
  const uint32_t alloca_bytes = 957u * 252u + 957u * 28u;
  CHECK(in_use + alloca_bytes > 256 * KB);
  CHECK(in_use + alloca_bytes < ScaledGuestStackSize(256 * KB, 4));

  // 0 = "use the XEX default", left to ExCreateThread.
  CHECK(ScaledGuestStackSize(0, 4) == 0);

  // Scale 1 (or nonsense below it) = the console's size.
  CHECK(ScaledGuestStackSize(256 * KB, 1) == 256 * KB);
  CHECK(ScaledGuestStackSize(256 * KB, 0) == 256 * KB);
  CHECK(ScaledGuestStackSize(256 * KB, -3) == 256 * KB);

  // Cap at 4 MB, never below the request, no 32-bit overflow.
  CHECK(ScaledGuestStackSize(2048 * KB, 4) == 4096 * KB);
  CHECK(ScaledGuestStackSize(8192 * KB, 4) == 8192 * KB);
  CHECK(ScaledGuestStackSize(0x80000000u, 16) == 0x80000000u);
  CHECK(ScaledGuestStackSize(256 * KB, 1000) == 4096 * KB);
  CHECK(ScaledGuestStackSize(16 * KB, 16) == 256 * KB);

  if (g_failures == 0) std::printf("dp_thread_stack_test: all passed\n");
  return g_failures == 0 ? 0 : 1;
}
