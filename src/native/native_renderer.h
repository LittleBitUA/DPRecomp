// Deadly Premonition Recompilation - native renderer core (milestone 1).
//
// Model (Unleashed / Marathon Recompiled): the XDK D3D entries the game calls
// are hooked; guest objects stay authentic (the XDK still builds its headers,
// D3DX and GetLevelDesc read them), and every object is registered here from
// its Create hook so a host resource can be attached to it. Draws are executed
// on the guest thread into one D3D12 command list per frame, submitted at
// Swap. No PM4, no register file, no EDRAM emulation: a guest surface is a
// host render target, Resolve copies it into the destination texture's host
// resource, and the front buffer texture is blitted into the SDK presenter.
#pragma once

#include <cstdint>
#include <string>

#include "deadlyprem_pch.h"

struct ID3D12Resource;

namespace dp::native {

struct RendererStats {
  uint32_t draws = 0, draws_skipped = 0, resolves = 0, clears = 0, uploads_vb = 0, uploads_ib = 0,
           uploads_tex = 0, pso_created = 0, tex_rehash = 0, tex_changes = 0, watch_hits = 0;
  // [NEW FABLE VERSION] 2026-09-22: performance counters for the frame line.
  uint32_t resolve_partial = 0;  // resolves with a source rect / dest point / dest level the blit ignores
  uint32_t psos_total = 0;       // pipelines alive (cumulative)
  uint64_t upload_bytes = 0;     // bytes written into the upload ring this frame
  uint64_t draw_cpu_us = 0;      // CPU time inside the native draw/clear/resolve/swap path this frame
  uint32_t const_uploads = 0, const_reuses = 0;  // constant banks uploaded vs reused this frame
};

// [NEW FABLE VERSION] p50/p90/p99 of the frame time, the native CPU time per
// frame, the fence wait and the GPU time per frame since the previous call.
std::string PerfSummaryAndReset();

// All entry points take the raw PPC context of the hooked call (arguments in
// r3.. as the XDK receives them) and are called only when the native renderer
// is enabled. They never call the original XDK body.
void OnCreateDevice();  // after the original CreateDevice returned (device global valid)
void OnCreateTexture(PPCContext& ctx, uint32_t texture);              // object returned by the XDK
void OnCreateSurface(uint32_t width, uint32_t height, uint32_t format, uint32_t multisample,
                     uint32_t params, uint32_t surface);
void OnCreateVertexBuffer(uint32_t vb);
void OnCreateIndexBuffer(uint32_t ib);
void OnCreateVertexDeclaration(uint32_t elements, uint32_t decl);
void OnCreateShader(uint32_t container, uint32_t shader, bool pixel);
void OnRelease(uint32_t object, uint32_t refcount_before);
void OnUnlock(uint32_t object);

void OnSetRenderTarget(uint32_t index, uint32_t surface);
void OnSetDepthStencilSurface(uint32_t surface);
void OnSetViewport(uint32_t viewport_ptr);
void OnSetTexture(uint32_t sampler, uint32_t texture);
void OnSetStreamSource(uint32_t stream, uint32_t vb, uint32_t offset, uint32_t stride);
void OnSetIndices(uint32_t ib);
void OnSetVertexDeclaration(uint32_t decl);
void OnSetVertexShader(uint32_t vs);
void OnSetPixelShader(uint32_t ps);

void OnDrawVertices(uint32_t prim, uint32_t start_vertex, uint32_t count);
void OnDrawIndexedVertices(uint32_t prim, uint32_t base_vertex, uint32_t start_index, uint32_t count);
void OnDrawVerticesUP(uint32_t prim, uint32_t count, uint32_t data, uint32_t stride);
void OnClear(uint32_t flags, uint32_t color_ptr, float z, uint32_t stencil);
void OnResolve(uint32_t flags, uint32_t rect_ptr, uint32_t dest_texture, uint32_t dest_point_ptr, uint32_t dest_level,
               uint32_t dest_slice, uint32_t clear_color_ptr, float clear_z);
// Presents the front buffer texture through the SDK presenter and starts the
// next frame. Returns the stats of the finished frame.
RendererStats OnSwap(uint32_t front_buffer_texture);

// Called by NativeGraphicsSystem inside the presenter refresh callback: records
// the blit of `src_srv` into the presenter's guest-output texture on the
// renderer's command list, then submits the frame.
void RendererBlitToPresenter(uint32_t src_srv, ID3D12Resource* dest, uint32_t width, uint32_t height);
void RendererSubmitCurrentFrame();

}  // namespace dp::native
