// [NEW FABLE VERSION] 2026-09-22
// Tests for the native renderer's internal resolution rules
// (src/native/native_scale.h). Plain executable, no framework: exit code 0 =
// pass. Build target dp_native_scale_test (see CMakeLists.txt).
#include <cstdint>
#include <cstdio>
#include <initializer_list>

#include "../src/native/native_scale.h"

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

void TestClamp() {
  CHECK(ClampScale(0) == 1);
  CHECK(ClampScale(-3) == 1);
  CHECK(ClampScale(1) == 1);
  CHECK(ClampScale(2) == 2);
  CHECK(ClampScale(4) == 4);
  CHECK(ClampScale(5) == 4);
  CHECK(ClampScale(1000) == 4);
}

// The sizes DP1 actually creates (from the surface list of a native run):
// scene 1024x576, output 1280x720, reflection 512x288, post copies 512x288 /
// 256x144 / 128x128 / 256x256, shadow maps 1024x1024 and 512x512, and the
// luminance ladder 256x64 -> 32x32 -> 1x1 plus a 256x32 and a 64x64 mask.
void TestTargetSizes() {
  const uint32_t s = 2;
  CHECK(ScaleForTarget(1024, 576, s) == 2);   // scene
  CHECK(ScaleForTarget(1280, 720, s) == 2);   // final image and the main depth
  CHECK(ScaleForTarget(512, 288, s) == 2);    // reflection pass
  CHECK(ScaleForTarget(1024, 1024, s) == 2);  // sun shadow cascades
  CHECK(ScaleForTarget(512, 512, s) == 2);    // local light shadow map
  CHECK(ScaleForTarget(256, 144, s) == 2);    // half-size post copy
  CHECK(ScaleForTarget(128, 128, s) == 2);    // normals target

  CHECK(ScaleForTarget(256, 64, s) == 1);  // luminance ladder head
  CHECK(ScaleForTarget(256, 32, s) == 1);
  CHECK(ScaleForTarget(32, 32, s) == 1);
  CHECK(ScaleForTarget(64, 64, s) == 1);
  CHECK(ScaleForTarget(1, 1, s) == 1);  // the 1x1 average must stay 1x1

  // At 1x nothing is scaled, whatever the size.
  CHECK(ScaleForTarget(1024, 576, 1) == 1);
  CHECK(ScaleForTarget(32, 32, 1) == 1);
}

void TestBindScale() {
  CHECK(BindScale(true, 2, true, 2) == 2);
  CHECK(BindScale(true, 2, true, 1) == 1);  // depth fell back to 1x
  CHECK(BindScale(true, 1, true, 2) == 1);  // colour fell back to 1x
  CHECK(BindScale(true, 3, false, 0) == 3);
  CHECK(BindScale(false, 0, true, 3) == 3);
  CHECK(BindScale(false, 0, false, 0) == 1);
}

void TestViewportAndScissor() {
  const ViewportRect guest = {0.0f, 0.0f, 1024.0f, 576.0f};
  CHECK(ScaleViewport(guest, 1) == guest);
  CHECK((ScaleViewport(guest, 2) == ViewportRect{0.0f, 0.0f, 2048.0f, 1152.0f}));
  // The minimap box: an offset viewport scales with its origin.
  const ViewportRect box = {76.0f, 368.0f, 232.0f, 200.0f};
  CHECK((ScaleViewport(box, 2) == ViewportRect{152.0f, 736.0f, 464.0f, 400.0f}));

  const ScissorRect guest_scissor = {76, 368, 308, 568};
  CHECK(ScaleScissor(guest_scissor, 1) == guest_scissor);
  CHECK((ScaleScissor(guest_scissor, 2) == ScissorRect{152, 736, 616, 1136}));
  CHECK((ScaleScissor({0, 0, 1280, 720}, 3) == ScissorRect{0, 0, 3840, 2160}));
}

void TestMemoryEstimate() {
  // Square law, and 1x has to be the amount the renderer already used.
  const uint64_t one = EstimateScaledBytes(1);
  CHECK(one > 0);
  CHECK(EstimateScaledBytes(2) == one * 4);
  CHECK(EstimateScaledBytes(4) == one * 16);
  // A sanity band: the 1x set is tens of megabytes, 4x is under 2 GB.
  CHECK(one > 20ull * 1024 * 1024);
  CHECK(one < 100ull * 1024 * 1024);
  CHECK(EstimateScaledBytes(4) < 2ull * 1024 * 1024 * 1024);
}

// [NEW FABLE VERSION] 2026-09-23 regression: 0x10 (FRAGMENT0) used to be read
// as CLEARRENDERTARGET, which wiped the light buffer's G (DoF depth) mid-pass.
void TestResolveFlags() {
  for (uint32_t f : {0x00u, 0x10u, 0x14u, 0x50u, 0x70u, 0x20u, 0x24u}) {
    CHECK(!ResolveClearsTarget(f));
    CHECK(!ResolveClearsDepth(f));
  }
  CHECK(ResolveIsDepth(0x14));
  CHECK(!ResolveIsDepth(0x10));
  CHECK(ResolveClearsTarget(0x110));
  CHECK(ResolveClearsDepth(0x214));
  CHECK(!ResolveClearsTarget(0x214));
}

}  // namespace

