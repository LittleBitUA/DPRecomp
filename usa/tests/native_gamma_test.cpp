// [new_fix_24092026]
// Tests for the native renderer's capture of the console's display gamma ramp
// (src/native/native_gamma.h). Plain executable, exit code 0 = pass. Build
// target dp_native_gamma_test.
#include <cstdint>
#include <cstdio>

#include "../src/native/native_gamma.h"

namespace {

int g_failures = 0;

#define CHECK(cond)                                               \
  do {                                                            \
    if (!(cond)) {                                                \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      ++g_failures;                                               \
    }                                                             \
  } while (0)

using namespace dp::native;

void TestIdentity() {
  GuestGammaRamp r;
  CHECK(r.IsIdentity());
  CHECK(r.version() == 0);
  CHECK(r.table()[0] == 0);
  CHECK(Lut30Red(r.table()[255]) == 0x3FF && Lut30Green(r.table()[255]) == 0x3FF && Lut30Blue(r.table()[255]) == 0x3FF);
  CHECK(Lut30Red(r.table()[128]) == 128u * 0x3FF / 0xFF);
}

// The XDK's 256-entry set-up: RW_MODE 0, RW_INDEX 0, WRITE_EN_MASK 7, then
// DC_LUT_30_COLOR per entry with an automatic index increment.
void TestTableWrites() {
  GuestGammaRamp r;
  r.Write(kRegDcLutRwMode, 0);
  r.Write(kRegDcLutRwIndex, 0);
  r.Write(kRegDcLutWriteEnMask, 7);
  CHECK(!r.Write(kRegDcLut30Color, 0));  // entry 0 already 0: no change
  CHECK(r.Write(kRegDcLut30Color, PackLut30(72, 72, 72)));  // entry 1
  CHECK(r.Write(kRegDcLut30Color, PackLut30(89, 89, 89)));  // entry 2
  CHECK(Lut30Green(r.table()[1]) == 72 && Lut30Blue(r.table()[2]) == 89);
  CHECK(!r.IsIdentity());
  CHECK(r.version() == 2);
  // Masked write: only red.
  r.Write(kRegDcLutRwIndex, 10);
  r.Write(kRegDcLutWriteEnMask, 4);
  r.Write(kRegDcLut30Color, PackLut30(500, 1, 2));
  CHECK(Lut30Red(r.table()[10]) == 500);
  CHECK(Lut30Green(r.table()[10]) == 10u * 0x3FF / 0xFF);
  // Index wraps at 256.
  r.Write(kRegDcLutWriteEnMask, 7);
  r.Write(kRegDcLutRwIndex, 255);
  r.Write(kRegDcLut30Color, PackLut30(1, 1, 1));
  r.Write(kRegDcLut30Color, PackLut30(3, 3, 3));
  CHECK(Lut30Red(r.table()[255]) == 1 && Lut30Red(r.table()[0]) == 3);
}

// DC_LUT_SEQ_COLOR: red, green, blue for one entry, then the next entry.
void TestSequentialWrites() {
  GuestGammaRamp r;
  r.Write(kRegDcLutRwIndex, 5);
  r.Write(kRegDcLutSeqColor, 100u << 6);
  r.Write(kRegDcLutSeqColor, 200u << 6);
  r.Write(kRegDcLutSeqColor, 300u << 6);
  CHECK(Lut30Red(r.table()[5]) == 100 && Lut30Green(r.table()[5]) == 200 && Lut30Blue(r.table()[5]) == 300);
  r.Write(kRegDcLutSeqColor, 7u << 6);  // entry 6, red
  CHECK(Lut30Red(r.table()[6]) == 7);
  // RW_INDEX resets the component: this goes to red of entry 20.
  r.Write(kRegDcLutRwIndex, 20);
  r.Write(kRegDcLutSeqColor, 9u << 6);
  CHECK(Lut30Red(r.table()[20]) == 9);
}

void TestPwlModeIgnoresTable() {
  GuestGammaRamp r;
  r.Write(kRegDcLutRwMode, 1);
  CHECK(!r.Write(kRegDcLut30Color, PackLut30(9, 9, 9)));
  r.Write(kRegDcLutPwlData, 0x12345678);
  CHECK(r.IsIdentity() && r.pwl_writes() == 1);
}

void TestHostPacking() {
  // R10G10B10A2_UNORM puts red in bits 0-9; DC_LUT_30_COLOR puts blue there.
  const uint32_t v = PackLut30(1, 2, 3);
  const uint32_t h = Lut30ToR10G10B10A2(v);
  CHECK((h & 0x3FF) == 1 && ((h >> 10) & 0x3FF) == 2 && ((h >> 20) & 0x3FF) == 3 && (h >> 30) == 3);
}

}  // namespace

int main() {
  TestIdentity();
  TestTableWrites();
  TestSequentialWrites();
  TestPwlModeIgnoresTable();
  TestHostPacking();
  if (g_failures) {
    std::printf("dp_native_gamma_test: %d failure(s)\n", g_failures);
    return 1;
  }
  std::printf("dp_native_gamma_test: all checks passed\n");
  return 0;
}
