// Deadly Premonition Recompilation - native renderer hooks (milestone 1).
//
// Every hook here replaces a recompiled XDK D3D function (the generated
// `sub_X` is a weak alias; this strong definition wins at link time, and the
// original body stays reachable as `__imp__sub_X`). With the native renderer
// off the hook forwards to the original at once, so the emulated path costs a
// single predictable branch per call and nothing else changes.
//
// Two kinds of hooks:
//  * "observe": Create* / Set* / Lock / Unlock / Release call the original
//    (guest objects and the device shadow stay authentic; D3DX and the game
//    read them) and then tell the renderer what happened;
//  * "replace": Swap, the three draw entries, Resolve, Clear, the GPR
//    allocation buffer and the fence wait never run the XDK body: the ring
//    buffer is not consumed by anything (NativeGraphicsSystem drains it).
//
// Addresses (PAL): docs/native_render_scout_2026-09-14/d3d_surface.md with the
// 18.09 corrections (docs/native_render_design_2026-09-18.md section 1).

#include <atomic>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/memory/utils.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xthread.h>

#include "deadlyprem_pch.h"
#include "native/native_graphics_system.h"
#include "native/native_renderer.h"

// ---------------------------------------------------------------------------
// XDK entry names per region (2026-09-20). DPX_<PAL address> expands to the
// PAL or the USA function; the USA names come from find_usa_func.py (the XDK
// block sits 0x8B0 lower in the USA XEX, the Draw* entries 0x5E8 higher). The
// bodies are instruction-identical, so the argument layouts carry over 1:1.
// ---------------------------------------------------------------------------
#define DP_NATIVE_IMP2(name) __imp__##name
#define DP_NATIVE_IMP(name) DP_NATIVE_IMP2(name)
#define DP_NATIVE_STR2(name) #name
#define DP_NATIVE_STR(name) DP_NATIVE_STR2(name)
#define DP_NATIVE_FN(name) name
#if defined(DP_REGION_USA)
#define DPX_824D9B18 sub_824D9268  // [new_fix_24092026] XDK gamma ramp dispatcher (PAL - 0x8B0, same body)
#define DPX_824D6010 sub_824D5760  // [new_fix_24092026] XDK gamma ramp writer, 256-entry table
#define DPX_824D6108 sub_824D5858  // [new_fix_24092026] XDK gamma ramp writer, PWL
#define DPX_824CD6E0 sub_824CCE30
#define DPX_824CD888 sub_824CCFD8
#define DPX_824CDF30 sub_824CD680
#define DPX_824CE3B0 sub_824CDB00
#define DPX_824CE688 sub_824CDDD8
#define DPX_824CE7C8 sub_824CDF18
#define DPX_824CE7E0 sub_824CDF30
#define DPX_824CFA28 sub_824CF178
#define DPX_824D01B8 sub_824CF908
#define DPX_824D0358 sub_824CFAB0
#define DPX_824D04F8 sub_824CFC48
#define DPX_824D0618 sub_824CFD68
#define DPX_824D07E8 sub_824CFF38
#define DPX_824D0878 sub_824CFFC8
#define DPX_824D15E0 sub_824D0D30
#define DPX_824D17A0 sub_824D0EF0
#define DPX_824D1970 sub_824D10C0
#define DPX_824D1A78 sub_824D11C8
#define DPX_824D1AF8 sub_824D1248
#define DPX_824D1D20 sub_824D1470
#define DPX_824D1F08 sub_824D1658
#define DPX_824D2658 sub_824D1DA8
#define DPX_824D2E78 sub_824D25C8
#define DPX_824D4B60 sub_824D42B0
#define DPX_824D4C38 sub_824D4388
#define DPX_824D4CE8 sub_824D4438
#define DPX_824D5760 sub_824D4EB0
#define DPX_824D5AA8 sub_824D51F8
#define DPX_824D5B78 sub_824D52C8
#define DPX_824DA330 sub_824D9A80
#define DPX_824E5DB8 sub_824E5508
#define DPX_824E7E70 sub_824E75C0
#define DPX_824E7F98 sub_824E76E8
#define DPX_825CD7A0 sub_825CDD88
#define DPX_825CD7E8 sub_825CDDD0
#define DPX_825CDBD8 sub_825CE1C0
#else
#define DPX_824CD6E0 sub_824CD6E0
#define DPX_824CD888 sub_824CD888
#define DPX_824CDF30 sub_824CDF30
#define DPX_824CE3B0 sub_824CE3B0
#define DPX_824CE688 sub_824CE688
#define DPX_824CE7C8 sub_824CE7C8
#define DPX_824CE7E0 sub_824CE7E0
#define DPX_824CFA28 sub_824CFA28
#define DPX_824D01B8 sub_824D01B8
#define DPX_824D0358 sub_824D0358
#define DPX_824D04F8 sub_824D04F8
#define DPX_824D0618 sub_824D0618
#define DPX_824D07E8 sub_824D07E8
#define DPX_824D0878 sub_824D0878
#define DPX_824D15E0 sub_824D15E0
#define DPX_824D17A0 sub_824D17A0
#define DPX_824D1970 sub_824D1970
#define DPX_824D1A78 sub_824D1A78
#define DPX_824D1AF8 sub_824D1AF8
#define DPX_824D1D20 sub_824D1D20
#define DPX_824D1F08 sub_824D1F08
#define DPX_824D2658 sub_824D2658
#define DPX_824D2E78 sub_824D2E78
#define DPX_824D4B60 sub_824D4B60
#define DPX_824D4C38 sub_824D4C38
#define DPX_824D4CE8 sub_824D4CE8
#define DPX_824D5760 sub_824D5760
#define DPX_824D5AA8 sub_824D5AA8
#define DPX_824D5B78 sub_824D5B78
#define DPX_824DA330 sub_824DA330
#define DPX_824D9B18 sub_824D9B18  // [new_fix_24092026] XDK gamma ramp dispatcher
#define DPX_824E5DB8 sub_824E5DB8
#define DPX_824E7E70 sub_824E7E70
#define DPX_824E7F98 sub_824E7F98
#define DPX_825CD7A0 sub_825CD7A0
#define DPX_825CD7E8 sub_825CD7E8
#define DPX_825CDBD8 sub_825CDBD8
#endif