// [NEW FABLE VERSION] 2026-09-23: the D3DFORMATs DP1 creates surfaces with (log
// of a native run: "Native renderer: surface ... format ...").
void TestSurfaceFormats() {
  CHECK(ClassifySurfaceFormat(0x1A22AB60) == SurfaceClass::kRgba16F);  // post chain, k_16_16_16_16_FLOAT (was RGBA8)
  CHECK(ClassifySurfaceFormat(0x18280186) == SurfaceClass::kRgba8);    // A8R8G8B8
  CHECK(ClassifySurfaceFormat(0x18280086) == SurfaceClass::kRgba8);
  CHECK(ClassifySurfaceFormat(0x2DA2ABA4) == SurfaceClass::kR32F);     // luminance ladder
  CHECK(ClassifySurfaceFormat(0x1A2201BF) == SurfaceClass::kRgba16F);  // 7e3 scene
  CHECK(ClassifySurfaceFormat(0x2D200196) == SurfaceClass::kDepth24);
  CHECK(ClassifySurfaceFormat(0x1A220197) == SurfaceClass::kDepthF24);
  CHECK(ClassifySurfaceFormat(31) == SurfaceClass::kRg16F);            // k_16_16_FLOAT, not 16_16_16_16
  CHECK(ClassifySurfaceFormat(7) == SurfaceClass::kRgba8);             // unmapped -> fallback...
  CHECK(!IsKnownSurfaceFormat(7));                                     // ...and reported
  for (uint32_t f : {0x1A22AB60u, 0x18280186u, 0x2DA2ABA4u, 0x1A2201BFu, 0x2D200196u, 0x1A220197u}) {
    CHECK(IsKnownSurfaceFormat(f));
  }
  CHECK(SurfaceIs64bpp(0x1A22AB60));
  CHECK(!SurfaceIs64bpp(0x1A2201BF));  // 7e3 is 32 bpp: DP1 re-binds its tiles as 8888 at the same pitch
  CHECK(!SurfaceIs64bpp(0x18280186));
}

// Tile footprints cross-checked with the emulator's EDRAM dumps
// (docs/hdr_audit_2026-09-14/game_pipeline.md: 234 tiles for 512x288 16F,
// 63 for 256x144 16F).
void TestEdram() {
  CHECK(EdramTileSpan(1024, 576, false) == 13u * 36u);
  CHECK(EdramTileSpan(1280, 720, false) == 16u * 45u);
  CHECK(EdramTileSpan(512, 288, true) == 234u);
  CHECK(EdramTileSpan(256, 144, true) == 63u);
  CHECK(EdramTileSpan(32, 32, false) == 2u);
  CHECK(EdramOverlap(720, 468, 720, 720));    // scene and the 1280x720 output
  CHECK(EdramOverlap(720, 468, 1100, 10));
  CHECK(!EdramOverlap(720, 468, 1188, 10));   // right after the scene
  CHECK(!EdramOverlap(1440, 234, 720, 720));  // reflection at 1440 vs output 720..1439
  // The tone map's re-bind: 7e3 scene -> 8888 on the same tiles.
  CHECK(EdramTransferKind(0x1A2201BF, 0x18280186, true) == EdramTransfer::kSaturate7e3);
  CHECK(EdramTransferKind(0x18280186, 0x18280086, true) == EdramTransfer::kCopy);  // tiled bit differs only
  CHECK(EdramTransferKind(0x1A22AB60, 0x1A22AB60, true) == EdramTransfer::kCopy);
  CHECK(EdramTransferKind(0x18280186, 0x1A2201BF, true) == EdramTransfer::kNone);  // 8888 -> 7e3
  CHECK(EdramTransferKind(0x1A2201BF, 0x18280186, false) == EdramTransfer::kNone);  // other size / pitch
}

// [NEW FABLE VERSION] 2026-09-24: newest writer of a texture's memory wins
// (dp_native_resolve_alias).
void TestResolveAlias() {
  CHECK(ResolveIsNewestWriter(0, 5, true));    // uploaded once, resolved since: the resolve's data
  CHECK(!ResolveIsNewestWriter(6, 5, true));   // the CPU rewrote it after the resolve: the CPU data
  CHECK(!ResolveIsNewestWriter(0, 0, true));   // never resolved
  CHECK(!ResolveIsNewestWriter(0, 5, false));  // another size or format views that memory differently
}

int main() {
  TestClamp();
  TestTargetSizes();
  TestBindScale();
  TestViewportAndScissor();
  TestMemoryEstimate();
  TestResolveFlags();
  TestSurfaceFormats();
  TestEdram();
  TestResolveAlias();
  if (g_failures == 0) std::printf("dp_native_scale_test: all checks passed\n");
  return g_failures == 0 ? 0 : 1;
}
