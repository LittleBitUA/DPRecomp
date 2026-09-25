// [new_fix_24092026]
// Tests for the guest vertex/index buffer header decoding
// (src/native/native_buffer_header.h, GitHub issue #35). Plain executable,
// exit code 0 = pass. Build target dp_native_buffer_header_test.
#include <cstdint>
#include <cstdio>

#include "../src/native/native_buffer_header.h"

namespace {

int g_failures = 0;

#define CHECK(cond)                                               \
  do {                                                            \
    if (!(cond)) {                                                \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      ++g_failures;                                               \
    }                                                             \
  } while (0)

using dp::native::BufferHeader;
using dp::native::DecodeBufferHeader;

void TestVertexBuffer() {
  // Fetch constant: address 0x1F0A0000 with fetch type 3, 100 dwords, 8in32.
  const BufferHeader h = DecodeBufferHeader(false, 0, 0x1F0A0003u, (100u << 2) | 2u);
  CHECK(h.address == 0x1F0A0000u);
  CHECK(h.size == 400u);
  CHECK(h.endian == 2u);
  CHECK(!h.index32);
}

void TestIndexBufferOddCount() {
  // The pillow in York's room (Crowley9's hotel save): 217 16-bit indices = 434
  // bytes. The vertex formula gave 432 and dropped the last index.
  const uint32_t common = (1u << 29);  // 8in16, 16-bit
  const BufferHeader h = DecodeBufferHeader(true, common, 0x44FEA680u, 434u);
  CHECK(h.size == 434u);
  CHECK(h.size / 2 == 217u);
  CHECK(h.endian == 1u);
  CHECK(!h.index32);
  // What 2.0.3 computed, kept here as the regression's shape.
  CHECK(((434u >> 2) & 0xFFFFFFu) * 4u == 432u);
}

void TestIndexBufferAddressAndFormat() {
  // A sub-allocated index buffer at a 2-byte boundary keeps its address.
  const BufferHeader h = DecodeBufferHeader(true, 0x80000000u | (2u << 29), 0x44FEA682u, 1200u);
  CHECK(h.address == 0x44FEA682u);
  CHECK(h.index32);
  CHECK(h.endian == 2u);
  CHECK(h.size == 1200u);
}

void TestEveryOddIndexCountSurvives() {
  for (uint32_t n = 1; n < 5000; ++n) {
    const BufferHeader h16 = DecodeBufferHeader(true, 1u << 29, 0x40000000u, n * 2u);
    const BufferHeader h32 = DecodeBufferHeader(true, 0x80000000u | (2u << 29), 0x40000000u, n * 4u);
    if (h16.size / 2 != n || h32.size / 4 != n) {
      CHECK(false);
      std::printf("  index count %u lost indices (16-bit %u, 32-bit %u)\n", n, h16.size / 2, h32.size / 4);
      break;
    }
  }
}

}  // namespace

int main() {
  TestVertexBuffer();
  TestIndexBufferOddCount();
  TestIndexBufferAddressAndFormat();
  TestEveryOddIndexCountSurvives();
  if (g_failures) {
    std::printf("%d check(s) failed\n", g_failures);
    return 1;
  }
  std::printf("native_buffer_header_test: all passed\n");
  return 0;
}
