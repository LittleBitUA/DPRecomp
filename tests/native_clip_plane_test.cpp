// [new_fix_03102026_clipplane]
// Tests for the user clip plane location in the XDK device (src/native/native_clip_plane.h).
// Plain executable, exit code 0 = pass. Build target dp_native_clip_plane_test.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "../src/native/native_clip_plane.h"

namespace {

int g_failures = 0;

#define CHECK(cond)                                               \
  do {                                                            \
    if (!(cond)) {                                                \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      ++g_failures;                                               \
    }                                                             \
  } while (0)

// Device block layout the native renderer reads (native_renderer.cpp kDev*).
constexpr uint32_t kDevPsConstants = 6016;    // 256 x float4
constexpr uint32_t kDevBoolConstants = 10112;
constexpr uint32_t kDevLoopConstants = 10144;  // 32 dwords
constexpr uint32_t kDevPaClClipCntl = 10564;
constexpr uint32_t kOldWrongOffset = 8528;     // used up to 2.0.12

void PutBE(std::vector<uint8_t>& d, uint32_t off, float f) {
  uint32_t u;
  std::memcpy(&u, &f, 4);
  d[off] = uint8_t(u >> 24);
  d[off + 1] = uint8_t(u >> 16);
  d[off + 2] = uint8_t(u >> 8);
  d[off + 3] = uint8_t(u);
}

float GetBE(const std::vector<uint8_t>& d, uint32_t off) {
  const uint32_t u = (uint32_t(d[off]) << 24) | (uint32_t(d[off + 1]) << 16) | (uint32_t(d[off + 2]) << 8) | d[off + 3];
  float f;
  std::memcpy(&f, &u, 4);
  return f;
}

// What PAL sub_824CDB80 / USA sub_824CD2D0 do: addi r11,r4,642; rlwinm r11,r11,4,0,27; add r11,r11,r3; 4 x stw.
void GuestSetClipPlane(std::vector<uint8_t>& dev, uint32_t index, const float plane[4]) {
  const uint32_t at = ((index + 642) << 4) & 0xFFFFFFF0u;
  for (int i = 0; i < 4; ++i) PutBE(dev, at + i * 4, plane[i]);
}

}  // namespace

int main() {
  using dp::native::DevClipPlaneOffset;
  using dp::native::kDevClipPlaneCount;

  // Plane 0 sits right after the loop constants, planes 1..5 follow.
  CHECK(DevClipPlaneOffset(0) == 10272);
  CHECK(DevClipPlaneOffset(0) == kDevLoopConstants + 32 * 4);
  CHECK(DevClipPlaneOffset(5) + 16 <= kDevPaClClipCntl);

  // None of them overlaps the shader constant banks.
  for (uint32_t i = 0; i < kDevClipPlaneCount; ++i) {
    CHECK(DevClipPlaneOffset(i) >= kDevLoopConstants + 128);
    CHECK(DevClipPlaneOffset(i) >= kDevBoolConstants);
  }

  // The bug: the old address is pixel shader constant c157, not a clip plane.
  CHECK(kOldWrongOffset >= kDevPsConstants && kOldWrongOffset < kDevBoolConstants);
  CHECK((kOldWrongOffset - kDevPsConstants) % 16 == 0);
  CHECK((kOldWrongOffset - kDevPsConstants) / 16 == 157);
  CHECK(kOldWrongOffset != DevClipPlaneOffset(0));

  // The guest setter and the reader agree: Emily's mirror frame (#46, f11191),
  // plane 0 and what c157 held at the same draw.
  {
    std::vector<uint8_t> dev(24576, 0);
    const float mirror[4] = {-0.461f, 0.023f, 65.933f, -65.235f};
    const float c157[4] = {-0.508f, -0.23f, -1.202f, 5.31f};
    for (int i = 0; i < 4; ++i) PutBE(dev, kOldWrongOffset + i * 4, c157[i]);
    GuestSetClipPlane(dev, 0, mirror);
    for (int i = 0; i < 4; ++i) {
      CHECK(GetBE(dev, DevClipPlaneOffset(0) + i * 4) == mirror[i]);
      CHECK(GetBE(dev, kOldWrongOffset + i * 4) == c157[i]);  // the old read saw this instead
    }
    // Plane 1 does not disturb plane 0.
    const float other[4] = {1.0f, 2.0f, 3.0f, 4.0f};
    GuestSetClipPlane(dev, 1, other);
    CHECK(GetBE(dev, DevClipPlaneOffset(0)) == mirror[0]);
    CHECK(GetBE(dev, DevClipPlaneOffset(1) + 12) == 4.0f);
  }

  if (g_failures == 0) std::printf("dp_native_clip_plane_test: all passed\n");
  return g_failures == 0 ? 0 : 1;
}
