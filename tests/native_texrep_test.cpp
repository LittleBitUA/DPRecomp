// [NEW FABLE VERSION] 2026-09-23
// Tests for the native renderer's texture replacement rules
// (src/native/native_texrep.h): they must match the SDK's GPU plugin
// (pipeline/texture/replacement.cpp) so the launcher's textures\<hash>.png and
// .overlay.png files apply identically on both render paths. Plain executable,
// exit code 0 = pass. Build target dp_native_texrep_test.
#include <cstdint>
#include <cstdio>
#include <vector>

#include "../src/native/native_texrep.h"

namespace {

int g_failures = 0;

#define CHECK(cond)                                               \
  do {                                                            \
    if (!(cond)) {                                                \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      ++g_failures;                                               \
    }                                                             \
  } while (0)

using namespace dp::native::texrep;

void TestNames() {
  uint64_t h = 0;
  bool ov = true;
  CHECK(ParseName("6E42BC7CF738BCF0", &h, &ov) && h == 0x6E42BC7CF738BCF0ull && !ov);
  CHECK(ParseName("2D1098B531AA9CA8.overlay", &h, &ov) && h == 0x2D1098B531AA9CA8ull && ov);
  CHECK(ParseName("2d1098b531aa9ca8.OVERLAY", &h, &ov) && h == 0x2D1098B531AA9CA8ull && ov);
  CHECK(ParseName("51AA9FBD20140C6D_1024x1024_k_DXT2_3", &h, &ov) && h == 0x51AA9FBD20140C6Dull && !ov);
  CHECK(!ParseName("51AA9FBD20140C6", &h, &ov));   // 15 digits
  CHECK(!ParseName("51AA9FBD2014XC6D", &h, &ov));  // not hex
}

void TestSeed() {
  // The SDK: (width << 40) ^ (height << 20) ^ format.
  CHECK(HashSeed(256, 256, 20) == ((uint64_t(256) << 40) ^ (uint64_t(256) << 20) ^ 20u));
  CHECK(HashSeed(1024, 512, 20) != HashSeed(512, 1024, 20));
  CHECK(HashSeed(1024, 1024, 19) != HashSeed(1024, 1024, 20));
}

void TestOverlay() {
  // Guest 1x1 red; overlay 2x2: opaque green, erase, pass-through, alpha 2.
  const uint8_t guest[4] = {255, 0, 0, 255};
  uint8_t ov[16] = {0, 255, 0, 255,  9, 9, 9, 1,  7, 7, 7, 0,  1, 2, 3, 2};
  ComposeOverlay(guest, 1, 1, ov, 2, 2);
  CHECK(ov[0] == 0 && ov[1] == 255 && ov[2] == 0 && ov[3] == 255);    // overlay kept
  CHECK(ov[4] == 0 && ov[5] == 0 && ov[6] == 0 && ov[7] == 0);        // alpha 1 erases
  CHECK(ov[8] == 255 && ov[9] == 0 && ov[10] == 0 && ov[11] == 255);  // alpha 0 shows the guest
  CHECK(ov[12] == 1 && ov[13] == 2 && ov[14] == 3 && ov[15] == 2);    // alpha 2 kept
}

void TestMips() {
  Image img;
  img.width = 4;
  img.height = 2;
  img.levels.push_back(std::vector<uint8_t>(4 * 2 * 4, 200));
  BuildMips(img);
  CHECK(img.levels.size() == 3);  // 4x2, 2x1, 1x1
  CHECK(img.LevelWidth(1) == 2 && img.LevelHeight(1) == 1);
  CHECK(img.levels[2].size() == 4 && img.levels[2][0] == 200);
}

void TestDecode() {
  // BC1 block: c0 = white (0xFFFF), c1 = black, all indices 0 -> white opaque.
  const uint8_t bc1[8] = {0xFF, 0xFF, 0x00, 0x00, 0, 0, 0, 0};
  std::vector<uint8_t> rgba;
  CHECK(DecodeBase(Codec::kDXT1, bc1, 8, 4, 4, rgba));
  CHECK(rgba.size() == 64 && rgba[0] == 255 && rgba[1] == 255 && rgba[2] == 255 && rgba[3] == 255);
  // BC3 alpha a0 = 255, a1 = 0, indices 1 -> alpha 0 everywhere; colour black.
  // Index 1 in every 3-bit slot: the bit string 001001... packs LSB first into
  // the repeating bytes 0x49 0x92 0x24.
  uint8_t bc3[16] = {255, 0, 0x49, 0x92, 0x24, 0x49, 0x92, 0x24};
  CHECK(DecodeBase(Codec::kDXT4_5, bc3, 16, 4, 4, rgba));
  bool all_transparent = true;
  for (int i = 0; i < 16; ++i) all_transparent = all_transparent && rgba[i * 4 + 3] == 0;
  CHECK(all_transparent && rgba[0] == 0);
  // 2x1 8888 with a padded row pitch.
  const uint8_t px[16] = {1, 2, 3, 4, 5, 6, 7, 8, 0xEE, 0xEE, 0xEE, 0xEE, 0xEE, 0xEE, 0xEE, 0xEE};
  CHECK(DecodeBase(Codec::k8888, px, 16, 2, 1, rgba));
  CHECK(rgba.size() == 8 && rgba[4] == 5 && rgba[7] == 8);
}

}  // namespace

int main() {
  TestNames();
  TestSeed();
  TestOverlay();
  TestMips();
  TestDecode();
  if (g_failures == 0) std::printf("dp_native_texrep_test: all checks passed\n");
  return g_failures == 0 ? 0 : 1;
}