REXCVAR_DEFINE_BOOL(dp_native_trace, false, "DP1",
                    "Native renderer diagnostics: log enter/exit of the observed XDK entries with the calling thread "
                    "(first 4000 lines)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
// [NEW FABLE VERSION] 2026-09-22: the frame statistics line (draws, uploads, PSOs,
// frame/CPU/GPU time percentiles) every N frames; 600 = every 10 s at 60 fps.
REXCVAR_DEFINE_INT32(dp_native_stats_frames, 600, "DP1",
                     "Native renderer: print the frame statistics line every N frames (0 = never)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace {
std::atomic<uint32_t> g_trace_budget{40};
std::atomic<uint32_t> g_xdk_trace_budget{30000};
inline void XdkTrace(const char* what, uint32_t r3, uint32_t r4, uint32_t r5) {
  if (!REXCVAR_GET(dp_native_trace)) return;
  // Only the unnamed worker threads (movie player, loaders): the main/render
  // threads would exhaust the budget within a frame.
  auto* thread = rex::system::XThread::GetCurrentThread();
  if (!thread || thread->name().rfind("XThread", 0) != 0) return;
  if (g_xdk_trace_budget.load(std::memory_order_relaxed) == 0) return;
  g_xdk_trace_budget.fetch_sub(1, std::memory_order_relaxed);
  REXLOG_INFO("Native xdk: {} r3={:08X} r4={:08X} r5={:08X}", what, r3, r4, r5);
}
#define DP_XDK_TRACE_HOOK(name)                                               REX_HOOK_RAW(name) {                                                          XdkTrace(#name " enter", ctx.r3.u32, ctx.r4.u32, ctx.r5.u32);               DP_NATIVE_IMP(name)(ctx, base);                                                   XdkTrace(#name " exit", ctx.r3.u32, ctx.r4.u32, ctx.r5.u32);              }
inline bool TraceBudget() {
  return g_trace_budget.load(std::memory_order_relaxed) > 0 && g_trace_budget.fetch_sub(1, std::memory_order_relaxed) > 0;
}
inline uint32_t GuestU32(uint32_t addr) {
  return rex::memory::load_and_swap<uint32_t>(REX_KERNEL_MEMORY()->TranslateVirtual(addr));
}
}  // namespace

#define DP_NATIVE_PASSTHROUGH(name)      \
  if (!dp::native::Enabled()) {          \
    DP_NATIVE_IMP(name)(ctx, base);            \
    return;                              \
  }

// ---------------------------------------------------------------------------
// Replaced entries
// ---------------------------------------------------------------------------
REX_EXTERN(DP_NATIVE_IMP(DPX_824DA330));  // D3DDevice_Swap(dev, frontBufferTexture, ...)
REX_EXTERN(DP_NATIVE_IMP(DPX_825CD7E8));  // D3DDevice_DrawVertices(dev, prim, startVertex, count)
REX_EXTERN(DP_NATIVE_IMP(DPX_825CDBD8));  // D3DDevice_DrawIndexedVertices(dev, prim, baseVertex, startIndex, count)
REX_EXTERN(DP_NATIVE_IMP(DPX_825CD7A0));  // D3DDevice_DrawVerticesUP(dev, prim, count, data, stride)
REX_EXTERN(DP_NATIVE_IMP(DPX_824E5DB8));  // D3DDevice_Resolve(dev, flags, rect, destTex, destPoint, ...)
REX_EXTERN(DP_NATIVE_IMP(DPX_824E7E70));  // D3DDevice_ClearF(dev, flags, rects, float4* color, ?, stencil; z = f1)
REX_EXTERN(DP_NATIVE_IMP(DPX_824D1AF8));  // D3DDevice_SetShaderGPRAllocation
REX_EXTERN(DP_NATIVE_IMP(DPX_824D2E78));  // XDK BlockOnFence(dev, fence)
// [new_fix_24092026] XDK gamma ramp dispatcher(dev, frontBufferFormat, ramp): writes
// the display gamma ramp into the ring (default ramp for the display type when
// ramp == 0; PWL for a 2_10_10_10 front buffer, the 256-entry table otherwise).
REX_EXTERN(DP_NATIVE_IMP(DPX_824D9B18));
REX_EXTERN(DP_NATIVE_IMP(DPX_824D6010));
REX_EXTERN(DP_NATIVE_IMP(DPX_824D6108));

namespace {
// [new_fix_24092026] diagnostic: the first calls of the XDK gamma ramp path,
// in both paths (the emulated one is the reference). r4 of the writers is a
// D3DGAMMARAMP (WORD red[256], green[256], blue[256], big-endian).
std::atomic<int32_t> g_gamma_trace_budget{12};
template <typename Ctx>
void GammaTrace(const char* what, const Ctx& ctx, bool ramp_in_r4) {
  if (g_gamma_trace_budget.fetch_sub(1, std::memory_order_relaxed) <= 0) return;
  std::string sample;
  if (ramp_in_r4 && ctx.r4.u32) {
    const auto* ramp = reinterpret_cast<const uint8_t*>(REX_KERNEL_MEMORY()->TranslateVirtual(ctx.r4.u32));
    auto word = [&](uint32_t off) { return uint32_t(ramp[off]) << 8 | ramp[off + 1]; };
    for (uint32_t i : {1u, 8u, 32u, 64u, 128u, 192u, 255u}) {
      sample += fmt::format(" [{}]={},{},{}", i, word(i * 2), word(512 + i * 2), word(1024 + i * 2));
    }
  }
  REXLOG_INFO("Native gamma trace: {} (native {}) lr={:08X} r3={:08X} r4={:08X} r5={:08X}{}", what,
              dp::native::Enabled(), uint32_t(ctx.lr), ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, sample);
}
}  // namespace

REX_HOOK_RAW(DPX_824D9B18) {  // [new_fix_24092026] diagnostic, calls the XDK body
  GammaTrace("dispatcher(dev, front buffer format, ramp)", ctx, false);
  DP_NATIVE_IMP(DPX_824D9B18)(ctx, base);
}
REX_HOOK_RAW(DPX_824D6010) {  // [new_fix_24092026] diagnostic, calls the XDK body
  GammaTrace("table writer(dev, ramp)", ctx, true);
  DP_NATIVE_IMP(DPX_824D6010)(ctx, base);
}
REX_HOOK_RAW(DPX_824D6108) {  // [new_fix_24092026] diagnostic, calls the XDK body
  GammaTrace("pwl writer(dev, ramp)", ctx, false);
  DP_NATIVE_IMP(DPX_824D6108)(ctx, base);
}

REX_HOOK_RAW(DPX_824DA330) {
  XdkTrace(DP_NATIVE_STR(DPX_824DA330), ctx.r3.u32, ctx.r4.u32, ctx.r5.u32);
  DP_NATIVE_PASSTHROUGH(DPX_824DA330);
  // [new_fix_24092026] The XDK's Swap writes the display gamma ramp when the
  // device marks it pending (byte dev+10943 bit 0x40, which Swap clears). The
  // native Swap does not run the XDK body, so the ramp was never written and the
  // native picture had no display gamma. Do that part here: the XDK dispatcher
  // writes the ramp into the ring, where NativeGraphicsSystem captures the DC_LUT
  // writes (native_gamma.h) and the present blit applies them.
  //
  // Measured on the emulated path (24.09): the XDK makes ONE call, at swap #2,
  // dispatcher(dev, 0x28280106 = 8_8_8_8 front buffer, dev+15212 = the device's
  // stored D3DGAMMARAMP, filled at device creation from the display type). Swap #1
  // takes the branch that sets bit 0x40 and suppresses the write; swap #2 writes
  // and clears it. The native Swap mirrors the net effect: the stored ramp once
  // per device, and again whenever bit 0x40 is set. SetGammaRamp writes its ramp
  // into the ring itself, so custom ramps reach NativeGraphicsSystem without this.
  if (dp::native::GammaRampEnabled() && ctx.r3.u32) {
    static uint32_t s_gamma_device = 0;  // Swap runs on the render thread only
    auto* pending = reinterpret_cast<uint8_t*>(REX_KERNEL_MEMORY()->TranslateVirtual(ctx.r3.u32 + 10943));
    const bool requested = (*pending & 0x40) != 0;
    if (requested || s_gamma_device != ctx.r3.u32) {
      s_gamma_device = ctx.r3.u32;
      *pending = uint8_t(*pending & ~0x40);
      const uint64_t r3 = ctx.r3.u64, r4 = ctx.r4.u64, r5 = ctx.r5.u64, lr = ctx.lr;
      ctx.r4.u64 = 0x28280106;          // the XDK's 8_8_8_8 front-buffer format: the 256-entry table
      ctx.r5.u64 = ctx.r3.u32 + 15212;  // the device's stored ramp, as the XDK Swap passes it
      DP_NATIVE_IMP(DPX_824D9B18)(ctx, base);
      ctx.r3.u64 = r3;
      ctx.r4.u64 = r4;
      ctx.r5.u64 = r5;
      ctx.lr = lr;
      REXLOG_INFO("Native renderer: display gamma ramp written from the device ramp ({})",
                  requested ? "XDK re-request" : "first swap of this device");
    }
  }
  auto* system = dp::native::NativeGraphicsSystem::instance();
  const dp::native::RendererStats stats = dp::native::OnSwap(ctx.r4.u32);
  if (system) {
    const uint32_t frame = system->frame_count();
    const int32_t every = REXCVAR_GET(dp_native_stats_frames);  // [NEW FABLE VERSION]
    if (frame == 0 || (every > 0 && (frame + 1) % uint32_t(every) == 0)) {
      REXLOG_INFO("Native renderer: frame #{}: {} draws ({} skipped), {} resolves ({} partial), {} clears, {} edram transfers, {} resolve aliases, uploads vb {} ib {} tex {} "
                  "({:.2f} MB, const banks {} up / {} reused) (watch hits {} changed {} vtf rehash {}), PSOs created {} (alive {}); {}; {}",
                  frame + 1, stats.draws, stats.draws_skipped, stats.resolves, stats.resolve_partial, stats.clears, stats.edram_transfers, stats.resolve_aliases,
                  stats.uploads_vb, stats.uploads_ib, stats.uploads_tex, double(stats.upload_bytes) / (1024.0 * 1024.0),
                  stats.const_uploads, stats.const_reuses,
                  stats.watch_hits, stats.tex_changes, stats.tex_rehash, stats.pso_created, stats.psos_total,
                  dp::native::PerfSummaryAndReset(), system->RingStats());
    }
    system->OnSwapDone(ctx.r4.u32);
  }
  ctx.r3.u64 = 0;
}

REX_HOOK_RAW(DPX_825CD7E8) {
  XdkTrace(DP_NATIVE_STR(DPX_825CD7E8), ctx.r3.u32, ctx.r4.u32, ctx.r5.u32);
  DP_NATIVE_PASSTHROUGH(DPX_825CD7E8);
  dp::native::OnDrawVertices(ctx.r4.u32, ctx.r5.u32, ctx.r6.u32);
}

REX_HOOK_RAW(DPX_825CDBD8) {
  XdkTrace(DP_NATIVE_STR(DPX_825CDBD8), ctx.r3.u32, ctx.r4.u32, ctx.r5.u32);
  DP_NATIVE_PASSTHROUGH(DPX_825CDBD8);
  dp::native::OnDrawIndexedVertices(ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32);
}

REX_HOOK_RAW(DPX_825CD7A0) {
  XdkTrace(DP_NATIVE_STR(DPX_825CD7A0), ctx.r3.u32, ctx.r4.u32, ctx.r5.u32);
  DP_NATIVE_PASSTHROUGH(DPX_825CD7A0);
  dp::native::OnDrawVerticesUP(ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32);
}

REX_HOOK_RAW(DPX_824E5DB8) {
  XdkTrace(DP_NATIVE_STR(DPX_824E5DB8), ctx.r3.u32, ctx.r4.u32, ctx.r5.u32);
  DP_NATIVE_PASSTHROUGH(DPX_824E5DB8);
  if (TraceBudget()) {
    REXLOG_INFO("Native trace: Resolve flags {:08X} rect {:08X} dest {:08X} r7 {:08X} r8 {:08X} r9 {:08X} r10 {:08X}",
                ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, ctx.r8.u32, ctx.r9.u32, ctx.r10.u32);
  }
  // (dev, Flags, pSourceRect, pDestTexture, pDestPoint, DestLevel, DestSliceOrFace, pClearColor, ClearZ = f1, ...)
  dp::native::OnResolve(ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, ctx.r8.u32, ctx.r9.u32, ctx.r10.u32,
                        float(ctx.f1.f64));
  ctx.r3.u64 = 0;
}

REX_HOOK_RAW(DPX_824E7E70) {
  XdkTrace(DP_NATIVE_STR(DPX_824E7E70), ctx.r3.u32, ctx.r4.u32, ctx.r5.u32);
  DP_NATIVE_PASSTHROUGH(DPX_824E7E70);
  if (TraceBudget()) {
    REXLOG_INFO("Native trace: ClearF count {} rects {:08X} flags {:08X} color {:08X} z {} stencil {}", ctx.r4.u32,
                ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, ctx.f1.f64, ctx.r8.u32);
  }
  // ClearF(dev, flags, rects, float4* color, ?, stencil; z in f1) - from the
  // D3DCOLOR wrapper DPX_824E7F98: mr r4,r28(flags); li r5,0; addi r6,r1,80.
  dp::native::OnClear(ctx.r4.u32, ctx.r6.u32, float(ctx.f1.f64), ctx.r8.u32);
  ctx.r3.u64 = 0;
}

REX_HOOK_RAW(DPX_824D1AF8) {
  XdkTrace(DP_NATIVE_STR(DPX_824D1AF8), ctx.r3.u32, ctx.r4.u32, ctx.r5.u32);
  DP_NATIVE_PASSTHROUGH(DPX_824D1AF8);
  ctx.r3.u64 = 0;
}

// XDK BlockOnFence: compares `fence` with the completed-fence word the GPU
// writes at [[dev+10896]+0]. Nothing writes it in native mode and every GPU
// use of a resource is copied out on the guest thread, so return at once.
REX_HOOK_RAW(DPX_824D2E78) {
  XdkTrace(DP_NATIVE_STR(DPX_824D2E78), ctx.r3.u32, ctx.r4.u32, ctx.r5.u32);
  DP_NATIVE_PASSTHROUGH(DPX_824D2E78);
  ctx.r3.u64 = 0;
}

// ---------------------------------------------------------------------------
// Observed entries (original runs, renderer is told afterwards)
// ---------------------------------------------------------------------------
REX_EXTERN(DP_NATIVE_IMP(DPX_824D2658));  // Direct3D_CreateDevice
REX_EXTERN(DP_NATIVE_IMP(DPX_824D04F8));  // CreateTexture(w, h, depth, levels, usage, fmt, pool, type) -> object
REX_EXTERN(DP_NATIVE_IMP(DPX_824D0618));  // CreateSurface(w, h, fmt, multisample, params) -> object
REX_EXTERN(DP_NATIVE_IMP(DPX_824D4B60));  // CreateVertexBuffer(size, ...) -> object
REX_EXTERN(DP_NATIVE_IMP(DPX_824D4C38));  // CreateIndexBuffer(size, ...) -> object
REX_EXTERN(DP_NATIVE_IMP(DPX_824D1A78));  // CreateVertexDeclaration(elements) -> object
REX_EXTERN(DP_NATIVE_IMP(DPX_824D1F08));  // CreateVertexShader(container, ...) -> object
REX_EXTERN(DP_NATIVE_IMP(DPX_824D1D20));  // CreatePixelShader(container, ...) -> object
REX_EXTERN(DP_NATIVE_IMP(DPX_824D5AA8));  // D3DResource_Release(obj)
REX_EXTERN(DP_NATIVE_IMP(DPX_824D5B78));  // Lock (VB/IB/texture level) -> pointer
REX_EXTERN(DP_NATIVE_IMP(DPX_824D4CE8));  // Unlock (buffers)
REX_EXTERN(DP_NATIVE_IMP(DPX_824CE7E0));  // UnlockRect (textures)
REX_EXTERN(DP_NATIVE_IMP(DPX_824CE7C8));  // UnlockTail
REX_EXTERN(DP_NATIVE_IMP(DPX_824CE688));  // SetRenderTarget(dev, idx, surface)
REX_EXTERN(DP_NATIVE_IMP(DPX_824CE3B0));  // SetDepthStencilSurface(dev, surface)
REX_EXTERN(DP_NATIVE_IMP(DPX_824CDF30));  // SetViewport(dev, D3DVIEWPORT9*)
REX_EXTERN(DP_NATIVE_IMP(DPX_824D0878));  // SetTexture(dev, sampler, texture)
REX_EXTERN(DP_NATIVE_IMP(DPX_824CD6E0));  // SetStreamSource(dev, stream, vb, offset, stride)
REX_EXTERN(DP_NATIVE_IMP(DPX_824CD888));  // SetIndices(dev, ib)
REX_EXTERN(DP_NATIVE_IMP(DPX_824D1970));  // SetVertexDeclaration(dev, decl)
REX_EXTERN(DP_NATIVE_IMP(DPX_824D17A0));  // SetVertexShader(dev, vs)
REX_EXTERN(DP_NATIVE_IMP(DPX_824D15E0));  // SetPixelShader(dev, ps)

REX_HOOK_RAW(DPX_824D2658) {
  XdkTrace(DP_NATIVE_STR(DPX_824D2658), ctx.r3.u32, ctx.r4.u32, ctx.r5.u32);
  DP_NATIVE_IMP(DPX_824D2658)(ctx, base);
  if (dp::native::Enabled()) dp::native::OnCreateDevice();
}

REX_HOOK_RAW(DPX_824D04F8) {
  XdkTrace("DPX_824D04F8 enter", ctx.r3.u32, ctx.r4.u32, ctx.r5.u32);
  DP_NATIVE_IMP(DPX_824D04F8)(ctx, base);
  XdkTrace("DPX_824D04F8 exit", ctx.r3.u32, ctx.r4.u32, ctx.r5.u32);
  if (dp::native::Enabled()) dp::native::OnCreateTexture(ctx, ctx.r3.u32);
}

REX_HOOK_RAW(DPX_824D0618) {
  XdkTrace(DP_NATIVE_STR(DPX_824D0618), ctx.r3.u32, ctx.r4.u32, ctx.r5.u32);
  const uint32_t w = ctx.r3.u32, h = ctx.r4.u32, fmt = ctx.r5.u32, ms = ctx.r6.u32, params = ctx.r7.u32;
  DP_NATIVE_IMP(DPX_824D0618)(ctx, base);
  if (dp::native::Enabled()) dp::native::OnCreateSurface(w, h, fmt, ms, params, ctx.r3.u32);
}

REX_HOOK_RAW(DPX_824D4B60) {
  XdkTrace(DP_NATIVE_STR(DPX_824D4B60), ctx.r3.u32, ctx.r4.u32, ctx.r5.u32);
  DP_NATIVE_IMP(DPX_824D4B60)(ctx, base);
  if (dp::native::Enabled()) dp::native::OnCreateVertexBuffer(ctx.r3.u32);
}

REX_HOOK_RAW(DPX_824D4C38) {
  XdkTrace(DP_NATIVE_STR(DPX_824D4C38), ctx.r3.u32, ctx.r4.u32, ctx.r5.u32);
  DP_NATIVE_IMP(DPX_824D4C38)(ctx, base);
  if (dp::native::Enabled()) dp::native::OnCreateIndexBuffer(ctx.r3.u32);
}

REX_HOOK_RAW(DPX_824D1A78) {
  XdkTrace(DP_NATIVE_STR(DPX_824D1A78), ctx.r3.u32, ctx.r4.u32, ctx.r5.u32);
  const uint32_t elements = ctx.r3.u32;
  DP_NATIVE_IMP(DPX_824D1A78)(ctx, base);
  if (dp::native::Enabled()) dp::native::OnCreateVertexDeclaration(elements, ctx.r3.u32);
}

REX_HOOK_RAW(DPX_824D1F08) {
  XdkTrace(DP_NATIVE_STR(DPX_824D1F08), ctx.r3.u32, ctx.r4.u32, ctx.r5.u32);
  const uint32_t container = ctx.r3.u32;
  DP_NATIVE_IMP(DPX_824D1F08)(ctx, base);
  if (dp::native::Enabled()) {
    if (TraceBudget()) REXLOG_INFO("Native trace: CreateVertexShader container {:08X} -> {:08X}", container, ctx.r3.u32);
    dp::native::OnCreateShader(container, ctx.r3.u32, false);
  }
}

REX_HOOK_RAW(DPX_824D1D20) {
  XdkTrace(DP_NATIVE_STR(DPX_824D1D20), ctx.r3.u32, ctx.r4.u32, ctx.r5.u32);
  const uint32_t container = ctx.r3.u32;
  DP_NATIVE_IMP(DPX_824D1D20)(ctx, base);
  if (dp::native::Enabled()) {
    if (TraceBudget()) REXLOG_INFO("Native trace: CreatePixelShader container {:08X} -> {:08X}", container, ctx.r3.u32);
    dp::native::OnCreateShader(container, ctx.r3.u32, true);
  }
}

REX_HOOK_RAW(DPX_824D5AA8) {
  XdkTrace(DP_NATIVE_STR(DPX_824D5AA8), ctx.r3.u32, ctx.r4.u32, ctx.r5.u32);
  const uint32_t object = ctx.r3.u32;
  uint32_t refcount = 0;
  if (dp::native::Enabled() && object) refcount = GuestU32(object + 4);
  DP_NATIVE_IMP(DPX_824D5AA8)(ctx, base);
  if (dp::native::Enabled()) dp::native::OnRelease(object, refcount);
}

REX_HOOK_RAW(DPX_824D5B78) {
  XdkTrace(DP_NATIVE_STR(DPX_824D5B78), ctx.r3.u32, ctx.r4.u32, ctx.r5.u32);
  const uint32_t object = ctx.r3.u32;
  XdkTrace("DPX_824D5B78 enter", object, ctx.r4.u32, ctx.r5.u32);
  DP_NATIVE_IMP(DPX_824D5B78)(ctx, base);
  XdkTrace("DPX_824D5B78 exit", ctx.r3.u32, ctx.r4.u32, ctx.r5.u32);
  if (dp::native::Enabled()) dp::native::OnUnlock(object);  // the CPU is about to write it
}

REX_HOOK_RAW(DPX_824D4CE8) {
  XdkTrace(DP_NATIVE_STR(DPX_824D4CE8), ctx.r3.u32, ctx.r4.u32, ctx.r5.u32);
  const uint32_t object = ctx.r3.u32;
  XdkTrace("DPX_824D4CE8 enter", object, ctx.r4.u32, ctx.r5.u32);
  DP_NATIVE_IMP(DPX_824D4CE8)(ctx, base);
  XdkTrace("DPX_824D4CE8 exit", ctx.r3.u32, ctx.r4.u32, ctx.r5.u32);
  if (dp::native::Enabled()) dp::native::OnUnlock(object);
}

REX_HOOK_RAW(DPX_824CE7E0) {
  XdkTrace(DP_NATIVE_STR(DPX_824CE7E0), ctx.r3.u32, ctx.r4.u32, ctx.r5.u32);
  const uint32_t object = ctx.r3.u32;
  XdkTrace("DPX_824CE7E0 enter", object, ctx.r4.u32, ctx.r5.u32);
  DP_NATIVE_IMP(DPX_824CE7E0)(ctx, base);
  XdkTrace("DPX_824CE7E0 exit", ctx.r3.u32, ctx.r4.u32, ctx.r5.u32);
  if (dp::native::Enabled()) dp::native::OnUnlock(object);
}

REX_HOOK_RAW(DPX_824CE7C8) {
  XdkTrace(DP_NATIVE_STR(DPX_824CE7C8), ctx.r3.u32, ctx.r4.u32, ctx.r5.u32);
  const uint32_t object = ctx.r3.u32;
  XdkTrace("DPX_824CE7C8 enter", object, ctx.r4.u32, ctx.r5.u32);
  DP_NATIVE_IMP(DPX_824CE7C8)(ctx, base);
  XdkTrace("DPX_824CE7C8 exit", ctx.r3.u32, ctx.r4.u32, ctx.r5.u32);
  if (dp::native::Enabled()) dp::native::OnUnlock(object);
}

REX_HOOK_RAW(DPX_824CE688) {
  XdkTrace(DP_NATIVE_STR(DPX_824CE688), ctx.r3.u32, ctx.r4.u32, ctx.r5.u32);
  const uint32_t index = ctx.r4.u32, surface = ctx.r5.u32;
  DP_NATIVE_IMP(DPX_824CE688)(ctx, base);
  if (dp::native::Enabled()) dp::native::OnSetRenderTarget(index, surface);
}

REX_HOOK_RAW(DPX_824CE3B0) {
  XdkTrace(DP_NATIVE_STR(DPX_824CE3B0), ctx.r3.u32, ctx.r4.u32, ctx.r5.u32);
  const uint32_t surface = ctx.r4.u32;
  DP_NATIVE_IMP(DPX_824CE3B0)(ctx, base);
  if (dp::native::Enabled()) dp::native::OnSetDepthStencilSurface(surface);
}

REX_HOOK_RAW(DPX_824CDF30) {
  XdkTrace(DP_NATIVE_STR(DPX_824CDF30), ctx.r3.u32, ctx.r4.u32, ctx.r5.u32);
  const uint32_t vp = ctx.r4.u32;
  DP_NATIVE_IMP(DPX_824CDF30)(ctx, base);
  if (dp::native::Enabled()) dp::native::OnSetViewport(vp);
}

REX_HOOK_RAW(DPX_824D0878) {
  XdkTrace(DP_NATIVE_STR(DPX_824D0878), ctx.r3.u32, ctx.r4.u32, ctx.r5.u32);
  const uint32_t sampler = ctx.r4.u32, texture = ctx.r5.u32;
  DP_NATIVE_IMP(DPX_824D0878)(ctx, base);
  if (dp::native::Enabled()) dp::native::OnSetTexture(sampler, texture);
}

REX_HOOK_RAW(DPX_824CD6E0) {
  XdkTrace(DP_NATIVE_STR(DPX_824CD6E0), ctx.r3.u32, ctx.r4.u32, ctx.r5.u32);
  const uint32_t stream = ctx.r4.u32, vb = ctx.r5.u32, offset = ctx.r6.u32, stride = ctx.r7.u32;
  DP_NATIVE_IMP(DPX_824CD6E0)(ctx, base);
  if (dp::native::Enabled()) dp::native::OnSetStreamSource(stream, vb, offset, stride);
}

REX_HOOK_RAW(DPX_824CD888) {
  XdkTrace(DP_NATIVE_STR(DPX_824CD888), ctx.r3.u32, ctx.r4.u32, ctx.r5.u32);
  const uint32_t ib = ctx.r4.u32;
  DP_NATIVE_IMP(DPX_824CD888)(ctx, base);
  if (dp::native::Enabled()) dp::native::OnSetIndices(ib);
}

REX_HOOK_RAW(DPX_824D1970) {
  XdkTrace(DP_NATIVE_STR(DPX_824D1970), ctx.r3.u32, ctx.r4.u32, ctx.r5.u32);
  const uint32_t decl = ctx.r4.u32;
  DP_NATIVE_IMP(DPX_824D1970)(ctx, base);
  if (dp::native::Enabled()) dp::native::OnSetVertexDeclaration(decl);
}

REX_HOOK_RAW(DPX_824D17A0) {
  XdkTrace(DP_NATIVE_STR(DPX_824D17A0), ctx.r3.u32, ctx.r4.u32, ctx.r5.u32);
  const uint32_t vs = ctx.r4.u32;
  DP_NATIVE_IMP(DPX_824D17A0)(ctx, base);
  if (dp::native::Enabled()) dp::native::OnSetVertexShader(vs);
}

REX_HOOK_RAW(DPX_824D15E0) {
  XdkTrace(DP_NATIVE_STR(DPX_824D15E0), ctx.r3.u32, ctx.r4.u32, ctx.r5.u32);
  const uint32_t ps = ctx.r4.u32;
  DP_NATIVE_IMP(DPX_824D15E0)(ctx, base);
  if (dp::native::Enabled()) dp::native::OnSetPixelShader(ps);
}

// Pass-through tracing of other XDK entries the loading threads go through.
REX_EXTERN(DP_NATIVE_IMP(DPX_824D07E8));  // LockRect family
REX_EXTERN(DP_NATIVE_IMP(DPX_824D0358));  // texture level pointer / lock
REX_EXTERN(DP_NATIVE_IMP(DPX_824CFA28));  // texture lock helper (blocking)

REX_EXTERN(DP_NATIVE_IMP(DPX_824D5760));  // resource fence check
REX_EXTERN(DP_NATIVE_IMP(DPX_824D01B8));  // LockTail
DP_XDK_TRACE_HOOK(DPX_824D07E8)
DP_XDK_TRACE_HOOK(DPX_824D0358)
DP_XDK_TRACE_HOOK(DPX_824CFA28)

DP_XDK_TRACE_HOOK(DPX_824D5760)
DP_XDK_TRACE_HOOK(DPX_824D01B8)
