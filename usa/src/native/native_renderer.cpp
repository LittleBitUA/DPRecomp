// Deadly Premonition Recompilation - native renderer core (milestone 1).
// See native_renderer.h for the model. Everything here runs on the guest
// thread that calls the XDK (Main XThread), one frame = one command list.

#include "native_renderer.h"
#include "native_cache_path.h"  // [new_fix_24092026] issue #33
#include "native_retire.h"      // [new_fix_24092026] 2.0.3: issues #33/#34
#include "native_buffer_header.h"  // [new_fix_24092026] 2.0.4: issue #35

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <rex/graphics/pipeline/texture/conversion.h>
#include <rex/graphics/pipeline/texture/info.h>
#include <rex/graphics/pipeline/texture/util.h>
#include <rex/graphics/xenos.h>
#include <rex/cvar.h>
#include <rex/hash.h>
#include <rex/kernel/xboxkrnl/video.h>
#include <rex/logging.h>
#include <rex/logging/api.h>  // [new_fix_24092026] FlushLogging before the exit after a GPU loss
#include <rex/filesystem.h>   // [new_fix_24092026] GetExecutableFolder (shader cache lookup)
#include <thread>              // [new_fix_24092026] the GPU-lost message box thread
#include <rex/memory/utils.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>
#include <rex/ui/d3d12/d3d12_presenter.h>
#include <rex/ui/d3d12/d3d12_provider.h>
#include <rex/ui/d3d12/d3d12_util.h>

#include <d3dcompiler.h>
#include <windows.h>
#include <intrin.h>    // [new_fix_24092026] __rdtsc
#include <tlhelp32.h>  // [new_fix_24092026] thread snapshot (dp_native_perf_threads)
#include <wrl/client.h>

#include "native_graphics_system.h"
#include "native_scale.h"  // [NEW FABLE VERSION] internal resolution rules (tested in tests/native_scale_test.cpp)
#include "native_texrep.h"  // [NEW FABLE VERSION] textures\<hash>.png replacement (tested in tests/native_texrep_test.cpp)

#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "d3d12.lib")

REXCVAR_DEFINE_BOOL(dp_native_no_alphatest, false, "DP1", "Diagnostic: ignore the guest alpha test");
// [NEW FABLE VERSION] 2026-09-23: the scene targets are 7e3 (k_2_10_10_10_FLOAT),
// whose alpha is fixed-point on the console and never leaves 0..1. The host
// target is RGBA16F, where an additive glow mesh pushed the scene alpha to 1.4,
// and the tone map uses that alpha as its SrcAlpha blend factor (a hard-edged
// bright "paper" shape around lamps). The resolve clamps it to 0..1, as the
// emulator's does (its alpha comes from the 16-bit ROV shadow, 0..1).
REXCVAR_DEFINE_BOOL(dp_native_7e3_alpha, true, "DP1",
                    "Native renderer: clamp the alpha of 7e3 scene targets to 0..1 when resolving them "
                    "(the console's alpha is fixed-point)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
// [NEW FABLE VERSION] 2026-09-23: surfaces are views of EDRAM tiles. DP1 draws
// the HDR scene into a 7e3 surface at tile 720, then binds an 8888 surface on
// the same tiles and tone-maps into it with SrcAlpha / InvSrcAlpha, so the
// scene itself is the blend destination wherever its alpha is below 1 (sky,
// cloud edges, glass: a quarter of an outdoor frame). The host keeps one
// resource per surface, so that destination was whatever the 8888 surface held
// from its own last use: black after a clear, or the previous frame (trails
// behind moving things, a brown sky with a hard edge over the mountains,
// doubled signs, blinking). Binding a surface now first copies in the newest
// content of its tiles when another surface wrote them since: an exact re-bind
// (same base, size and scale) is a copy, 7e3 -> 8888 is saturate(), as in the
// emulator (the SDK's 7e3 -> 8888 ownership transfer). Other pairs keep their
// own content and are logged once.
// [NEW FABLE VERSION] 2026-09-24: textures are views of guest memory, and the
// emulator's texture cache is keyed by memory; ours is keyed by fetch constant.
// A texture whose memory was last written by a resolve through another texture
// object now samples that resolve's result (same base, size and format). Found
// while chasing the white sheriff's-office floor (fetch #9 of the floor shader,
// 512x288 at 0x1C4C1000, is never written here), but that floor was the
// recompiler's co-issue bug (XenosRecomp `pv`); this path did not fire once in
// a whole session in the office (resolve aliases 0). Kept: it is the emulator's
// semantics and costs a map lookup per CPU-texture bind.
// [NEW FABLE VERSION] 2.0.1: off by default. Adversarial review: a view created
// after a level load over memory an old resolve wrote (same size and format,
// other fetch key) would sample that stale resolve forever, because an aliased
// view is never uploaded and so never page-watched. Zero hits in the 2.0 test
// session; turn on only to experiment.
REXCVAR_DEFINE_BOOL(dp_native_resolve_alias, false, "DP1",
                    "Native renderer: a texture view of memory a resolve wrote last samples that resolve's "
                    "result (the emulator's texture cache is keyed by memory)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
// [new_fix_24092026] The console passes the front buffer through the display
// gamma ramp (DC_LUT) the XDK programs for the display type the kernel reports
// through VdGetCurrentDisplayGamma (the SDK reports TV / BT.709). The emulated
// path applies it at swap; measured on DP1 it darkens shadows and low mids
// (input 32/255 -> 66/1023 instead of 128, 128/255 -> 462 instead of 513).
// The native renderer had no ramp at all (a lighter, flatter picture). Now the
// Swap hook lets the XDK write the pending ramp (native_hooks.cpp),
// NativeGraphicsSystem captures it (native_gamma.h) and the final blit applies
// the table. Off = the previous native output.
// [new_fix_24092026] CPU per host thread over each stats window (see
// ThreadCpuReport). Off: no thread snapshot at all.
REXCVAR_DEFINE_BOOL(dp_native_perf_threads, false, "DP1",
                    "Native renderer stats line: CPU share of the busiest host threads over the window")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
// [new_fix_24092026] GPU loss diagnostics: a DRED breadcrumb context (marker)
// per draw / blit / clear / resolve and names on the renderer's resources, so a
// device removal names the operation that was running. Read at startup: the
// DRED setting has to exist before the D3D12 device does.
// [new_fix_24092026] test only: remove the D3D12 device at this native frame.
REXCVAR_DEFINE_INT32(dp_native_test_gpu_loss_frame, 0, "DP1",
                     "Native renderer test: remove the D3D12 device at this frame to exercise the GPU-loss path "
                     "(0 = never)");
REXCVAR_DEFINE_BOOL(dp_native_gpu_markers, false, "DP1",
                    "Native renderer diagnostics: tag every GPU operation for DRED so a GPU loss names the draw "
                    "(small CPU cost per draw)");
// [new_fix_24092026] 2.0.2: off by default until the GPU losses seen with it
// on (24.09) are explained by a DRED log.
REXCVAR_DEFINE_BOOL(dp_native_gamma_ramp, false, "DP1",
                    "Native renderer: apply the console's display gamma ramp (DC_LUT) to the final image, as the "
                    "emulated path does")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(dp_native_edram_alias, true, "DP1",
                    "Native renderer: a render target re-bound over EDRAM tiles another target wrote since "
                    "starts with that content (copy, or 7e3 -> 8888 saturate), as on the console")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(dp_native_7e3_resolve, true, "DP1",
                    "Native renderer: clamp and quantize 7e3 (k_2_10_10_10_FLOAT) colour targets to the "
                    "console's range (max 31.875) when resolving them")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(dp_native_dump_all, false, "DP1", "Dump frame: detailed state for every draw (not just the first per shader/target key)");
REXCVAR_DEFINE_STRING(dp_native_dump_ps, "", "DP1",
                      "Dump frame: also record the bound render target after every draw whose pixel shader hash (hex) matches");
// [NEW FABLE VERSION] 2026-09-22: internal resolution multiplier (DPfix's
// renderWidth/shadowMapScale/reflectionScale in one knob). The game's own
// surfaces and the textures it resolves them into are allocated N times larger;
// every shader addresses them through normalized UVs, and the screen-space
// constants the game uploads (1/width texel steps) keep their screen-space
// meaning, so the picture is the same one at a higher sampling rate.
REXCVAR_DEFINE_INT32(dp_native_scale, 1, "DP1",
                     "Native renderer: internal resolution multiplier (1-4). 2 renders the scene, the reflections, the "
                     "shadow maps and the final image at twice the console's resolution");
REXCVAR_DEFINE_INT32(dp_native_dump_frame, 0, "DP1",
                     "Native renderer diagnostics: at this frame number write the front buffer and every resolve "
                     "destination texture as .ppm into logs/native_dump (0 = off)");

namespace dp::native {

using Microsoft::WRL::ComPtr;
namespace xenos = rex::graphics::xenos;

// ===========================================================================
// Guest memory helpers
// ===========================================================================
namespace {

inline rex::memory::Memory* Mem() { return REX_KERNEL_MEMORY(); }
inline uint32_t LoadU32(uint32_t guest_addr) {
  return rex::memory::load_and_swap<uint32_t>(Mem()->TranslateVirtual(guest_addr));
}
inline float LoadF32(uint32_t guest_addr) {
  const uint32_t u = LoadU32(guest_addr);
  float f;
  std::memcpy(&f, &u, 4);
  return f;
}
// Fetch constants carry GPU physical addresses (the XDK converts them).
inline const uint8_t* PhysPtr(uint32_t phys) { return Mem()->TranslatePhysical(phys & 0x1FFFFFFF); }
// XDK object headers (VB/IB +24) carry guest *virtual* addresses. The
// 0xE0000000 physical heap maps to the host with a +0x1000 offset
// (REX_PHYS_HOST_OFFSET in the recompiled code, PhysicalHeap::host_address_offset
// in the SDK); TranslateVirtual applies it, TranslatePhysical(addr & 0x1FFFFFFF)
// would read 4 KB early (the milestone-1 "dashed triangles").
inline const uint8_t* VirtPtr(uint32_t guest_virtual) { return Mem()->TranslateVirtual(guest_virtual); }
// Address taken from an XDK object header (VB/IB +24, texture fetch constant
// base_address): a guest virtual address, unless it is already physical.
inline const uint8_t* HeaderPtr(uint32_t addr) { return addr < 0x20000000u ? PhysPtr(addr) : VirtPtr(addr); }
// Guest physical address of a texture/buffer base the way the XDK converts it
// (SetStreamSource / DrawIndexedVertices): the 0xE0000000 heap sits 4 KB into
// physical memory. Only the physical heaps (A0/C0/E0) and raw physical
// addresses can be write-watched.
inline bool WatchableAddress(uint32_t addr) { return addr < 0x20000000u || addr >= 0xA0000000u; }
inline uint32_t GuestPhysical(uint32_t addr) {
  return addr < 0x20000000u ? addr : (addr & 0x1FFFFFFFu) + (((addr >> 20) + 0x200u) & 0x1000u);
}

// The XDK device block (created by the original CreateDevice; global 0x842AE74C).
constexpr uint32_t kDevicePointerGlobal = 0x842AE74C;
uint32_t g_device = 0;
inline uint32_t Dev(uint32_t offset) { return LoadU32(g_device + offset); }

// Device shadow offsets (PAL, d3d_surface.md + scout C 18.09).
constexpr uint32_t kDevVsConstants = 1920;     // 256 x float4, big-endian floats
constexpr uint32_t kDevPsConstants = 6016;     // 256 x float4
constexpr uint32_t kDevBoolConstants = 10112;  // 0x4900..0x4907 SHADER_CONSTANT_BOOL (256 bits), flushed by the draw path
constexpr uint32_t kDevLoopConstants = 10144;  // 0x4908..0x4927 SHADER_CONSTANT_LOOP (32 dwords) right after the bools
                                               // (10140 was one dword short: i0 read 0 -> skinning loops never ran -> white world)
constexpr uint32_t kDevRbColorMask = 10460;
constexpr uint32_t kDevAlphaRef = 10500;
constexpr uint32_t kDevRbDepthControl = 10548;
constexpr uint32_t kDevRbColorControl = 10556;
constexpr uint32_t kDevPaSuScModeCntl = 10568;
// PA_SU_POLY_OFFSET_FRONT_SCALE/OFFSET, BACK_SCALE/OFFSET (0x2380..0x2383), contiguous
// in the shadow (dev-block diff main pass vs shadow pass: 32.0 / 2^-22).
constexpr uint32_t kDevPaSuPolyOffsetFrontScale = 10832;
constexpr uint32_t kDevPaSuPolyOffsetFrontOffset = 10836;
constexpr uint32_t kDevPaSuPolyOffsetBackScale = 10840;
constexpr uint32_t kDevPaSuPolyOffsetBackOffset = 10844;
constexpr uint32_t kDevPaClVteCntl = 10572;  // viewport transform enable bits 0-5
constexpr uint32_t kDevPaScWindowScissorTl = 10436;  // PA_SC_WINDOW_SCISSOR_TL: x bits 0-14, y bits 16-30
constexpr uint32_t kDevPaScWindowScissorBr = 10440;  // PA_SC_WINDOW_SCISSOR_BR
constexpr uint32_t kDevPaClClipCntl = 10564;  // PA_CL_CLIP_CNTL: bit 0 ucp_ena_0 (the floor reflection pass)
constexpr uint32_t kDevPaClUcp0 = 8528;       // PA_CL_UCP_0 X,Y,Z,W (clip-space plane; dev-block scan, shadow is not linear here)
constexpr uint32_t kDevBlendFactors = 12024;    // game-level packed D3DRS blend states (diagnostics only)
constexpr uint32_t kDevBlendEnable = 12028;     // game-level ALPHABLENDENABLE (bit 31) / SEPARATEALPHA (bit 30) flags (diagnostics only)
constexpr uint32_t kDevRbBlendControl0 = 10552; // 0x2201 RB_BLENDCONTROL0: what the GPU actually gets (XDK writes 0x00010001 when blending is off)
constexpr uint32_t kRbBlendControlIdentity = 0x00010001;  // src ONE, dst ZERO, ADD for colour and alpha
constexpr uint32_t kDevFetchConstants = 1152;  // fetch constant n at 1152 + n * 24 (6 BE dwords)

}  // namespace

// ===========================================================================
// Shader cache (dp_native_shaders.bin, packed by pack_native_shaders.py)
// ===========================================================================
namespace {

struct ShaderCacheEntry {
  uint64_t hash;
  uint32_t dxil_offset;
  uint32_t dxil_size;
  uint32_t is_pixel;
  uint32_t reserved;
};

struct ShaderCache {
  std::vector<uint8_t> file;
  std::unordered_map<uint64_t, const ShaderCacheEntry*> by_hash;
  const uint8_t* dxil = nullptr;
  bool loaded = false;

  bool Load() {
    if (loaded) return true;
    loaded = true;
    // [new_fix_24092026] issue #33: native\ or the copy launcher updates
    // deliver to the shareable folder, whichever is newer (native_cache_path.h).
    const std::filesystem::path base = rex::filesystem::GetExecutableFolder();
    const auto found = FindShaderCache(base, WorkingDirOrEmpty());
    if (!found) {
      REXLOG_ERROR("Native renderer: shader cache {} not found (nor {}) - every draw will be skipped",
                   ShaderCacheZipPath(base).string(), ShaderCacheUpdaterPath(base).string());
      return false;
    }
    const std::filesystem::path path = *found;
    std::ifstream in(path, std::ios::binary);
    if (!in) {
      REXLOG_ERROR("Native renderer: shader cache {} cannot be opened - every draw will be skipped", path.string());
      return false;
    }
    REXLOG_INFO("Native renderer: shader cache {}", path.string());
    file.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    if (file.size() < 16 || std::memcmp(file.data(), "DPNS0001", 8) != 0) {
      REXLOG_ERROR("Native renderer: shader cache {} has a bad header", path.string());
      file.clear();
      return false;
    }
    uint32_t count, dxil_size;
    std::memcpy(&count, file.data() + 8, 4);
    std::memcpy(&dxil_size, file.data() + 12, 4);
    const auto* entries = reinterpret_cast<const ShaderCacheEntry*>(file.data() + 16);
    dxil = file.data() + 16 + size_t(count) * sizeof(ShaderCacheEntry);
    if (dxil + dxil_size > file.data() + file.size()) {
      REXLOG_ERROR("Native renderer: shader cache truncated");
      file.clear();
      return false;
    }
    for (uint32_t i = 0; i < count; ++i) by_hash[entries[i].hash] = &entries[i];
    REXLOG_INFO("Native renderer: shader cache loaded, {} shaders, {} bytes of DXIL", count, dxil_size);
    return true;
  }
  const ShaderCacheEntry* Find(uint64_t hash) const {
    auto it = by_hash.find(hash);
    return it == by_hash.end() ? nullptr : it->second;
  }
};
ShaderCache g_shader_cache;

}  // namespace

// ===========================================================================
// Guest object registry
// ===========================================================================
namespace {

struct GuestSurface;

struct GuestTexture {
  uint32_t guest = 0;
  xenos::xe_gpu_texture_fetch_t fetch{};
  rex::graphics::TextureInfo info{};
  bool info_valid = false;
  bool dirty = true;   // guest memory changed since the last upload
  uint64_t content_hash = 0;  // vertex-fetch textures: XXH3 of the guest memory at the last upload
  std::atomic<bool> written{false};  // set by the page-watch callback: the CPU wrote the guest memory
  bool watched = false;              // guest pages write-protected since the last upload
  bool unwatchable_logged = false;
  bool is_resolve_target = false;
  uint32_t width = 0, height = 0, depth = 1;
  DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
  bool is_cube = false;
  bool is_3d = false;         // k3D (volume or stacked): Texture3D on the host, slice RTVs
  bool is_stacked_3d = false;  // stacked: 2D-tiled slices in guest memory (volume: 3D tiling)
  // [NEW FABLE VERSION] host mip chain (guest levels 0..mip_max_level; 3D/stacked stay single-level).
  uint32_t mip_levels = 1;
  // [NEW FABLE VERSION] host resolution multiplier; only resolve destinations
  // scale (a CPU-uploaded texture must keep the guest's size and layout).
  uint32_t scale = 1;
  uint32_t hw() const { return width * scale; }
  uint32_t hh() const { return height * scale; }
  ComPtr<ID3D12Resource> resource;
  D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON;
  uint32_t srv_index = UINT32_MAX;
  uint32_t rtv_index = UINT32_MAX;  // when used as a resolve destination (2D: whole texture)
  std::vector<uint32_t> slice_rtvs;  // per array slice / cube face resolve destinations
  // [NEW FABLE VERSION] host resource holds a textures\<hash>.png replacement
  // (RGBA8 at the PNG's size) instead of the guest data.
  bool replaced = false;
  uint64_t replaced_hash = 0;
  // [NEW FABLE VERSION] 2026-09-24: g_mem_seq of the last write of this
  // texture's guest memory we know of (a resolve into it, or a CPU write the
  // page watch or the rehash saw); 0 = only its initial upload.
  uint64_t mem_seq = 0;
};

struct GuestSurface {
  uint32_t guest = 0;
  uint32_t width = 0, height = 0, guest_format = 0;
  uint32_t edram_base = 0;
  // [NEW FABLE VERSION] host resolution multiplier (1 = the console's size).
  uint32_t scale = 1;
  uint32_t hw() const { return width * scale; }
  uint32_t hh() const { return height * scale; }
  bool depth = false;
  DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
  ComPtr<ID3D12Resource> resource;
  D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON;
  uint32_t view_index = UINT32_MAX;  // RTV or DSV
  uint32_t srv_index = UINT32_MAX;
  // [NEW FABLE VERSION] 2026-09-23: g_edram_seq of this surface's last colour
  // write (draw, clear, EDRAM transfer); 0 = never written.
  uint64_t write_seq = 0;
  // [NEW FABLE VERSION] 2.0.1: g_edram_seq when SyncEdramAlias last scanned for
  // this surface; nothing to do again until another colour write happens.
  uint64_t alias_checked_seq = 0;
};

struct GuestBuffer {
  uint32_t guest = 0;
  uint32_t address = 0;  // physical
  uint32_t size = 0;
  bool index = false;
  bool index32 = false;  // index buffers: D3DIndexBuffer::Common bit 31 (XDK DrawIndexedVertices reads it there)
  bool dirty = true;
  std::atomic<bool> written{false};  // page-watch callback: the CPU wrote the guest memory since the upload
  bool watched = false;              // pages write-protected since the last upload
  uint32_t endian = 0;   // VB: fetch dword1 bits 0-1; IB: Common bits 29-30 (1 = 8in16, 2 = 8in32)
  ComPtr<ID3D12Resource> resource;
  D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON;
};

struct GuestDecl {
  uint32_t guest = 0;
  struct Element {
    uint16_t stream, offset;
    uint8_t type, method, usage, usage_index;
  };
  std::vector<Element> elements;
  std::vector<D3D12_INPUT_ELEMENT_DESC> layout;
  uint32_t swapped_texcoords = 0;
  uint32_t swapped_blend = 0;  // tfetchSwapBytes mode
  uint32_t normal_mode = 0;    // tfetchR11G11B10 mode
  uint64_t hash = 0;
};

// A vertex fetch as compiled into the microcode (the defaults the XDK leaves in
// place when the runtime declaration does not name the usage).
struct UcodeVfetch {
  uint8_t usage = 0, usage_index = 0;
  uint8_t stream = 0;        // 95 - fetch constant index (XDK stream mapping)
  uint8_t decl_type = 17;    // DeclType equivalent of format + signed + normalized (17 = unsupported)
  uint32_t offset = 0;       // bytes
  uint32_t stride = 0;       // bytes, as compiled
};
struct GuestShader {
  uint32_t guest = 0;
  uint64_t hash = 0;
  const ShaderCacheEntry* entry = nullptr;
  bool pixel = false;
  std::vector<UcodeVfetch> vfetches;  // vertex shaders only
  uint32_t container = 0, container_size = 0;  // guest container (the XDK patches its vfetches in place)
};

std::mutex g_registry_mutex;  // Create hooks may run on loading threads
std::unordered_map<uint32_t, std::unique_ptr<GuestTexture>> g_textures;
std::unordered_map<uint64_t, std::unique_ptr<GuestTexture>> g_textures_by_key;  // host textures: by fetch constant (memory), the truth
std::unordered_map<uint32_t, uint64_t> g_object_key;  // last fetch key seen through a texture object (Unlock -> dirty)
std::unordered_map<uint32_t, std::unique_ptr<GuestSurface>> g_surfaces;
uint64_t g_edram_seq = 0;  // [NEW FABLE VERSION] colour writes into EDRAM, in order (see dp_native_edram_alias)
uint32_t g_gamma_lut_srv = UINT32_MAX;  // [new_fix_24092026] 256x1 display gamma ramp (see dp_native_gamma_ramp)
// [NEW FABLE VERSION] 2026-09-24: guest memory writes in order, and the last
// resolve destination per physical base (see dp_native_resolve_alias). Host
// textures are never destroyed (g_textures_by_key only grows), so the raw
// pointers stay valid. Render thread only.
uint64_t g_mem_seq = 0;
std::unordered_map<uint32_t, GuestTexture*> g_resolve_by_base;
std::unordered_map<uint32_t, std::unique_ptr<GuestBuffer>> g_buffers;
std::unordered_map<uint32_t, std::unique_ptr<GuestDecl>> g_decls;
std::unordered_map<uint32_t, std::unique_ptr<GuestShader>> g_shaders;
// [new_fix_24092026] 2.0.3: objects the game released (or re-created at the same
// address), waiting for DestroyReleasedObjects. Under g_registry_mutex.
std::vector<std::unique_ptr<GuestBuffer>> g_released_buffers;
std::vector<std::unique_ptr<GuestSurface>> g_released_surfaces;

template <typename T>
T* Lookup(std::unordered_map<uint32_t, std::unique_ptr<T>>& map, uint32_t guest) {
  if (!guest) return nullptr;
  std::lock_guard<std::mutex> lock(g_registry_mutex);
  auto it = map.find(guest);
  return it == map.end() ? nullptr : it->second.get();
}

}  // namespace

// ===========================================================================
// Host D3D12 context
// ===========================================================================
namespace {

constexpr uint32_t kFramesInFlight = 2;
constexpr size_t kUploadRingBytes = 96u << 20;
constexpr uint32_t kViewHeapSize = 8192;
constexpr uint32_t kSamplerHeapSize = 256;
constexpr uint32_t kRtvHeapSize = 512;
constexpr uint32_t kDsvHeapSize = 128;
// [NEW FABLE VERSION] c48 = x: RT0 is 7e3 (PS clamps colour to 0..31.875), yz: sub-pixel jitter (clip units), w: reserved.
constexpr uint32_t kSharedConstantsBytes = 784;  // c0..c48 (c47 = user clip plane 0, c48 = dp1 misc)

struct UploadRing {
  ComPtr<ID3D12Resource> buffer;
  uint8_t* mapped = nullptr;
  size_t head = 0;
  bool Init(ID3D12Device* device) {
    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = kUploadRingBytes;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (FAILED(device->CreateCommittedResource(&rex::ui::d3d12::util::kHeapPropertiesUpload, D3D12_HEAP_FLAG_NONE, &desc,
                                               D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&buffer)))) {
      return false;
    }
    buffer->SetName(L"DP1 upload ring");  // [new_fix_24092026] 2.0.3
    D3D12_RANGE no_read = {0, 0};
    return SUCCEEDED(buffer->Map(0, &no_read, reinterpret_cast<void**>(&mapped)));
  }
  // Returns the offset or SIZE_MAX when full.
  size_t Allocate(size_t bytes, size_t align) {
    size_t offset = (head + align - 1) & ~(align - 1);
    if (offset + bytes > kUploadRingBytes) return SIZE_MAX;
    head = offset + bytes;
    return offset;
  }
};

struct DescriptorAllocator {
  ComPtr<ID3D12DescriptorHeap> heap;
  uint32_t size = 0, increment = 0, next = 0;
  std::vector<uint32_t> free_list;
  bool shader_visible = false;
  // [new_fix_24092026] 2.0.3: indices come back from the retire queue's drain,
  // which does not have to run on the thread that allocates.
  std::mutex mutex;
  bool Init(ID3D12Device* device, D3D12_DESCRIPTOR_HEAP_TYPE type, uint32_t count, bool visible) {
    D3D12_DESCRIPTOR_HEAP_DESC desc = {};
    desc.Type = type;
    desc.NumDescriptors = count;
    desc.Flags = visible ? D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE : D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    if (FAILED(device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&heap)))) return false;
    size = count;
    increment = device->GetDescriptorHandleIncrementSize(type);
    shader_visible = visible;
    return true;
  }
  uint32_t Allocate() {
    std::lock_guard<std::mutex> lock(mutex);
    if (!free_list.empty()) {
      uint32_t i = free_list.back();
      free_list.pop_back();
      return i;
    }
    if (next >= size) return UINT32_MAX;
    return next++;
  }
  void Free(uint32_t index) {
    if (index == UINT32_MAX) return;
    std::lock_guard<std::mutex> lock(mutex);
    free_list.push_back(index);
  }
  uint32_t InUse() {  // [new_fix_24092026] 2.0.3: stats
    std::lock_guard<std::mutex> lock(mutex);
    return next - uint32_t(free_list.size());
  }
  D3D12_CPU_DESCRIPTOR_HANDLE Cpu(uint32_t index) const {
    D3D12_CPU_DESCRIPTOR_HANDLE h = heap->GetCPUDescriptorHandleForHeapStart();
    h.ptr += SIZE_T(index) * increment;
    return h;
  }
  D3D12_GPU_DESCRIPTOR_HANDLE Gpu(uint32_t index) const {
    D3D12_GPU_DESCRIPTOR_HANDLE h = heap->GetGPUDescriptorHandleForHeapStart();
    h.ptr += UINT64(index) * increment;
    return h;
  }
};

struct PipelineKey {
  uint64_t vs = 0, ps = 0, decl = 0;
  uint32_t rt_format = 0, ds_format = 0;
  uint32_t blend = 0, blend_enable = 0, color_mask = 0;
  uint32_t depth_control = 0, cull = 0, topology = 0, strip_cut = 0;
  int32_t depth_bias = 0;
  uint32_t slope_bias_bits = 0;  // float bits
  uint32_t strides[2] = {0, 0};
  uint32_t instanced = 0;  // per-instance input layout (index-scaled vertex fetch shaders)
  bool operator==(const PipelineKey& o) const { return std::memcmp(this, &o, sizeof(*this)) == 0; }
};
struct PipelineKeyHash {
  size_t operator()(const PipelineKey& k) const { return size_t(XXH3_64bits(&k, sizeof(k))); }
};

struct SamplerKey {
  uint32_t value = 0;
  bool operator==(const SamplerKey& o) const { return value == o.value; }
};

struct Context {
  ID3D12Device* device = nullptr;
  ID3D12CommandQueue* queue = nullptr;
  rex::ui::d3d12::D3D12Provider* provider = nullptr;
  ComPtr<ID3D12CommandAllocator> allocators[kFramesInFlight];
  ComPtr<ID3D12GraphicsCommandList> list;
  ComPtr<ID3D12Fence> fence;
  HANDLE fence_event = nullptr;
  UINT64 fence_values[kFramesInFlight] = {};
  FenceCounter fences;  // [new_fix_24092026] 2.0.3: frame fence values (native_retire.h); read on any thread
  uint32_t frame_index = 0;
  UploadRing upload[kFramesInFlight];
  DescriptorAllocator views, samplers, rtvs, dsvs;
  ComPtr<ID3D12RootSignature> root_signature;
  ComPtr<ID3D12RootSignature> blit_root_signature;
  ComPtr<ID3DBlob> blit_vs, blit_ps;
  std::unordered_map<uint32_t, ComPtr<ID3D12PipelineState>> blit_psos;  // by dest DXGI format
  std::unordered_map<PipelineKey, ComPtr<ID3D12PipelineState>, PipelineKeyHash> psos;
  std::unordered_map<uint64_t, uint32_t> sampler_indices;  // key -> heap index ([NEW FABLE VERSION] 64-bit: mip range + lod bias)
  // [NEW FABLE VERSION] GPU frame timing: two timestamps per frame slot, read back
  // after the fence wait for that slot (two frames later).
  ComPtr<ID3D12QueryHeap> ts_heap;
  ComPtr<ID3D12Resource> ts_readback;
  uint64_t* ts_mapped = nullptr;
  uint64_t ts_frequency = 0;
  bool ts_pending[kFramesInFlight] = {};
  ComPtr<ID3D12Resource> white_texture;
  uint32_t white_srv = UINT32_MAX;
  // Null descriptors (read as zeros): what an unbound / invalid fetch constant
  // samples on the console (Xenia binds its null texture). A white fallback
  // made the light pass read an unbound lightmap (tLmap, s8) as 1.0: washed-out
  // rooms, white hair and floors.
  uint32_t null_srv_2d = UINT32_MAX, null_srv_3d = UINT32_MAX, null_srv_cube = UINT32_MAX;
  bool recording = false;
  bool ready = false;
  bool failed = false;
  // [new_fix_24092026] 2.0.3: retired objects live in g_retired (fence-tagged), see native_retire.h.
  RendererStats stats;
  uint32_t frame_number = 0;
  // [NEW FABLE VERSION] constant banks: guest bytes of the last upload (big-endian,
  // compared with memcmp) and where that upload lives in this frame's ring.
  alignas(16) uint8_t vs_bank_cache[4096] = {};
  alignas(16) uint8_t ps_bank_cache[4096] = {};
  D3D12_GPU_VIRTUAL_ADDRESS vs_bank_gpu = 0, ps_bank_gpu = 0;
  uint32_t bank_cache_frame = UINT32_MAX;  // frame_number the cached addresses belong to
  bool bank_cache_valid = false;
  uint32_t const_uploads = 0, const_reuses = 0;  // per-frame counters
  uint32_t log_budget = 200;
};
Context g;

// [NEW FABLE VERSION] Resolution multiplier for a render target of this guest
// size (dp_native_scale, read once in InitContext). The small fixed-function
// chains (the 1x1 luminance ladder with its 256x64 / 32x32 steps, the tiny
// masks) stay at the console's size: they carry averages rather than an image,
// so leaving them alone keeps their arithmetic identical to the 1x path.
uint32_t g_scale = 1;
inline uint32_t ScaleFor(uint32_t w, uint32_t h) { return ScaleForTarget(w, h, g_scale); }

// [NEW FABLE VERSION] Frame timing window (QueryPerformanceCounter): the frame
// interval between Swaps, the CPU time inside the native path, the fence wait
// and the GPU time per frame. Summarised (p50/p90/p99) by PerfSummaryAndReset.
inline int64_t Qpc() {
  LARGE_INTEGER li;
  QueryPerformanceCounter(&li);
  return li.QuadPart;
}
inline double QpcToMs(int64_t ticks) {
  static const double k = [] {
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    return 1000.0 / double(f.QuadPart);
  }();
  return double(ticks) * k;
}
struct PerfWindow {
  std::vector<float> frame_ms, cpu_ms, wait_ms, gpu_ms;
  std::vector<float> thread_ms;      // [new_fix_24092026] main thread CPU per frame
  uint64_t last_thread_cycles = 0;   // [new_fix_24092026]
  int64_t last_swap = 0;
  int64_t cpu_ticks = 0;   // native path CPU time this frame
  int64_t wait_ticks = 0;  // fence waits this frame
};
PerfWindow g_perf;
struct ScopedCpuTimer {
  int64_t t0 = Qpc();
  ~ScopedCpuTimer() { g_perf.cpu_ticks += Qpc() - t0; }
};

// [new_fix_24092026] dp_native_gpu_markers (see LogDeviceRemoval).
bool GpuMarkersOn() {
  static const bool on = REXCVAR_GET(dp_native_gpu_markers);
  return on;
}
// A PIX-style Unicode marker (metadata 0): DRED records it as the breadcrumb
// context of the next operation.
void GpuMarker(const std::string& s) {
  if (!GpuMarkersOn() || !g.list) return;
  const std::wstring w(s.begin(), s.end());
  g.list->SetMarker(0, w.c_str(), UINT((w.size() + 1) * sizeof(wchar_t)));
}
// Names cost nothing per frame (once per resource) and make a DRED page fault
// name the resource, so they are always on; only the per-draw markers wait for
// dp_native_gpu_markers.
void NameResource(ID3D12Object* o, const std::string& s) {
  if (!o) return;
  const std::wstring w(s.begin(), s.end());
  o->SetName(w.c_str());
}

// [new_fix_24092026] TSC ticks per millisecond, measured against QPC since the
// first call (QueryThreadCycleTime counts TSC ticks). 0 for the first 100 ms.
double TscPerMs() {
  static const int64_t q0 = Qpc();
  static const uint64_t t0 = __rdtsc();
  const double ms = QpcToMs(Qpc() - q0);
  return ms > 100.0 ? double(__rdtsc() - t0) / ms : 0.0;
}

// [new_fix_24092026] dp_native_perf_threads: CPU share of each host thread since
// the previous call (1.00 = one core busy the whole window), busiest 8, and the
// sum over all threads. Called once per stats window from the Swap thread.
std::string ThreadCpuReport() {
  static std::unordered_map<DWORD, uint64_t> prev;
  static uint64_t prev_tsc = 0;
  const uint64_t tsc = __rdtsc();
  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
  if (snap == INVALID_HANDLE_VALUE) return "threads n/a";
  const DWORD pid = GetCurrentProcessId();
  struct Row {
    double share;
    std::string name;
  };
  std::vector<Row> rows;
  std::unordered_map<DWORD, uint64_t> next;
  double total = 0.0;
  THREADENTRY32 te = {};
  te.dwSize = sizeof(te);
  for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
    if (te.th32OwnerProcessID != pid) continue;
    HANDLE h = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, te.th32ThreadID);
    if (!h) continue;
    ULONG64 cycles = 0;
    if (QueryThreadCycleTime(h, &cycles)) {
      next[te.th32ThreadID] = cycles;
      const auto it = prev.find(te.th32ThreadID);
      const uint64_t before = it != prev.end() ? it->second : 0;  // new thread: all of its cycles are in this window
      if (prev_tsc && tsc > prev_tsc && cycles >= before) {
        const double share = double(cycles - before) / double(tsc - prev_tsc);
        total += share;
        std::string name;
        PWSTR desc = nullptr;
        // Looked up at run time: a static import would stop the exe from
        // loading at all on Windows 10 before 1607.
        using GetThreadDescriptionFn = HRESULT(WINAPI*)(HANDLE, PWSTR*);
        static const auto get_desc = reinterpret_cast<GetThreadDescriptionFn>(
            GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetThreadDescription"));
        if (get_desc && SUCCEEDED(get_desc(h, &desc)) && desc) {
          const int n = WideCharToMultiByte(CP_UTF8, 0, desc, -1, nullptr, 0, nullptr, nullptr);
          if (n > 1) {
            name.resize(size_t(n - 1));
            WideCharToMultiByte(CP_UTF8, 0, desc, -1, name.data(), n, nullptr, nullptr);
          }
          LocalFree(desc);
        }
        if (name.empty()) name = fmt::format("tid {}", te.th32ThreadID);
        rows.push_back({share, std::move(name)});
      }
    }
    CloseHandle(h);
  }
  CloseHandle(snap);
  prev.swap(next);
  const bool first = prev_tsc == 0;
  prev_tsc = tsc;
  if (first) return "threads: first window";
  std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.share > b.share; });
  std::string s = fmt::format("cpu cores busy {:.2f} ({} threads):", total, rows.size());
  for (size_t i = 0; i < rows.size() && i < 8; ++i) s += fmt::format(" [{} {:.2f}]", rows[i].name, rows[i].share);
  return s;
}

// ---------------------------------------------------------------------------
// Blit shader: full-screen triangle sampling t0 into the bound RTV.
// ---------------------------------------------------------------------------
const char kBlitHlsl[] = R"(
Texture2D<float4> t0 : register(t0);
Texture2D<float4> t1 : register(t1);  // [new_fix_24092026] display gamma ramp (mode 4)
SamplerState s0 : register(s0);
struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
VSOut VSMain(uint id : SV_VertexID) {
  VSOut o;
  float2 uv = float2((id << 1) & 2, id & 2);
  o.pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
  o.uv = uv;
  return o;
}
cbuffer BlitParams : register(b0) { uint g_mode; }
// 7e3 float (Xenos k_2_10_10_10_FLOAT colour channels): 3-bit exponent, 7-bit
// mantissa, no sign; max (1 + 127/128) * 2^4 = 31.875, denormal step 2^-10.
float To7e3(float v) {
  v = clamp(v, 0.0, 31.875);
  if (v < 0.125) return round(v * 1024.0) / 1024.0;
  float e = floor(log2(v));
  float scale = exp2(e) / 128.0;
  return round(v / scale) * scale;
}
float4 PSMain(VSOut i) : SV_Target0 {
  float4 c = t0.SampleLevel(s0, i.uv, 0);
  if (g_mode & 1u) {
    c.rgb = float3(To7e3(c.r), To7e3(c.g), To7e3(c.b));
  }
  // [NEW FABLE VERSION] 2026-09-23: fixed-point alpha of a 7e3 target, see
  // dp_native_7e3_alpha.
  if (g_mode & 2u) {
    c.a = saturate(c.a);
  }
  // [new_fix_24092026] the console's display gamma ramp: the 8-bit front buffer
  // value indexes the 256-entry table (t1, 256x1 R10G10B10A2), per channel.
  // Between two entries the value is interpolated: exact for an 8-bit input,
  // no extra banding for a filtered or wider source.
  if (g_mode & 4u) {
    float3 x = saturate(c.rgb) * 255.0;
    uint3 lo = uint3(floor(x));
    uint3 hi = min(lo + 1u, 255u);
    float3 f = x - float3(lo);
    c.r = lerp(t1.Load(int3(lo.r, 0, 0)).r, t1.Load(int3(hi.r, 0, 0)).r, f.r);
    c.g = lerp(t1.Load(int3(lo.g, 0, 0)).g, t1.Load(int3(hi.g, 0, 0)).g, f.g);
    c.b = lerp(t1.Load(int3(lo.b, 0, 0)).b, t1.Load(int3(hi.b, 0, 0)).b, f.b);
  }
  return c;
}
)";

bool CompileBlit() {
  ComPtr<ID3DBlob> err;
  if (FAILED(D3DCompile(kBlitHlsl, sizeof(kBlitHlsl) - 1, "blit", nullptr, nullptr, "VSMain", "vs_5_0", 0, 0,
                        &g.blit_vs, &err)) ||
      FAILED(D3DCompile(kBlitHlsl, sizeof(kBlitHlsl) - 1, "blit", nullptr, nullptr, "PSMain", "ps_5_0", 0, 0,
                        &g.blit_ps, &err))) {
    REXLOG_ERROR("Native renderer: blit shader compile failed: {}",
                 err ? std::string(static_cast<const char*>(err->GetBufferPointer()), err->GetBufferSize()) : "?");
    return false;
  }
  D3D12_DESCRIPTOR_RANGE range = {};
  range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
  range.NumDescriptors = 1;
  // [new_fix_24092026] t1: the display gamma ramp table for the final blit.
  D3D12_DESCRIPTOR_RANGE range_lut = {};
  range_lut.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
  range_lut.NumDescriptors = 1;
  range_lut.BaseShaderRegister = 1;
  D3D12_ROOT_PARAMETER params[3] = {};
  params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  params[2].DescriptorTable.NumDescriptorRanges = 1;
  params[2].DescriptorTable.pDescriptorRanges = &range_lut;
  params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
  params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  params[0].DescriptorTable.NumDescriptorRanges = 1;
  params[0].DescriptorTable.pDescriptorRanges = &range;
  params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
  params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;  // b0: blit mode (1 = 7e3 range)
  params[1].Constants.ShaderRegister = 0;
  params[1].Constants.Num32BitValues = 1;
  params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
  D3D12_STATIC_SAMPLER_DESC sampler = {};
  sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
  sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
  sampler.MaxLOD = D3D12_FLOAT32_MAX;
  sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
  D3D12_ROOT_SIGNATURE_DESC desc = {};
  desc.NumParameters = 3;  // [new_fix_24092026] + the gamma ramp table
  desc.pParameters = params;
  desc.NumStaticSamplers = 1;
  desc.pStaticSamplers = &sampler;
  ComPtr<ID3DBlob> blob;
  if (FAILED(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err))) return false;
  return SUCCEEDED(g.device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                                 IID_PPV_ARGS(&g.blit_root_signature)));
}

ID3D12PipelineState* BlitPso(DXGI_FORMAT dest) {
  auto it = g.blit_psos.find(dest);
  if (it != g.blit_psos.end()) return it->second.Get();
  D3D12_GRAPHICS_PIPELINE_STATE_DESC d = {};
  d.pRootSignature = g.blit_root_signature.Get();
  d.VS = {g.blit_vs->GetBufferPointer(), g.blit_vs->GetBufferSize()};
  d.PS = {g.blit_ps->GetBufferPointer(), g.blit_ps->GetBufferSize()};
  d.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
  d.SampleMask = UINT_MAX;
  d.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
  d.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
  d.RasterizerState.DepthClipEnable = TRUE;
  d.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  d.NumRenderTargets = 1;
  d.RTVFormats[0] = dest;
  d.SampleDesc.Count = 1;
  ComPtr<ID3D12PipelineState> pso;
  if (FAILED(g.device->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&pso)))) {
    REXLOG_ERROR("Native renderer: blit PSO for format {} failed", uint32_t(dest));
    return nullptr;
  }
  g.blit_psos[dest] = pso;
  return pso.Get();
}

// ---------------------------------------------------------------------------
// Game root signature (XenosRecomp dpxr-fork contract, shader_recompiler.cpp):
//   b0 space4 = VS float constants (256 float4), b1 space4 = PS float constants,
//   b2 space4 = SharedConstants (720 B); t0 space0/1/2/6 = bindless 2D/3D/cube/
//   1D SRVs (one heap, all ranges at offset 0); s0 space3 = bindless samplers.
// ---------------------------------------------------------------------------
bool CreateRootSignature() {
  D3D12_DESCRIPTOR_RANGE srv_ranges[4] = {};
  const uint32_t spaces[4] = {0, 1, 2, 6};
  for (int i = 0; i < 4; ++i) {
    srv_ranges[i].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srv_ranges[i].NumDescriptors = kViewHeapSize;
    srv_ranges[i].BaseShaderRegister = 0;
    srv_ranges[i].RegisterSpace = spaces[i];
    srv_ranges[i].OffsetInDescriptorsFromTableStart = 0;
  }
  D3D12_DESCRIPTOR_RANGE sampler_range = {};
  sampler_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
  sampler_range.NumDescriptors = kSamplerHeapSize;
  sampler_range.RegisterSpace = 3;
  D3D12_ROOT_PARAMETER params[5] = {};
  for (int i = 0; i < 3; ++i) {
    params[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[i].Descriptor.ShaderRegister = i;
    params[i].Descriptor.RegisterSpace = 4;
    params[i].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  }
  params[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  params[3].DescriptorTable.NumDescriptorRanges = 4;
  params[3].DescriptorTable.pDescriptorRanges = srv_ranges;
  params[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  params[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  params[4].DescriptorTable.NumDescriptorRanges = 1;
  params[4].DescriptorTable.pDescriptorRanges = &sampler_range;
  params[4].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  D3D12_ROOT_SIGNATURE_DESC desc = {};
  desc.NumParameters = 5;
  desc.pParameters = params;
  desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
  ComPtr<ID3DBlob> blob, err;
  if (FAILED(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err))) {
    REXLOG_ERROR("Native renderer: root signature: {}",
                 err ? std::string(static_cast<const char*>(err->GetBufferPointer()), err->GetBufferSize()) : "?");
    return false;
  }
  return SUCCEEDED(
      g.device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&g.root_signature)));
}

bool CreateWhiteTexture() {
  D3D12_RESOURCE_DESC desc = {};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  desc.Width = 1;
  desc.Height = 1;
  desc.DepthOrArraySize = 1;
  desc.MipLevels = 1;
  desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  desc.SampleDesc.Count = 1;
  if (FAILED(g.device->CreateCommittedResource(&rex::ui::d3d12::util::kHeapPropertiesDefault, D3D12_HEAP_FLAG_NONE, &desc,
                                               D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&g.white_texture)))) {
    return false;
  }
  g.white_texture->SetName(L"DP1 white 1x1");  // [new_fix_24092026] 2.0.3
  g.white_srv = g.views.Allocate();
  g.device->CreateShaderResourceView(g.white_texture.Get(), nullptr, g.views.Cpu(g.white_srv));
  {
    D3D12_SHADER_RESOURCE_VIEW_DESC n = {};
    n.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    n.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    n.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    n.Texture2D.MipLevels = 1;
    g.null_srv_2d = g.views.Allocate();
    g.device->CreateShaderResourceView(nullptr, &n, g.views.Cpu(g.null_srv_2d));
    n.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
    n.Texture3D.MipLevels = 1;
    g.null_srv_3d = g.views.Allocate();
    g.device->CreateShaderResourceView(nullptr, &n, g.views.Cpu(g.null_srv_3d));
    n.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
    n.TextureCube.MipLevels = 1;
    g.null_srv_cube = g.views.Allocate();
    g.device->CreateShaderResourceView(nullptr, &n, g.views.Cpu(g.null_srv_cube));
  }
  return true;
}

// [new_fix_24092026] 2.0.3 (issues #33/#34, native_retire.h): what the renderer
// stops using waits for the fence value of the frame being recorded (between
// frames: of the next one). Up to 2.0.2 it went into the list of the current
// frame slot, and a resource retired between frames or from a loading thread
// was released while the frame just submitted could still read it.
struct RetiredObject {
  ComPtr<ID3D12Resource> resource;
  DescriptorAllocator* heap = nullptr;  // a descriptor index to give back to `heap`
  uint32_t index = UINT32_MAX;
};
// Never destroyed: the guest's loading threads can still release objects while
// the process exits.
RetireQueue<RetiredObject>& Retired() {
  static auto* queue = new RetireQueue<RetiredObject>();
  return *queue;
}
std::atomic<uint32_t> g_retired_count{0}, g_freed_count{0}, g_recreated_count{0};

UINT64 RetireFence() { return g.fences.ForRetire(); }

void RetireResource(ComPtr<ID3D12Resource> r) {
  if (!r) return;
  Retired().Push(RetireFence(), RetiredObject{std::move(r), nullptr, UINT32_MAX});
  g_retired_count.fetch_add(1, std::memory_order_relaxed);
}

void RetireDescriptor(DescriptorAllocator& heap, uint32_t index) {
  if (index == UINT32_MAX) return;
  Retired().Push(RetireFence(), RetiredObject{nullptr, &heap, index});
}

// Frame start, after the wait: everything the GPU has finished with goes (the
// resources are released when `done` goes out of scope).
void ReleaseRetired() {
  std::vector<RetiredObject> done = Retired().TakeCompleted(g.fence->GetCompletedValue());
  for (RetiredObject& o : done) {
    if (o.heap) o.heap->Free(o.index);
    if (o.resource) g_freed_count.fetch_add(1, std::memory_order_relaxed);
  }
}

// [new_fix_24092026] 2.0.3: a full descriptor heap is logged (a few times) and
// the caller gives up on that object instead of writing past the heap.
void HeapFull(const char* what) {
  static std::atomic<uint32_t> logged{0};
  if (logged.fetch_add(1, std::memory_order_relaxed) < 8) {
    REXLOG_ERROR("Native renderer: {} descriptor heap full, the object is not drawn", what);
  }
}

// [new_fix_24092026] 2.0.3: the recording path writes one command list, so the
// guest threads that reach it (draws, clears and resolves on the game's
// RenderThread, resolves and Swap on its main thread) must take turns. The game
// does that itself; this lock makes sure of it, and the first times a thread
// has to wait for another one are logged (that would be a bug of its own).
std::recursive_mutex g_record_mutex;
std::atomic<DWORD> g_record_owner{0};
std::atomic<uint32_t> g_record_overlaps{0};
struct RecordScope {
  std::unique_lock<std::recursive_mutex> lock;
  DWORD previous_owner = 0;
  explicit RecordScope(const char* where) : lock(g_record_mutex, std::try_to_lock) {
    if (!lock.owns_lock()) {
      const DWORD holder = g_record_owner.load(std::memory_order_relaxed);
      const uint32_t n = g_record_overlaps.fetch_add(1, std::memory_order_relaxed);
      if (n < 8) {
        REXLOG_WARN("Native renderer: {} on thread {} waits for thread {}, still recording (overlap #{})", where,
                    GetCurrentThreadId(), holder, n + 1);
      }
      lock.lock();
    }
    previous_owner = g_record_owner.exchange(GetCurrentThreadId(), std::memory_order_relaxed);
  }
  ~RecordScope() { g_record_owner.store(previous_owner, std::memory_order_relaxed); }
};

void ForgetWatch(GuestBuffer* b);  // page watch (defined with UploadTexture)

// [new_fix_24092026] 2.0.3: frame start, on the recording thread (inside
// RecordScope, before any Lookup of this frame). What OnRelease, RegisterBuffer
// and OnCreateSurface unlinked is torn down here: the recording thread may have
// been using the object while another thread released it. Resources and shader
// views go through the retire queue, RTV/DSV indices back at once (CPU-only
// heaps, read when a command is recorded), then the object is destroyed.
void DestroyReleasedObjects() {
  std::vector<std::unique_ptr<GuestBuffer>> buffers;
  std::vector<std::unique_ptr<GuestSurface>> surfaces;
  {
    std::lock_guard<std::mutex> lock(g_registry_mutex);
    buffers.swap(g_released_buffers);
    surfaces.swap(g_released_surfaces);
  }
  for (auto& b : buffers) {
    ForgetWatch(b.get());
    RetireResource(std::move(b->resource));
  }
  for (auto& s : surfaces) {
    (s->depth ? g.dsvs : g.rtvs).Free(s->view_index);
    RetireDescriptor(g.views, s->srv_index);
    RetireResource(std::move(s->resource));
  }
}

void Barrier(ID3D12Resource* r, D3D12_RESOURCE_STATES& state, D3D12_RESOURCE_STATES to) {
  if (state == to) return;
  D3D12_RESOURCE_BARRIER b = {};
  b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  b.Transition.pResource = r;
  b.Transition.StateBefore = state;
  b.Transition.StateAfter = to;
  b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  g.list->ResourceBarrier(1, &b);
  state = to;
}

bool BeginFrame() {
  if (!g.ready) return false;
  if (g.recording) return true;
  // Wait for the frame that used this allocator two frames ago.
  const UINT64 needed = g.fence_values[g.frame_index];
  if (needed && g.fence->GetCompletedValue() < needed) {
    const int64_t w0 = Qpc();  // [NEW FABLE VERSION] fence wait accounting
    g.fence->SetEventOnCompletion(needed, g.fence_event);
    WaitForSingleObject(g.fence_event, INFINITE);
    g_perf.wait_ticks += Qpc() - w0;
  }
  // [NEW FABLE VERSION] the GPU timestamps of the frame that used this slot are complete now.
  if (g.ts_mapped && g.ts_pending[g.frame_index]) {
    g.ts_pending[g.frame_index] = false;
    const uint64_t t0 = g.ts_mapped[g.frame_index * 2], t1 = g.ts_mapped[g.frame_index * 2 + 1];
    if (t1 > t0 && g.ts_frequency) g_perf.gpu_ms.push_back(float(double(t1 - t0) * 1000.0 / double(g.ts_frequency)));
  }
  DestroyReleasedObjects();  // [new_fix_24092026] 2.0.3
  ReleaseRetired();          // [new_fix_24092026] 2.0.3: by fence, not by frame slot (native_retire.h)
  g.allocators[g.frame_index]->Reset();
  g.list->Reset(g.allocators[g.frame_index].Get(), nullptr);
  g.upload[g.frame_index].head = 0;
  ID3D12DescriptorHeap* heaps[] = {g.views.heap.Get(), g.samplers.heap.Get()};
  g.list->SetDescriptorHeaps(2, heaps);
  if (g.ts_heap) g.list->EndQuery(g.ts_heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, g.frame_index * 2);
  g.recording = true;
  g.stats = RendererStats{};
  // Upload the white texture once.
  static bool white_uploaded = false;
  if (!white_uploaded) {
    white_uploaded = true;
    UploadRing& ring = g.upload[g.frame_index];
    const size_t off = ring.Allocate(256, 512);
    std::memset(ring.mapped + off, 0xFF, 256);
    D3D12_TEXTURE_COPY_LOCATION dst = {}, src = {};
    dst.pResource = g.white_texture.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.pResource = ring.buffer.Get();
    src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src.PlacedFootprint.Offset = off;
    src.PlacedFootprint.Footprint = {DXGI_FORMAT_R8G8B8A8_UNORM, 1, 1, 1, 256};
    g.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    D3D12_RESOURCE_STATES s = D3D12_RESOURCE_STATE_COPY_DEST;
    Barrier(g.white_texture.Get(), s, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  }
  return true;
}

void SubmitFrame() {
  if (!g.recording) return;
  if (g.ts_heap) {  // [NEW FABLE VERSION] GPU frame timing
    g.list->EndQuery(g.ts_heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, g.frame_index * 2 + 1);
    g.list->ResolveQueryData(g.ts_heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, g.frame_index * 2, 2, g.ts_readback.Get(),
                             UINT64(g.frame_index) * 2 * sizeof(uint64_t));
    g.ts_pending[g.frame_index] = true;
  }
  g.list->Close();
  ID3D12CommandList* lists[] = {g.list.Get()};
  g.queue->ExecuteCommandLists(1, lists);
  // [new_fix_24092026] 2.0.3: signal the value, then move on (FenceCounter, tested)
  g.fence_values[g.frame_index] = g.fences.Submit([](uint64_t value) { g.queue->Signal(g.fence.Get(), value); });
  g.frame_index = (g.frame_index + 1) % kFramesInFlight;
  g.recording = false;
  g.provider->LogDebugLayerMessages();
}

// page watch (defined with UploadTexture)
extern void* g_watch_handle;
std::pair<uint32_t, uint32_t> WatchCallback(void*, uint32_t phys_start, uint32_t length, bool exact_range);

bool InitContext() {
  if (g.ready || g.failed) return g.ready;
  auto* system = NativeGraphicsSystem::instance();
  if (!system || !system->provider()) return false;
  g.provider = static_cast<rex::ui::d3d12::D3D12Provider*>(system->provider());
  g.device = g.provider->GetDevice();
  g.queue = g.provider->GetDirectQueue();
  bool ok = true;
  for (uint32_t i = 0; i < kFramesInFlight && ok; ++i) {
    ok = SUCCEEDED(g.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g.allocators[i]))) &&
         g.upload[i].Init(g.device);
  }
  ok = ok && SUCCEEDED(g.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g.allocators[0].Get(), nullptr,
                                                   IID_PPV_ARGS(&g.list)));
  if (ok) g.list->Close();
  if (ok) g.list->SetName(L"DP1 native frame list");  // [new_fix_24092026] DRED names the list
  ok = ok && SUCCEEDED(g.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g.fence)));
  g.fence_event = CreateEvent(nullptr, FALSE, FALSE, nullptr);
  ok = ok && g.fence_event != nullptr;
  ok = ok && g.views.Init(g.device, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, kViewHeapSize, true);
  ok = ok && g.samplers.Init(g.device, D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, kSamplerHeapSize, true);
  ok = ok && g.rtvs.Init(g.device, D3D12_DESCRIPTOR_HEAP_TYPE_RTV, kRtvHeapSize, false);
  ok = ok && g.dsvs.Init(g.device, D3D12_DESCRIPTOR_HEAP_TYPE_DSV, kDsvHeapSize, false);
  ok = ok && CreateRootSignature() && CompileBlit() && CreateWhiteTexture();
  if (ok) {
    // [NEW FABLE VERSION] timestamp queries for the GPU time per frame (optional: the renderer works without them).
    D3D12_QUERY_HEAP_DESC qd = {};
    qd.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    qd.Count = 2 * kFramesInFlight;
    if (SUCCEEDED(g.device->CreateQueryHeap(&qd, IID_PPV_ARGS(&g.ts_heap)))) {
      D3D12_RESOURCE_DESC rd = {};
      rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
      rd.Width = 2 * kFramesInFlight * sizeof(uint64_t);
      rd.Height = 1;
      rd.DepthOrArraySize = 1;
      rd.MipLevels = 1;
      rd.SampleDesc.Count = 1;
      rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
      if (SUCCEEDED(g.device->CreateCommittedResource(&rex::ui::d3d12::util::kHeapPropertiesReadback, D3D12_HEAP_FLAG_NONE,
                                                      &rd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                      IID_PPV_ARGS(&g.ts_readback))) &&
          SUCCEEDED(g.ts_readback->SetName(L"DP1 GPU timestamps")) &&  // [new_fix_24092026] 2.0.3
          SUCCEEDED(g.ts_readback->Map(0, nullptr, reinterpret_cast<void**>(&g.ts_mapped))) &&
          SUCCEEDED(g.queue->GetTimestampFrequency(&g.ts_frequency))) {
        // ready
      } else {
        g.ts_mapped = nullptr;
        g.ts_readback.Reset();
        g.ts_heap.Reset();
      }
    }
  }
  g_shader_cache.Load();
  if (!ok) {
    REXLOG_ERROR("Native renderer: D3D12 context creation failed; draws are skipped");
    g.failed = true;
    return false;
  }
  // [NEW FABLE VERSION] internal resolution multiplier, read once (a change mid-frame
  // would leave half the targets at the old size).
  g_scale = ClampScale(REXCVAR_GET(dp_native_scale));
  if (g_scale != 1) REXLOG_INFO("Native renderer: internal resolution scale {}x (dp_native_scale)", g_scale);
  g_watch_handle = Mem()->RegisterPhysicalMemoryInvalidationCallback(WatchCallback, nullptr);
  REXLOG_INFO("Native renderer: page watch {}", g_watch_handle ? "registered" : "UNAVAILABLE (textures re-upload on Unlock only)");
  g.ready = true;
  REXLOG_INFO("Native renderer: D3D12 context ready (views {}, samplers {}, upload ring {} MB x {})", kViewHeapSize,
              kSamplerHeapSize, kUploadRingBytes >> 20, kFramesInFlight);
  return true;
}

}  // namespace

// ===========================================================================
// Format mapping
// ===========================================================================
namespace {

// Guest D3DFORMAT dwords seen in DP1 (scout C 18.09) -> host format.
struct SurfaceFormat {
  DXGI_FORMAT format;
  bool depth;
};
// [NEW FABLE VERSION] 2026-09-23: the guest format -> host class decision lives
// in native_scale.h (ClassifySurfaceFormat, tested); 32 = k_16_16_16_16_FLOAT
// used to fall through to RGBA8.
SurfaceFormat MapSurfaceFormat(uint32_t guest_format) {
  switch (ClassifySurfaceFormat(guest_format)) {
    case SurfaceClass::kDepth24:
      // [NEW FABLE VERSION] the console's 24-bit UNORM depth: the polygon offset
      // (shadow casters) is converted in 1/2^24 units, which only holds on a
      // UNORM24 host format (on D32_FLOAT the bias scales with the depth value).
      return {DXGI_FORMAT_D24_UNORM_S8_UINT, true};
    case SurfaceClass::kDepthF24:
      return {DXGI_FORMAT_D32_FLOAT, true};
    case SurfaceClass::kRgba16F:
      return {DXGI_FORMAT_R16G16B16A16_FLOAT, false};
    case SurfaceClass::kRg16F:
      return {DXGI_FORMAT_R16G16_FLOAT, false};
    case SurfaceClass::kR32F:
      return {DXGI_FORMAT_R32_FLOAT, false};
    case SurfaceClass::kR8:
      return {DXGI_FORMAT_R8_UNORM, false};
    case SurfaceClass::kRgba8:
    default:
      return {DXGI_FORMAT_R8G8B8A8_UNORM, false};
  }
}

// Texture formats for CPU-uploaded guest textures (after CopySwapBlock with
// the fetch constant's endian). Returns UNKNOWN for unsupported formats.
DXGI_FORMAT MapTextureFormat(xenos::TextureFormat f) {
  switch (f) {
    case xenos::TextureFormat::k_8_8_8_8:
      // Memory order after the fetch-endian swap, exactly like the emulator's
      // texture cache; the guest fetch swizzle (A8R8G8B8 = .zyxw) is applied
      // on the SRV (HostSwizzle / SrvMapping below).
      return DXGI_FORMAT_R8G8B8A8_UNORM;
    case xenos::TextureFormat::k_DXT1:
      return DXGI_FORMAT_BC1_UNORM;
    case xenos::TextureFormat::k_DXT2_3:
      return DXGI_FORMAT_BC2_UNORM;
    case xenos::TextureFormat::k_DXT4_5:
      return DXGI_FORMAT_BC3_UNORM;
    case xenos::TextureFormat::k_8:
      return DXGI_FORMAT_R8_UNORM;
    case xenos::TextureFormat::k_8_8:
      return DXGI_FORMAT_R8G8_UNORM;
    case xenos::TextureFormat::k_16_16_16_16_FLOAT:
    case xenos::TextureFormat::k_16_16_16_16_EXPAND:
      return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case xenos::TextureFormat::k_32_FLOAT:
      return DXGI_FORMAT_R32_FLOAT;
    case xenos::TextureFormat::k_32_32_32_32_FLOAT:
      return DXGI_FORMAT_R32G32B32A32_FLOAT;
    case xenos::TextureFormat::k_24_8:
    case xenos::TextureFormat::k_24_8_FLOAT:
      return DXGI_FORMAT_R32_FLOAT;  // depth resolve destinations
    case xenos::TextureFormat::k_5_6_5:
      return DXGI_FORMAT_B5G6R5_UNORM;
    case xenos::TextureFormat::k_4_4_4_4:
      return DXGI_FORMAT_B4G4R4A4_UNORM;
    case xenos::TextureFormat::k_1_5_5_5:
      return DXGI_FORMAT_B5G5R5A1_UNORM;
    default:
      return DXGI_FORMAT_UNKNOWN;
  }
}

// Xbox 360 D3DDECLTYPE dwords (GPU fetch encodings; UnleashedRecomp video.h) -> small enum.
uint8_t DecodeDeclType(uint32_t t) {
  switch (t) {
    case 0x2C83A4: return 0;   // FLOAT1
    case 0x2C23A5: return 1;   // FLOAT2
    case 0x2A23B9: return 2;   // FLOAT3
    case 0x1A23A6: return 3;   // FLOAT4
    case 0x182886: return 4;   // D3DCOLOR
    case 0x1A2286: case 0x1A2386: return 5;   // UBYTE4
    case 0x2C2359: return 6;   // SHORT2
    case 0x1A235A: return 7;   // SHORT4
    case 0x1A2086: case 0x1A2186: return 8;   // UBYTE4N
    case 0x2C2159: return 9;   // SHORT2N
    case 0x1A215A: return 10;  // SHORT4N
    case 0x2C2059: return 11;  // USHORT2N
    case 0x1A205A: return 12;  // USHORT4N
    case 0x2C235F: return 15;  // FLOAT16_2
    case 0x1A2360: return 16;  // FLOAT16_4
    default: return 17;
  }
}
enum DeclType : uint8_t {
  kFloat1 = 0, kFloat2 = 1, kFloat3 = 2, kFloat4 = 3, kD3dColor = 4, kUByte4 = 5, kShort2 = 6, kShort4 = 7,
  kUByte4N = 8, kShort2N = 9, kShort4N = 10, kUShort2N = 11, kUShort4N = 12, kUDec3 = 13, kDec3N = 14,
  kFloat16_2 = 15, kFloat16_4 = 16, kUnused = 17,
};
enum DeclUsage : uint8_t {
  kPosition = 0, kBlendWeight = 1, kBlendIndices = 2, kNormal = 3, kPSize = 4, kTexCoord = 5, kTangent = 6,
  kBinormal = 7, kTessFactor = 8, kPositionT = 9, kColor = 10, kFog = 11, kDepth = 12, kSample = 13,
};
const char* kUsageSemantics[] = {"POSITION", "BLENDWEIGHT", "BLENDINDICES", "NORMAL", "PSIZE", "TEXCOORD", "TANGENT",
                                 "BINORMAL", "TESSFACTOR", "POSITIONT", "COLOR", "FOG", "DEPTH", "SAMPLE"};

DXGI_FORMAT MapDeclType(uint8_t type, uint8_t usage) {
  // Plain D3D9 semantics, which is what the DP1 ucode expects: D3DCOLOR is
  // normalized and delivered as (R,G,B,A) of the big-endian ARGB dword, i.e.
  // bytes (1,2,3,0) of the guest dword = B8G8R8A8 over the 8-in-32 swapped
  // host bytes. Both skinning families rely on it: the constant-palette VS
  // applies D3DCOLORtoUBYTE4 (.zyxw * 255) to get back to memory order = the
  // UBYTE4 weight lanes, the vertex-texture VS pads its indices for that
  // order. R8G8B8A8 paired a bone index with another lane's weight: spikes
  // on every skinned mesh (edgecheck.py, 785/2260 stretched triangles vs 0).
  // UBYTE4 is integer (bone weights in percent, 0..100; the bone matrices
  // carry the 0.01). The recompiled inputs are float4 BLENDINDICES /
  // uint4 BLENDWEIGHT.
  const bool uint_lanes = usage == kNormal || usage == kTangent || usage == kBinormal || usage == kBlendWeight;
  if (usage == kBlendIndices || usage == kBlendWeight) {
    if (type == kD3dColor) return usage == kBlendWeight ? DXGI_FORMAT_R8G8B8A8_UINT : DXGI_FORMAT_B8G8R8A8_UNORM;
    if (type == kUByte4N) return usage == kBlendWeight ? DXGI_FORMAT_R8G8B8A8_UINT : DXGI_FORMAT_R8G8B8A8_UNORM;
    if (type == kUByte4) return usage == kBlendIndices ? DXGI_FORMAT_R8G8B8A8_UNORM : DXGI_FORMAT_R8G8B8A8_UINT;
  }
  switch (type) {
    case kFloat1: return uint_lanes ? DXGI_FORMAT_R32_UINT : DXGI_FORMAT_R32_FLOAT;
    case kFloat2: return uint_lanes ? DXGI_FORMAT_R32G32_UINT : DXGI_FORMAT_R32G32_FLOAT;
    case kFloat3: return uint_lanes ? DXGI_FORMAT_R32G32B32_UINT : DXGI_FORMAT_R32G32B32_FLOAT;
    case kFloat4: return uint_lanes ? DXGI_FORMAT_R32G32B32A32_UINT : DXGI_FORMAT_R32G32B32A32_FLOAT;
    case kD3dColor: return uint_lanes ? DXGI_FORMAT_R8G8B8A8_UINT : DXGI_FORMAT_B8G8R8A8_UNORM;
    case kUByte4: return DXGI_FORMAT_R8G8B8A8_UINT;
    case kUByte4N: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case kShort2: return DXGI_FORMAT_R16G16_SINT;
    case kShort4: return DXGI_FORMAT_R16G16B16A16_SINT;
    case kShort2N: return DXGI_FORMAT_R16G16_SNORM;
    case kShort4N: return DXGI_FORMAT_R16G16B16A16_SNORM;
    case kUShort2N: return DXGI_FORMAT_R16G16_UNORM;
    case kUShort4N: return DXGI_FORMAT_R16G16B16A16_UNORM;
    case kFloat16_2: return uint_lanes ? DXGI_FORMAT_R16G16_UINT : DXGI_FORMAT_R16G16_FLOAT;
    case kFloat16_4: return uint_lanes ? DXGI_FORMAT_R16G16B16A16_UINT : DXGI_FORMAT_R16G16B16A16_FLOAT;
    default: return DXGI_FORMAT_UNKNOWN;
  }
}

}  // namespace

// ===========================================================================
// Resource creation on the host
// ===========================================================================
namespace {

bool EnsureSurfaceResource(GuestSurface& s) {
  if (s.resource) return true;
  s.scale = ScaleFor(s.width, s.height);  // [NEW FABLE VERSION]
  D3D12_RESOURCE_DESC desc = {};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  desc.Width = s.hw();
  desc.Height = s.hh();
  desc.DepthOrArraySize = 1;
  desc.MipLevels = 1;
  // [NEW FABLE VERSION] depth surfaces: s.format is the DSV format (D24_UNORM_S8 for the
  // console's D24S8, D32_FLOAT for D24FS8); the resource is the matching typeless format.
  const bool d24 = s.depth && s.format == DXGI_FORMAT_D24_UNORM_S8_UINT;
  desc.Format = s.depth ? (d24 ? DXGI_FORMAT_R24G8_TYPELESS : DXGI_FORMAT_R32_TYPELESS) : s.format;
  desc.SampleDesc.Count = 1;
  desc.Flags = s.depth ? D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL : D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
  D3D12_CLEAR_VALUE clear = {};
  clear.Format = s.format;
  if (s.depth) clear.DepthStencil.Depth = 1.0f;
  const D3D12_RESOURCE_STATES initial = s.depth ? D3D12_RESOURCE_STATE_DEPTH_WRITE : D3D12_RESOURCE_STATE_RENDER_TARGET;
  if (FAILED(g.device->CreateCommittedResource(&rex::ui::d3d12::util::kHeapPropertiesDefault, D3D12_HEAP_FLAG_NONE, &desc,
                                               initial, &clear, IID_PPV_ARGS(&s.resource)))) {
    REXLOG_ERROR("Native renderer: surface {:08X} {}x{} (host {}x{}) format {:08X} host creation failed", s.guest, s.width,
                 s.height, s.hw(), s.hh(), s.guest_format);
    // [NEW FABLE VERSION] out of video memory at this scale: fall back to 1x
    // rather than losing the target (a missing surface is a black frame).
    if (s.scale == 1) return false;
    REXLOG_WARN("Native renderer: retrying surface {:08X} at 1x (dp_native_scale {} did not fit)", s.guest, s.scale);
    s.scale = 1;
    desc.Width = s.width;
    desc.Height = s.height;
    if (FAILED(g.device->CreateCommittedResource(&rex::ui::d3d12::util::kHeapPropertiesDefault, D3D12_HEAP_FLAG_NONE,
                                                 &desc, initial, &clear, IID_PPV_ARGS(&s.resource)))) {
      return false;
    }
  }
  s.state = initial;
  NameResource(s.resource.Get(), fmt::format("surface {:08X} {}x{} x{} fmt {:08X}", s.guest, s.width, s.height, s.scale,
                                             s.guest_format));  // [new_fix_24092026]
  // [new_fix_24092026] 2.0.3: both views or neither (a full heap is logged, not
  // written past; the resource just created goes through the retire queue).
  DescriptorAllocator& target_views = s.depth ? g.dsvs : g.rtvs;
  s.view_index = target_views.Allocate();
  s.srv_index = g.views.Allocate();
  if (s.view_index == UINT32_MAX || s.srv_index == UINT32_MAX) {
    HeapFull(s.view_index == UINT32_MAX ? (s.depth ? "depth stencil view" : "render target view") : "shader view");
    target_views.Free(s.view_index);
    g.views.Free(s.srv_index);
    s.view_index = s.srv_index = UINT32_MAX;
    RetireResource(std::move(s.resource));
    return false;
  }
  if (s.depth) {
    D3D12_DEPTH_STENCIL_VIEW_DESC dsv = {};
    dsv.Format = s.format;
    dsv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    g.device->CreateDepthStencilView(s.resource.Get(), &dsv, g.dsvs.Cpu(s.view_index));
    D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
    srv.Format = d24 ? DXGI_FORMAT_R24_UNORM_X8_TYPELESS : DXGI_FORMAT_R32_FLOAT;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels = 1;
    g.device->CreateShaderResourceView(s.resource.Get(), &srv, g.views.Cpu(s.srv_index));
  } else {
    g.device->CreateRenderTargetView(s.resource.Get(), nullptr, g.rtvs.Cpu(s.view_index));
    g.device->CreateShaderResourceView(s.resource.Get(), nullptr, g.views.Cpu(s.srv_index));
  }
  return true;
}

// Host component that holds each guest component for a CPU-uploaded format
// (rexglue-sdk d3d12/texture_cache.cpp host_formats_ table), 3 bits each.
uint32_t HostSwizzle(xenos::TextureFormat f) {
  using namespace xenos;
  switch (f) {
    case TextureFormat::k_8:
    case TextureFormat::k_32_FLOAT:
    case TextureFormat::k_24_8:
    case TextureFormat::k_24_8_FLOAT:
      return XE_GPU_TEXTURE_SWIZZLE_RRRR;
    case TextureFormat::k_8_8:
      return XE_GPU_TEXTURE_SWIZZLE_RGGG;
    case TextureFormat::k_5_6_5:  // read through B5G6R5 without the emulator's R/B-swapping load shader
      return XE_GPU_MAKE_TEXTURE_SWIZZLE(B, G, R, B);
    case TextureFormat::k_1_5_5_5:
    case TextureFormat::k_4_4_4_4:
      return XE_GPU_MAKE_TEXTURE_SWIZZLE(B, G, R, A);
    default:
      return XE_GPU_TEXTURE_SWIZZLE_RGBA;
  }
}

// D3D12 SRV component mapping = guest fetch swizzle composed with the host
// format swizzle (TextureCache::GuestToHostSwizzle). Resolve destinations are
// written by our blit in logical RGBA order, so they take the identity.
// Resolve destinations hold the blit's logical RGBA already (the guest byte
// order the swizzle would undo never exists on the host), so only the forced
// 0/1 components of the fetch swizzle apply: e.g. the luminance chain's R32F
// textures fetch as (X,1,1,1) and the tonemap reads .y = 1.
uint32_t SrvMapping(const GuestTexture& t) {
  if (!t.info_valid) return D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  const uint32_t host = HostSwizzle(t.info.format);
  uint32_t comp[4];
  for (uint32_t i = 0; i < 4; ++i) {
    const uint32_t gc = (t.fetch.swizzle >> (3 * i)) & 7;
    if (gc >= xenos::XE_GPU_TEXTURE_SWIZZLE_0) {
      comp[i] = (gc & 1) ? D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_1 : D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_0;
    } else if (t.is_resolve_target) {
      comp[i] = D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_0 + i;
    } else {
      comp[i] = D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_0 + ((host >> (3 * gc)) & 7);
    }
  }
  return D3D12_ENCODE_SHADER_4_COMPONENT_MAPPING(comp[0], comp[1], comp[2], comp[3]);
}

bool EnsureTextureResource(GuestTexture& t) {
  if (t.resource) return true;
  if (!t.info_valid || t.format == DXGI_FORMAT_UNKNOWN || !t.width || !t.height) return false;
  // [new_fix_24092026] 2.0.3: the view slot first (a full heap: logged, nothing
  // created, no descriptor written past the heap).
  const uint32_t srv_index = g.views.Allocate();
  if (srv_index == UINT32_MAX) {
    HeapFull("shader view");
    return false;
  }
  // [NEW FABLE VERSION] only resolve destinations follow the resolution scale
  // (a CPU-uploaded texture has to keep the guest's size and tiling), and they
  // hold exactly what the blit writes: one level, no mip chain.
  if (t.is_resolve_target) {
    t.scale = ScaleFor(t.width, t.height);
    t.mip_levels = 1;
  } else {
    t.scale = 1;
  }
  D3D12_RESOURCE_DESC desc = {};
  desc.Dimension = t.is_3d ? D3D12_RESOURCE_DIMENSION_TEXTURE3D : D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  desc.Width = t.hw();
  desc.Height = t.hh();
  desc.DepthOrArraySize = uint16_t(t.is_cube ? 6 : std::max(1u, t.depth));
  desc.MipLevels = uint16_t(std::max(1u, t.mip_levels));  // [NEW FABLE VERSION] full mip chain for 2D/cube
  desc.Format = t.format;
  desc.SampleDesc.Count = 1;
  // Resolve destinations are rendered into by the blit pass.
  const bool renderable = t.format != DXGI_FORMAT_BC1_UNORM && t.format != DXGI_FORMAT_BC2_UNORM &&
                          t.format != DXGI_FORMAT_BC3_UNORM;
  if (renderable) desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
  if (FAILED(g.device->CreateCommittedResource(&rex::ui::d3d12::util::kHeapPropertiesDefault, D3D12_HEAP_FLAG_NONE, &desc,
                                               D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&t.resource)))) {
    REXLOG_ERROR("Native renderer: texture {:08X} {}x{} (host {}x{}) format {} host creation failed", t.guest, t.width,
                 t.height, t.hw(), t.hh(), uint32_t(t.format));
    // [NEW FABLE VERSION] same 1x fallback as the surfaces.
    if (t.scale == 1) {
      g.views.Free(srv_index);  // [new_fix_24092026] 2.0.3: never written
      return false;
    }
    REXLOG_WARN("Native renderer: retrying texture {:08X} at 1x (dp_native_scale {} did not fit)", t.guest, t.scale);
    t.scale = 1;
    desc.Width = t.width;
    desc.Height = t.height;
    if (FAILED(g.device->CreateCommittedResource(&rex::ui::d3d12::util::kHeapPropertiesDefault, D3D12_HEAP_FLAG_NONE,
                                                 &desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                 IID_PPV_ARGS(&t.resource)))) {
      g.views.Free(srv_index);  // [new_fix_24092026] 2.0.3: never written
      return false;
    }
  }
  t.state = D3D12_RESOURCE_STATE_COPY_DEST;
  NameResource(t.resource.Get(), fmt::format("texture {:08X} {}x{} x{} dxgi {}{}", t.guest, t.width, t.height, t.scale,
                                             uint32_t(t.format), t.is_resolve_target ? " resolve" : ""));  // [new_fix_24092026]
  t.srv_index = srv_index;  // [new_fix_24092026] 2.0.3: allocated above
  D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
  srv.Format = t.format;
  srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  if (t.is_cube) {
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
    srv.TextureCube.MipLevels = std::max(1u, t.mip_levels);
  } else if (t.is_3d) {
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
    srv.Texture3D.MipLevels = 1;
  } else if (t.depth > 1) {
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
    srv.Texture2DArray.MipLevels = 1;
    srv.Texture2DArray.ArraySize = t.depth;
  } else {
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Texture2D.MipLevels = std::max(1u, t.mip_levels);
  }
  srv.Shader4ComponentMapping = SrvMapping(t);
  g.device->CreateShaderResourceView(t.resource.Get(), &srv, g.views.Cpu(t.srv_index));
  // [new_fix_24092026] 2.0.3: no RTV here any more. Every renderable texture got
  // one for good (textures are never destroyed, the heap holds 512); only
  // resolve destinations need it, EnsureTextureRtv makes it on the first resolve.
  return true;
}

// [new_fix_24092026] 2.0.3: the whole-texture RTV of a 2D resolve destination.
bool EnsureTextureRtv(GuestTexture& t) {
  if (t.rtv_index != UINT32_MAX) return true;
  if (!t.resource || t.is_cube || t.depth > 1 || t.format == DXGI_FORMAT_BC1_UNORM ||
      t.format == DXGI_FORMAT_BC2_UNORM || t.format == DXGI_FORMAT_BC3_UNORM) {
    return false;
  }
  const uint32_t index = g.rtvs.Allocate();
  if (index == UINT32_MAX) {
    HeapFull("render target view");
    return false;
  }
  g.device->CreateRenderTargetView(t.resource.Get(), nullptr, g.rtvs.Cpu(index));
  t.rtv_index = index;
  return true;
}

// Untiles / byte-swaps mip 0 of a guest texture into the upload ring and copies it.
// ---------------------------------------------------------------------------
// Page watch (guest memory write protection, the Xenia/ReXGlue mechanism)
// ---------------------------------------------------------------------------
// The callback runs on the faulting thread with the memory system's global
// critical region held, so it takes only g_watch_mutex, and g_watch_mutex is
// never held while calling into the memory system (which takes that region).
struct TextureWatch {
  uint32_t phys = 0, len = 0;
  GuestTexture* texture = nullptr;
  GuestBuffer* buffer = nullptr;  // buffer watch (2026-09-20): exactly one of texture/buffer is set
};
std::mutex g_watch_mutex;
std::vector<TextureWatch> g_watches;
void* g_watch_handle = nullptr;
std::atomic<uint32_t> g_watch_hits{0};

std::pair<uint32_t, uint32_t> WatchCallback(void*, uint32_t phys_start, uint32_t length, bool /*exact_range*/) {
  std::lock_guard<std::mutex> lock(g_watch_mutex);
  // Everything the write touched is invalid; the memory system then unprotects
  // the extent we return, so every watch inside that extent must go too (the
  // pages would otherwise stay unprotected while the texture believes it is
  // watched, and a later write would be missed).
  uint32_t first = phys_start, last = phys_start + length;
  for (const TextureWatch& w : g_watches) {
    if (w.phys < phys_start + length && phys_start < w.phys + w.len) {
      first = std::min(first, w.phys);
      last = std::max(last, w.phys + w.len);
    }
  }
  first &= ~0xFFFu;
  last = (last + 0xFFFu) & ~0xFFFu;
  for (size_t i = 0; i < g_watches.size();) {
    TextureWatch& w = g_watches[i];
    if (w.phys < last && first < w.phys + w.len) {
      if (w.texture) w.texture->written.store(true, std::memory_order_release);
      if (w.buffer) w.buffer->written.store(true, std::memory_order_release);
      g_watch_hits.fetch_add(1, std::memory_order_relaxed);
      w = g_watches.back();
      g_watches.pop_back();
    } else {
      ++i;
    }
  }
  return {first, last - first};
}

void ForgetWatch(GuestTexture* t) {
  std::lock_guard<std::mutex> lock(g_watch_mutex);
  for (size_t i = 0; i < g_watches.size();) {
    if (g_watches[i].texture == t) {
      g_watches[i] = g_watches.back();
      g_watches.pop_back();
    } else {
      ++i;
    }
  }
}

void ForgetWatch(GuestBuffer* b) {
  std::lock_guard<std::mutex> lock(g_watch_mutex);
  for (size_t i = 0; i < g_watches.size();) {
    if (g_watches[i].buffer == b) {
      g_watches[i] = g_watches.back();
      g_watches.pop_back();
    } else {
      ++i;
    }
  }
}

// Called right after a buffer upload: protect its pages so the next CPU write
// (the streaming pools are written without Lock, mid-frame) marks it written.
void WatchBuffer(GuestBuffer& b) {
  b.watched = false;
  if (!g_watch_handle || !b.size || !WatchableAddress(b.address)) return;
  const uint32_t phys = GuestPhysical(b.address);
  ForgetWatch(&b);
  b.written.store(false, std::memory_order_relaxed);
  {
    std::lock_guard<std::mutex> lock(g_watch_mutex);
    g_watches.push_back({phys, b.size, nullptr, &b});
  }
  Mem()->EnablePhysicalMemoryAccessCallbacks(phys, b.size, true, false);
  b.watched = true;
}

// Called right after the upload: the guest memory now matches the resource.
void WatchTexture(GuestTexture& t) {
  t.watched = false;
  if (!g_watch_handle || !t.info_valid || !t.info.memory.base_size) return;
  const uint32_t base = t.info.memory.base_address;
  if (!WatchableAddress(base)) return;
  const uint32_t phys = GuestPhysical(base);
  uint32_t len = t.info.memory.base_size;
  if (t.info.memory.mip_size && WatchableAddress(t.info.memory.mip_address)) {
    const uint32_t mip = GuestPhysical(t.info.memory.mip_address);
    // one range when the mips follow the base level, else the base only
    if (mip >= phys && mip <= phys + len) len = mip + t.info.memory.mip_size - phys;
  }
  // Table entry first, protection second: a write in between only costs a
  // spurious re-upload, the other order would lose the write.
  ForgetWatch(&t);
  t.written.store(false, std::memory_order_relaxed);
  {
    std::lock_guard<std::mutex> lock(g_watch_mutex);
    g_watches.push_back({phys, len, &t});
  }
  Mem()->EnablePhysicalMemoryAccessCallbacks(phys, len, true, false);
  t.watched = true;
}

// [NEW FABLE VERSION] 2026-09-23: texture replacement. The emulated path swaps
// guest textures for textures\<hash>.png / <hash>.overlay.png in the SDK's GPU
// plugin (the launcher's keyboard key caps over the button prompt atlas); the
// native renderer does not load that plugin, so the prompts showed the pad
// buttons. Same hash, same files, same overlay rules: see native_texrep.h.
void DropHostTexture(GuestTexture& t) {
  ForgetWatch(&t);
  RetireDescriptor(g.views, t.srv_index);  // [new_fix_24092026] 2.0.3: after the GPU, like the resource
  g.rtvs.Free(t.rtv_index);                 // CPU-only heap: read when a command is recorded
  for (uint32_t r : t.slice_rtvs) g.rtvs.Free(r);
  t.slice_rtvs.clear();
  t.srv_index = UINT32_MAX;
  t.rtv_index = UINT32_MAX;
  if (t.resource) RetireResource(std::move(t.resource));
  t.resource.Reset();
  t.state = D3D12_RESOURCE_STATE_COMMON;
  t.watched = false;
  t.replaced = false;
  t.replaced_hash = 0;
}

// Guest base level as RGBA8 (for overlays): untiled and endian swapped the way
// UploadTexture does it, then decoded on the CPU.
bool DecodeGuestBaseRgba(const GuestTexture& t, std::vector<uint8_t>& rgba, uint32_t& width, uint32_t& height) {
  using xenos::TextureFormat;
  texrep::Codec codec;
  switch (t.info.format) {
    case TextureFormat::k_8_8_8_8: codec = texrep::Codec::k8888; break;
    case TextureFormat::k_8: codec = texrep::Codec::k8; break;
    case TextureFormat::k_DXT1: codec = texrep::Codec::kDXT1; break;
    case TextureFormat::k_DXT2_3: codec = texrep::Codec::kDXT2_3; break;
    case TextureFormat::k_DXT4_5: codec = texrep::Codec::kDXT4_5; break;
    case TextureFormat::k_DXT5A: codec = texrep::Codec::kDXT5A; break;
    default: return false;
  }
  const rex::graphics::FormatInfo* fi = t.info.format_info();
  if (!fi) return false;
  const rex::graphics::TextureExtent ge = t.info.GetMipExtent(0, true);
  const uint32_t bpb = fi->bytes_per_block();
  const size_t row_pitch = size_t(ge.block_width) * bpb;
  std::vector<uint8_t> linear(row_pitch * ge.block_height);
  const uint8_t* src = HeaderPtr(t.info.memory.base_address);
  if (t.info.is_tiled) {
    rex::graphics::texture_conversion::UntileInfo ui = {};
    ui.width = ge.block_width;
    ui.height = ge.block_height;
    ui.input_pitch = ge.block_pitch_h;
    ui.output_pitch = ge.block_width;
    ui.input_format_info = fi;
    ui.output_format_info = fi;
    const xenos::Endian endian = t.info.endianness;
    ui.copy_callback = [endian](void* o, const void* i, size_t len) {
      rex::graphics::texture_conversion::CopySwapBlock(endian, o, i, len);
    };
    rex::graphics::texture_conversion::Untile(linear.data(), src, &ui);
  } else {
    for (uint32_t y = 0; y < ge.block_height; ++y) {
      rex::graphics::texture_conversion::CopySwapBlock(t.info.endianness, linear.data() + y * row_pitch,
                                                        src + size_t(y) * ge.block_pitch_h * bpb, row_pitch);
    }
  }
  width = t.width;
  height = t.height;
  return texrep::DecodeBase(codec, linear.data(), row_pitch, width, height, rgba);
}

// True when the texture is (now) served from a replacement PNG.
bool UploadReplacement(GuestTexture& t) {
  if (!t.info_valid || t.is_cube || t.is_3d || t.depth > 1 || t.is_resolve_target) return false;
  if (!texrep::Active()) return false;
  const xenos::xe_gpu_texture_fetch_t& fetch = t.fetch;
  if (fetch.dimension != xenos::DataDimension::k2DOrStacked) return false;
  // Exactly the SDK's key: TextureCache::BindingInfoFromFetchConstant +
  // TextureKey::GetGuestLayout + HashGuestTexture.
  uint32_t wm1 = 0, hm1 = 0, dm1 = 0, base_page = 0, mip_page = 0, mip_max = 0;
  rex::graphics::texture_util::GetSubresourcesFromFetchConstant(fetch, &wm1, &hm1, &dm1, &base_page, &mip_page,
                                                                nullptr, &mip_max);
  if (!base_page || dm1 != 0) return false;
  const xenos::TextureFormat format = rex::graphics::GetBaseFormat(fetch.format);
  const rex::graphics::texture_util::TextureGuestLayout layout = rex::graphics::texture_util::GetGuestTextureLayout(
      fetch.dimension, fetch.pitch, wm1 + 1, hm1 + 1, 1, fetch.tiled != 0, format, fetch.packed_mips != 0, true,
      mip_max);
  if (layout.packed_level == 0 || !layout.base.level_data_extent_bytes) return false;
  const uint64_t hash = texrep::Hash(HeaderPtr(t.info.memory.base_address), layout.base.level_data_extent_bytes,
                                     wm1 + 1, hm1 + 1, uint32_t(format));
  const texrep::Image* image = texrep::Find(hash, [&t](std::vector<uint8_t>& rgba, uint32_t& w, uint32_t& h) {
    return DecodeGuestBaseRgba(t, rgba, w, h);
  });
  if (!image) return false;
  if (t.replaced && t.replaced_hash == hash && t.resource) {
    t.dirty = false;  // same content as the PNG already on the GPU
    WatchTexture(t);
    return true;
  }
  if (t.resource) DropHostTexture(t);
  const uint32_t mips = std::max(1u, std::min<uint32_t>(uint32_t(image->levels.size()), mip_max + 1));
  D3D12_RESOURCE_DESC desc = {};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  desc.Width = image->width;
  desc.Height = image->height;
  desc.DepthOrArraySize = 1;
  desc.MipLevels = uint16_t(mips);
  desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  desc.SampleDesc.Count = 1;
  if (FAILED(g.device->CreateCommittedResource(&rex::ui::d3d12::util::kHeapPropertiesDefault, D3D12_HEAP_FLAG_NONE,
                                               &desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                               IID_PPV_ARGS(&t.resource)))) {
    REXLOG_ERROR("Native renderer: replacement {:016X} ({}x{}) host creation failed", hash, image->width,
                 image->height);
    t.resource.Reset();
    return false;
  }
  NameResource(t.resource.Get(), fmt::format("replacement {:016X} for texture {:08X}", hash, t.guest));  // [new_fix_24092026] 2.0.3
  t.state = D3D12_RESOURCE_STATE_COPY_DEST;
  t.scale = 1;
  UploadRing& ring = g.upload[g.frame_index];
  for (uint32_t level = 0; level < mips; ++level) {
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp = {};
    UINT rows = 0;
    UINT64 row_bytes = 0, total = 0;
    g.device->GetCopyableFootprints(&desc, level, 1, 0, &fp, &rows, &row_bytes, &total);
    const size_t off = ring.Allocate(size_t(total), 512);
    if (off == SIZE_MAX) {
      REXLOG_WARN("Native renderer: upload ring full while uploading replacement {:016X}", hash);
      RetireResource(std::move(t.resource));
      t.resource.Reset();
      t.state = D3D12_RESOURCE_STATE_COMMON;
      return false;
    }
    const uint32_t lw = image->LevelWidth(level), lh = image->LevelHeight(level);
    const std::vector<uint8_t>& src = image->levels[level];
    for (uint32_t y = 0; y < lh; ++y) {
      std::memcpy(ring.mapped + off + size_t(y) * fp.Footprint.RowPitch, src.data() + size_t(y) * lw * 4,
                  size_t(lw) * 4);
    }
    fp.Offset = off;
    D3D12_TEXTURE_COPY_LOCATION dl = {}, sl = {};
    dl.pResource = t.resource.Get();
    dl.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dl.SubresourceIndex = level;
    sl.pResource = ring.buffer.Get();
    sl.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    sl.PlacedFootprint = fp;
    g.list->CopyTextureRegion(&dl, 0, 0, 0, &sl, nullptr);
    g.stats.upload_bytes += total;
  }
  t.srv_index = g.views.Allocate();
  if (t.srv_index == UINT32_MAX) {  // [new_fix_24092026] 2.0.3
    HeapFull("shader view");
    RetireResource(std::move(t.resource));
    t.state = D3D12_RESOURCE_STATE_COMMON;
    return false;
  }
  D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
  srv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
  srv.Texture2D.MipLevels = mips;
  // The PNG holds decoded RGBA in logical order, like a DXT texture on the
  // host, so the guest's fetch swizzle applies unchanged.
  srv.Shader4ComponentMapping = SrvMapping(t);
  g.device->CreateShaderResourceView(t.resource.Get(), &srv, g.views.Cpu(t.srv_index));
  Barrier(t.resource.Get(), t.state,
          D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  t.replaced = true;
  t.replaced_hash = hash;
  t.dirty = false;
  g.stats.uploads_tex++;
  WatchTexture(t);
  return true;
}

bool UploadTexture(GuestTexture& t) {
  // [NEW FABLE VERSION] a replacement PNG wins; a texture that had one but now
  // holds other guest data goes back to the guest path with a fresh resource.
  if (UploadReplacement(t)) return true;
  if (t.replaced) DropHostTexture(t);
  if (!EnsureTextureResource(t)) return false;
  const rex::graphics::TextureInfo& info = t.info;
  const rex::graphics::FormatInfo* fi = info.format_info();
  if (!fi) return false;
  const uint32_t faces = t.is_cube ? 6 : 1;
  const rex::graphics::TextureExtent guest_extent = info.GetMipExtent(0, true);
  const rex::graphics::TextureExtent host_extent = info.GetMipExtent(0, false);
  const uint32_t bytes_per_block = fi->bytes_per_block();
  const uint32_t host_row_bytes = host_extent.block_pitch_h * bytes_per_block;
  const uint32_t host_pitch = (host_row_bytes + 255) & ~255u;
  const uint32_t guest_face_bytes = guest_extent.block_pitch_h * guest_extent.block_pitch_v * bytes_per_block;
  const uint32_t depth = t.is_3d ? std::max(1u, t.depth) : 1;
  const size_t face_bytes = size_t(host_pitch) * host_extent.block_height * depth;
  UploadRing& ring = g.upload[g.frame_index];
  const size_t off = ring.Allocate(face_bytes * faces, 512);
  if (off == SIZE_MAX) {
    REXLOG_WARN("Native renderer: upload ring full while uploading texture {:08X}", t.guest);
    return false;
  }
  if (t.is_3d) {
    // Volume: every slice, block by block, through the 3D tiled address (32x32x4
    // tiles) or the linear slice layout; host slices are host_pitch * height apart.
    const uint8_t* src3 = HeaderPtr(info.memory.base_address);
    uint8_t* dst3 = ring.mapped + off;
    const size_t host_slice = size_t(host_pitch) * host_extent.block_height;
    const uint32_t bpb_log2 = uint32_t(std::log2(bytes_per_block));
    const size_t guest_slice = size_t(guest_extent.block_pitch_h) * guest_extent.block_pitch_v * bytes_per_block;
    for (uint32_t z = 0; z < depth; ++z) {
      if (t.is_stacked_3d) {
        uint8_t* dst_slice = dst3 + z * host_slice;
        const uint8_t* src_slice = src3 + z * guest_slice;
        if (info.is_tiled) {
          rex::graphics::texture_conversion::UntileInfo ui = {};
          ui.width = guest_extent.block_width;
          ui.height = guest_extent.block_height;
          ui.input_pitch = guest_extent.block_pitch_h;
          ui.output_pitch = host_pitch / bytes_per_block;
          ui.input_format_info = fi;
          ui.output_format_info = fi;
          const xenos::Endian endian = info.endianness;
          ui.copy_callback = [endian](void* o, const void* i, size_t len) {
            rex::graphics::texture_conversion::CopySwapBlock(endian, o, i, len);
          };
          rex::graphics::texture_conversion::Untile(dst_slice, src_slice, &ui);
        } else {
          for (uint32_t y = 0; y < guest_extent.block_height; ++y) {
            rex::graphics::texture_conversion::CopySwapBlock(info.endianness, dst_slice + size_t(y) * host_pitch,
                                                              src_slice + size_t(y) * guest_extent.block_pitch_h * bytes_per_block, host_row_bytes);
          }
        }
        continue;
      }
      for (uint32_t y = 0; y < guest_extent.block_height; ++y) {
        uint8_t* row = dst3 + z * host_slice + size_t(y) * host_pitch;
        for (uint32_t x = 0; x < guest_extent.block_width; ++x) {
          const size_t so = info.is_tiled
              ? size_t(rex::graphics::texture_util::GetTiledOffset3D(int32_t(x), int32_t(y), int32_t(z), guest_extent.block_pitch_h,
                                                                     guest_extent.block_pitch_v, bpb_log2))
              : (size_t(z) * guest_extent.block_pitch_v + y) * guest_extent.block_pitch_h * bytes_per_block + size_t(x) * bytes_per_block;
          rex::graphics::texture_conversion::CopySwapBlock(info.endianness, row + size_t(x) * bytes_per_block, src3 + so, bytes_per_block);
        }
      }
    }
    Barrier(t.resource.Get(), t.state, D3D12_RESOURCE_STATE_COPY_DEST);
    D3D12_TEXTURE_COPY_LOCATION dl = {}, sl = {};
    dl.pResource = t.resource.Get();
    dl.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dl.SubresourceIndex = 0;
    sl.pResource = ring.buffer.Get();
    sl.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    sl.PlacedFootprint.Offset = off;
    sl.PlacedFootprint.Footprint.Format = t.format;
    sl.PlacedFootprint.Footprint.Width = std::max(t.width, uint32_t(fi->block_width));
    sl.PlacedFootprint.Footprint.Height = std::max(t.height, uint32_t(fi->block_height));
    sl.PlacedFootprint.Footprint.Depth = depth;
    sl.PlacedFootprint.Footprint.RowPitch = host_pitch;
    g.list->CopyTextureRegion(&dl, 0, 0, 0, &sl, nullptr);
    Barrier(t.resource.Get(), t.state,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    t.dirty = false;
    g.stats.uploads_tex++;
    WatchTexture(t);
    return true;
  }
  // The header's fetch constant still holds the guest virtual base (the XDK
  // converts it to physical only when it copies the constant into the device
  // shadow); the 0xE0000000 heap shifts by 4 KB = the movie planes' "256-byte
  // row rotation" and the font atlas' wrong glyphs.
  const uint8_t* src_base = HeaderPtr(info.memory.base_address);
  Barrier(t.resource.Get(), t.state, D3D12_RESOURCE_STATE_COPY_DEST);
  for (uint32_t face = 0; face < faces; ++face) {
    uint8_t* dst = ring.mapped + off + face * face_bytes;
    const uint8_t* src = src_base + face * guest_face_bytes;
    if (info.is_tiled) {
      rex::graphics::texture_conversion::UntileInfo ui = {};
      ui.offset_x = 0;
      ui.offset_y = 0;
      ui.width = guest_extent.block_width;
      ui.height = guest_extent.block_height;
      ui.input_pitch = guest_extent.block_pitch_h;
      ui.output_pitch = host_pitch / bytes_per_block;
      ui.input_format_info = fi;
      ui.output_format_info = fi;
      const xenos::Endian endian = info.endianness;
      ui.copy_callback = [endian](void* o, const void* i, size_t len) {
        rex::graphics::texture_conversion::CopySwapBlock(endian, o, i, len);
      };
      rex::graphics::texture_conversion::Untile(dst, src, &ui);
    } else {
      const uint32_t src_pitch = guest_extent.block_pitch_h * bytes_per_block;
      for (uint32_t y = 0; y < guest_extent.block_height; ++y) {
        rex::graphics::texture_conversion::CopySwapBlock(info.endianness, dst + size_t(y) * host_pitch,
                                                          src + size_t(y) * src_pitch, host_row_bytes);
      }
    }
    D3D12_TEXTURE_COPY_LOCATION dl = {}, sl = {};
    dl.pResource = t.resource.Get();
    dl.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dl.SubresourceIndex = face * std::max(1u, t.mip_levels);  // [NEW FABLE VERSION] mip 0 of this face (review fix)
    sl.pResource = ring.buffer.Get();
    sl.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    sl.PlacedFootprint.Offset = off + face * face_bytes;
    sl.PlacedFootprint.Footprint.Format = t.format;
    sl.PlacedFootprint.Footprint.Width = std::max(t.width, uint32_t(fi->block_width));
    sl.PlacedFootprint.Footprint.Height = std::max(t.height, uint32_t(fi->block_height));
    sl.PlacedFootprint.Footprint.Depth = 1;
    sl.PlacedFootprint.Footprint.RowPitch = host_pitch;
    g.list->CopyTextureRegion(&dl, 0, 0, 0, &sl, nullptr);
  }
  g.stats.upload_bytes += face_bytes * faces;
  // [NEW FABLE VERSION] mip levels 1..N. The guest location of each level comes
  // from TextureInfo (the packed tail levels share one 32x32 tile and carry a
  // block offset), the host layout from GetCopyableFootprints. Until now every
  // texture was sampled from its base level only: shimmer and moire at a
  // distance, and a sharper look than the console's trilinear filtering.
  if (t.mip_levels > 1) {
    const D3D12_RESOURCE_DESC rdesc = t.resource->GetDesc();
    bool ring_full = false;
    for (uint32_t mip = 1; mip < t.mip_levels && !ring_full; ++mip) {
      uint32_t ox = 0, oy = 0;
      const uint32_t mip_addr = info.GetMipLocation(mip, &ox, &oy, true);
      if (!mip_addr) break;
      const rex::graphics::TextureExtent ge = info.GetMipExtent(mip, true);
      const rex::graphics::TextureExtent he = info.GetMipExtent(mip, false);
      const uint32_t src_pitch = ge.block_pitch_h * bytes_per_block;
      const uint32_t guest_mip_face_bytes = ge.block_pitch_h * ge.block_pitch_v * bytes_per_block;
      const uint8_t* mip_base = HeaderPtr(mip_addr);
      for (uint32_t face = 0; face < faces; ++face) {
        const uint32_t sub = mip + face * t.mip_levels;
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp = {};
        UINT rows = 0;
        UINT64 row_bytes = 0, total = 0;
        g.device->GetCopyableFootprints(&rdesc, sub, 1, 0, &fp, &rows, &row_bytes, &total);
        if (!total || !rows) continue;
        const size_t moff = ring.Allocate(size_t(total), 512);
        if (moff == SIZE_MAX) {
          REXLOG_WARN("Native renderer: upload ring full while uploading mip {} of texture {:08X}", mip, t.guest);
          ring_full = true;
          break;
        }
        uint8_t* dst = ring.mapped + moff;
        const uint8_t* src = mip_base + size_t(face) * guest_mip_face_bytes;
        const uint32_t w_blocks = std::min(ge.block_width, he.block_width);
        const uint32_t h_blocks = std::min<uint32_t>(std::min(ge.block_height, he.block_height), rows);
        if (info.is_tiled) {
          rex::graphics::texture_conversion::UntileInfo ui = {};
          ui.offset_x = ox;
          ui.offset_y = oy;
          ui.width = w_blocks;
          ui.height = h_blocks;
          ui.input_pitch = ge.block_pitch_h;
          ui.output_pitch = fp.Footprint.RowPitch / bytes_per_block;
          ui.input_format_info = fi;
          ui.output_format_info = fi;
          const xenos::Endian endian = info.endianness;
          ui.copy_callback = [endian](void* o, const void* i, size_t len) {
            rex::graphics::texture_conversion::CopySwapBlock(endian, o, i, len);
          };
          rex::graphics::texture_conversion::Untile(dst, src, &ui);
        } else {
          src += size_t(oy) * src_pitch + size_t(ox) * bytes_per_block;
          for (uint32_t y = 0; y < h_blocks; ++y) {
            rex::graphics::texture_conversion::CopySwapBlock(info.endianness, dst + size_t(y) * fp.Footprint.RowPitch,
                                                              src + size_t(y) * src_pitch, size_t(w_blocks) * bytes_per_block);
          }
        }
        fp.Offset = moff;
        D3D12_TEXTURE_COPY_LOCATION dl = {}, sl = {};
        dl.pResource = t.resource.Get();
        dl.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dl.SubresourceIndex = sub;
        sl.pResource = ring.buffer.Get();
        sl.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        sl.PlacedFootprint = fp;
        g.list->CopyTextureRegion(&dl, 0, 0, 0, &sl, nullptr);
        g.stats.upload_bytes += total;
      }
    }
  }
  Barrier(t.resource.Get(), t.state,
          D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  t.dirty = false;
  g.stats.uploads_tex++;
  WatchTexture(t);
  return true;
}

// The game re-points its streaming buffers (FixBufferCopy threads rewrite the
// XDK header's fetch constant in place), so the address/size are re-read from
// the guest header at every use; a change means new content.
// Header layout (XDK, confirmed in the recompiled SetStreamSource sub_824CD6E0
// and DrawIndexedVertices sub_825CDBD8): +24 = guest virtual address (VB: with
// the fetch type in bits 0-1), +28 = size in bytes | endian (VB only; the IB's
// endian and 16/32-bit format live in Common (+0) bits 29-30 and 31).
void RefreshBufferHeader(GuestBuffer& b) {
  // [new_fix_24092026] 2.0.4 (issue #35): an index buffer's +28 is its size in
  // BYTES and +24 its plain address; 2.0.3 decoded both as a vertex fetch
  // constant and lost the last index of odd 16-bit index counts
  // (native_buffer_header.h).
  const BufferHeader h = DecodeBufferHeader(b.index, b.index ? LoadU32(b.guest) : 0u, LoadU32(b.guest + 24),
                                            LoadU32(b.guest + 28));
  const uint32_t address = h.address;
  const uint32_t size = h.size;
  b.endian = h.endian;
  if (b.index) b.index32 = h.index32;
  if (address != b.address || size != b.size) {
    if (b.watched) {
      ForgetWatch(&b);
      b.watched = false;
    }
    b.address = address;
    if (size != b.size) {
      RetireResource(std::move(b.resource));
      b.size = size;
    }
    b.dirty = true;
  }
}

bool UploadBuffer(GuestBuffer& b) {
  RefreshBufferHeader(b);
  if (!b.size) return false;
  if (!b.resource) {
    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = (b.size + 3) & ~3u;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (FAILED(g.device->CreateCommittedResource(&rex::ui::d3d12::util::kHeapPropertiesDefault, D3D12_HEAP_FLAG_NONE,
                                                 &desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                 IID_PPV_ARGS(&b.resource)))) {
      return false;
    }
    b.state = D3D12_RESOURCE_STATE_COPY_DEST;
    NameResource(b.resource.Get(), fmt::format("{} buffer {:08X} {} bytes", b.index ? "index" : "vertex", b.guest,
                                               b.size));  // [new_fix_24092026] 2.0.3: DRED names it
  }
  UploadRing& ring = g.upload[g.frame_index];
  const size_t bytes = (b.size + 3) & ~3u;
  const size_t off = ring.Allocate(bytes, 16);
  if (off == SIZE_MAX) {
    REXLOG_WARN("Native renderer: upload ring full while uploading buffer {:08X} ({} bytes)", b.guest, b.size);
    return false;
  }
  const uint8_t* src = HeaderPtr(b.address);
  uint8_t* dst = ring.mapped + off;
  if (b.index) {
    if (b.index32) rex::memory::copy_and_swap_32_unaligned(dst, src, bytes / 4);
    else rex::memory::copy_and_swap_16_unaligned(dst, src, bytes / 2);
  } else if (b.endian == 0) {
    std::memcpy(dst, src, bytes);
  } else if (b.endian == 1) {
    rex::memory::copy_and_swap_16_unaligned(dst, src, bytes / 2);
  } else {
    rex::memory::copy_and_swap_32_unaligned(dst, src, bytes / 4);
  }
  Barrier(b.resource.Get(), b.state, D3D12_RESOURCE_STATE_COPY_DEST);
  g.list->CopyBufferRegion(b.resource.Get(), 0, ring.buffer.Get(), off, bytes);
  Barrier(b.resource.Get(), b.state,
          b.index ? D3D12_RESOURCE_STATE_INDEX_BUFFER : D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
  b.dirty = false;
  g.stats.upload_bytes += bytes;
  WatchBuffer(b);
  if (b.index) g.stats.uploads_ib++; else g.stats.uploads_vb++;
  return true;
}

uint32_t SamplerIndex(const xenos::xe_gpu_texture_fetch_t& fetch) {
  // [NEW FABLE VERSION] the key also carries the mip range and the LOD bias (dword 4).
  const uint64_t key = (uint64_t(fetch.mag_filter) << 0) | (uint64_t(fetch.min_filter) << 2) |
                       (uint64_t(fetch.mip_filter) << 4) | (uint64_t(fetch.clamp_x) << 6) |
                       (uint64_t(fetch.clamp_y) << 9) | (uint64_t(fetch.clamp_z) << 12) |
                       (uint64_t(fetch.border_color) << 15) | (uint64_t(fetch.aniso_filter) << 17) |
                       (uint64_t(fetch.mip_min_level & 0xF) << 20) | (uint64_t(fetch.mip_max_level & 0xF) << 24) |
                       (uint64_t(uint32_t(fetch.lod_bias) & 0x3FF) << 28);
  auto it = g.sampler_indices.find(key);
  if (it != g.sampler_indices.end()) return it->second;
  const uint32_t index = g.samplers.Allocate();
  if (index == UINT32_MAX) return 0;
  auto address = [](xenos::ClampMode m) {
    switch (m) {
      case xenos::ClampMode::kRepeat: return D3D12_TEXTURE_ADDRESS_MODE_WRAP;
      case xenos::ClampMode::kMirroredRepeat: return D3D12_TEXTURE_ADDRESS_MODE_MIRROR;
      case xenos::ClampMode::kClampToBorder:
      case xenos::ClampMode::kMirrorClampToBorder: return D3D12_TEXTURE_ADDRESS_MODE_BORDER;
      default: return D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    }
  };
  const bool mag_linear = fetch.mag_filter != xenos::TextureFilter::kPoint;
  const bool min_linear = fetch.min_filter != xenos::TextureFilter::kPoint;
  const bool mip_linear = fetch.mip_filter == xenos::TextureFilter::kLinear;
  D3D12_SAMPLER_DESC d = {};
  d.Filter = D3D12_ENCODE_BASIC_FILTER(min_linear ? D3D12_FILTER_TYPE_LINEAR : D3D12_FILTER_TYPE_POINT,
                                       mag_linear ? D3D12_FILTER_TYPE_LINEAR : D3D12_FILTER_TYPE_POINT,
                                       mip_linear ? D3D12_FILTER_TYPE_LINEAR : D3D12_FILTER_TYPE_POINT,
                                       D3D12_FILTER_REDUCTION_TYPE_STANDARD);
  if (uint32_t(fetch.aniso_filter) >= 1 && uint32_t(fetch.aniso_filter) <= 5) {
    d.Filter = D3D12_FILTER_ANISOTROPIC;
    d.MaxAnisotropy = 1u << uint32_t(fetch.aniso_filter);
  }
  d.AddressU = address(fetch.clamp_x);
  d.AddressV = address(fetch.clamp_y);
  d.AddressW = address(fetch.clamp_z);
  const bool white_border = fetch.border_color != xenos::BorderColor::k_ABGR_Black;
  d.BorderColor[0] = d.BorderColor[1] = d.BorderColor[2] = d.BorderColor[3] = white_border ? 1.0f : 0.0f;
  // [NEW FABLE VERSION] mip range and LOD bias from the fetch constant (lod_bias
  // has 5 fractional bits); kBaseMap samples the base level only (no mipmapping).
  const bool base_only = fetch.mip_filter == xenos::TextureFilter::kBaseMap;
  d.MipLODBias = float(int32_t(fetch.lod_bias)) / 32.0f;
  d.MinLOD = float(fetch.mip_min_level);
  d.MaxLOD = base_only ? float(fetch.mip_min_level) : float(fetch.mip_max_level);
  d.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
  g.device->CreateSampler(&d, g.samplers.Cpu(index));
  g.sampler_indices[key] = index;
  return index;
}

}  // namespace

// ===========================================================================
// Current binding state (from the Set* hooks) and the draw path
// ===========================================================================
namespace {

struct BindState {
  uint32_t rt0 = 0, ds = 0;
  uint32_t textures[32] = {};
  struct Stream {
    uint32_t vb = 0, offset = 0, stride = 0;
  } streams[2];
  uint32_t ib = 0;
  uint32_t decl = 0, vs = 0, ps = 0;
  D3D12_VIEWPORT viewport = {0, 0, 1280, 720, 0, 1};
  bool viewport_valid = false;
  // Host-side "what is bound on the command list" mirror.
  // [new_fix_24092026] 2.0.3: the bound GuestSurface objects (not guest addresses:
  // an object re-created at the same address must be bound again).
  uintptr_t bound_rt0 = UINTPTR_MAX, bound_ds = UINTPTR_MAX;
  ID3D12PipelineState* bound_pso = nullptr;
  bool root_bound = false;  // [NEW FABLE VERSION] game root signature + bindless tables set on the list
};
BindState st;

D3D12_BLEND MapBlendFactor(uint32_t f) {
  switch (f) {
    case 0: return D3D12_BLEND_ZERO;
    case 1: return D3D12_BLEND_ONE;
    case 4: return D3D12_BLEND_SRC_COLOR;
    case 5: return D3D12_BLEND_INV_SRC_COLOR;
    case 6: return D3D12_BLEND_SRC_ALPHA;
    case 7: return D3D12_BLEND_INV_SRC_ALPHA;
    case 8: return D3D12_BLEND_DEST_COLOR;
    case 9: return D3D12_BLEND_INV_DEST_COLOR;
    case 10: return D3D12_BLEND_DEST_ALPHA;
    case 11: return D3D12_BLEND_INV_DEST_ALPHA;
    case 12: return D3D12_BLEND_BLEND_FACTOR;
    case 13: return D3D12_BLEND_INV_BLEND_FACTOR;
    case 14: return D3D12_BLEND_BLEND_FACTOR;      // constant alpha
    case 15: return D3D12_BLEND_INV_BLEND_FACTOR;
    case 16: return D3D12_BLEND_SRC_ALPHA_SAT;
    default: return D3D12_BLEND_ONE;
  }
}
D3D12_BLEND_OP MapBlendOp(uint32_t op) {
  switch (op) {
    case 0: return D3D12_BLEND_OP_ADD;
    case 1: return D3D12_BLEND_OP_SUBTRACT;
    case 2: return D3D12_BLEND_OP_MIN;
    case 3: return D3D12_BLEND_OP_MAX;
    case 4: return D3D12_BLEND_OP_REV_SUBTRACT;
    default: return D3D12_BLEND_OP_ADD;
  }
}

ID3D12PipelineState* GetPipeline(const PipelineKey& key, const GuestShader& vs, const GuestShader& ps,
                                 const GuestDecl& decl) {
  auto it = g.psos.find(key);
  if (it != g.psos.end()) return it->second.Get();
  D3D12_GRAPHICS_PIPELINE_STATE_DESC d = {};
  d.pRootSignature = g.root_signature.Get();
  d.VS = {g_shader_cache.dxil + vs.entry->dxil_offset, vs.entry->dxil_size};
  d.PS = {g_shader_cache.dxil + ps.entry->dxil_offset, ps.entry->dxil_size};
  // Blend (game-level fields, scout C 2.2): src 0-4, op 5-7, dst 8-12, srcA 16-20, opA 21-23, dstA 24-28.
  // key.blend = RB_BLENDCONTROL0 (same field layout as the game-level dword):
  // src 0-4, op 5-7, dst 8-12, srcA 16-20, opA 21-23, dstA 24-28. The XDK
  // writes the identity (ONE/ZERO/ADD) when D3DRS_ALPHABLENDENABLE is off, so
  // "enabled" = "not identity", exactly like the emulator.
  auto& rt = d.BlendState.RenderTarget[0];
  rt.BlendEnable = key.blend != kRbBlendControlIdentity;
  rt.SrcBlend = MapBlendFactor(key.blend & 0x1F);
  rt.BlendOp = MapBlendOp((key.blend >> 5) & 7);
  rt.DestBlend = MapBlendFactor((key.blend >> 8) & 0x1F);
  rt.SrcBlendAlpha = MapBlendFactor((key.blend >> 16) & 0x1F);
  rt.BlendOpAlpha = MapBlendOp((key.blend >> 21) & 7);
  rt.DestBlendAlpha = MapBlendFactor((key.blend >> 24) & 0x1F);
  // D3D12 forbids colour factors in the alpha slots.
  auto alpha_safe = [](D3D12_BLEND b) {
    switch (b) {
      case D3D12_BLEND_SRC_COLOR: return D3D12_BLEND_SRC_ALPHA;
      case D3D12_BLEND_INV_SRC_COLOR: return D3D12_BLEND_INV_SRC_ALPHA;
      case D3D12_BLEND_DEST_COLOR: return D3D12_BLEND_DEST_ALPHA;
      case D3D12_BLEND_INV_DEST_COLOR: return D3D12_BLEND_INV_DEST_ALPHA;
      default: return b;
    }
  };
  rt.SrcBlendAlpha = alpha_safe(rt.SrcBlendAlpha);
  rt.DestBlendAlpha = alpha_safe(rt.DestBlendAlpha);
  rt.RenderTargetWriteMask = uint8_t(key.color_mask & 0xF);
  d.SampleMask = UINT_MAX;
  d.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
  // PA_SU_SC_MODE_CNTL: bit0 cull front, bit1 cull back, bit2 front face = CW.
  const bool cull_front = key.cull & 1, cull_back = key.cull & 2;
  d.RasterizerState.CullMode = cull_front && cull_back ? D3D12_CULL_MODE_BACK
                               : cull_front           ? D3D12_CULL_MODE_FRONT
                               : cull_back            ? D3D12_CULL_MODE_BACK
                                                      : D3D12_CULL_MODE_NONE;
  d.RasterizerState.FrontCounterClockwise = (key.cull & 4) == 0;
  d.RasterizerState.DepthClipEnable = TRUE;
  d.RasterizerState.DepthBias = key.depth_bias;
  d.RasterizerState.DepthBiasClamp = 0.0f;
  std::memcpy(&d.RasterizerState.SlopeScaledDepthBias, &key.slope_bias_bits, 4);
  d.DepthStencilState.DepthEnable = key.ds_format != 0 && (key.depth_control & 2) != 0;
  d.DepthStencilState.DepthWriteMask = (key.depth_control & 4) ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
  d.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC(((key.depth_control >> 4) & 7) + 1);
  // Declaration gaps (dummy elements on slot 15) filled from the shader's own
  // vertex fetches when that stream is bound (stride known from SetStreamSource).
  std::vector<D3D12_INPUT_ELEMENT_DESC> layout = decl.layout;
  for (auto& ie : layout) {
    if (ie.InputSlot != 15) continue;
    for (const auto& v : vs.vfetches) {
      if (v.usage >= 14 || std::strcmp(kUsageSemantics[v.usage], ie.SemanticName) != 0 || v.usage_index != ie.SemanticIndex) continue;
      if (v.stream >= 2 || key.strides[v.stream] == 0 || v.offset + 4 > key.strides[v.stream]) break;
      const DXGI_FORMAT f = MapDeclType(v.decl_type, v.usage);
      if (f == DXGI_FORMAT_UNKNOWN) break;
      ie.Format = f;
      ie.InputSlot = v.stream;
      ie.AlignedByteOffset = v.offset;
      break;
    }
  }
  if (key.instanced) {
    // One record per instance: the microcode fetches record[index / 4] and
    // builds the corner from index % 4 (see g_IndexCount in the recompiler).
    for (auto& ie : layout) {
      ie.InputSlotClass = D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA;
      ie.InstanceDataStepRate = 1;
    }
  }
  d.InputLayout = {layout.data(), UINT(layout.size())};
  d.IBStripCutValue = key.strip_cut == 1 ? D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_0xFFFF
                    : key.strip_cut == 2 ? D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_0xFFFFFFFF
                                         : D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_DISABLED;
  d.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE(key.topology);
  d.NumRenderTargets = key.rt_format ? 1 : 0;
  d.RTVFormats[0] = DXGI_FORMAT(key.rt_format);
  d.DSVFormat = DXGI_FORMAT(key.ds_format);
  d.SampleDesc.Count = 1;
  ComPtr<ID3D12PipelineState> pso;
  const HRESULT hr = g.device->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&pso));
  if (FAILED(hr)) {
    if (g.log_budget) {
      --g.log_budget;
      REXLOG_ERROR("Native renderer: PSO creation failed (hr {:08X}) vs {:016X} ps {:016X} decl {:016X} rt {} ds {}",
                   uint32_t(hr), key.vs, key.ps, key.decl, key.rt_format, key.ds_format);
    }
  }
  g.psos[key] = pso;  // cache failures too, so we do not retry every draw
  g.stats.pso_created++;
  return pso.Get();
}

// Binds RT0/DS + viewport on the list if they changed.
void SyncEdramAlias(GuestSurface& s);  // [NEW FABLE VERSION] defined after Blit

// [NEW FABLE VERSION] rt_cleared: the caller clears the colour target right away,
// so its previous EDRAM content does not matter (no transfer).
bool BindTargets(GuestSurface** out_rt, GuestSurface** out_ds, bool rt_cleared = false) {
  GuestSurface* rt = Lookup(g_surfaces, st.rt0);
  GuestSurface* ds = Lookup(g_surfaces, st.ds);
  if (rt && !EnsureSurfaceResource(*rt)) rt = nullptr;
  if (ds && !EnsureSurfaceResource(*ds)) ds = nullptr;
  if (!rt && !ds) return false;
  if (rt && !rt_cleared) SyncEdramAlias(*rt);
  if (rt) Barrier(rt->resource.Get(), rt->state, D3D12_RESOURCE_STATE_RENDER_TARGET);
  if (ds) Barrier(ds->resource.Get(), ds->state, D3D12_RESOURCE_STATE_DEPTH_WRITE);
  const uintptr_t rt_id = reinterpret_cast<uintptr_t>(rt), ds_id = reinterpret_cast<uintptr_t>(ds);  // [new_fix_24092026] 2.0.3
  if (rt_id != st.bound_rt0 || ds_id != st.bound_ds) {
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = rt ? g.rtvs.Cpu(rt->view_index) : D3D12_CPU_DESCRIPTOR_HANDLE{};
    D3D12_CPU_DESCRIPTOR_HANDLE dsv = ds ? g.dsvs.Cpu(ds->view_index) : D3D12_CPU_DESCRIPTOR_HANDLE{};
    g.list->OMSetRenderTargets(rt ? 1 : 0, rt ? &rtv : nullptr, FALSE, ds ? &dsv : nullptr);
    st.bound_rt0 = rt_id;
    st.bound_ds = ds_id;
  }
  D3D12_VIEWPORT vp = st.viewport;
  const uint32_t w = rt ? rt->width : ds->width, h = rt ? rt->height : ds->height;
  // [NEW FABLE VERSION] internal resolution: the guest's viewport and scissor are
  // in console pixels; the host target is `scale` times larger. When a colour and
  // a depth target ended up at different scales (the 1x fallback above), the
  // smaller one decides, so neither is ever rasterized outside its extent.
  const uint32_t scale = BindScale(rt != nullptr, rt ? rt->scale : 1u, ds != nullptr, ds ? ds->scale : 1u);
  if (!st.viewport_valid || vp.Width <= 0 || vp.Height <= 0) vp = {0, 0, float(w), float(h), 0, 1};
  if (vp.MinDepth > vp.MaxDepth) std::swap(vp.MinDepth, vp.MaxDepth);  // z flipped in the VS (g_NdcZ)
  // Pre-transformed vertices (PA_CL_VTE_CNTL scale/offset bits clear: UI,
  // sprites, post quads) are screen pixels of the whole target; the guest
  // viewport rectangle does not apply to them on Xenos (only the scissor
  // does). The minimap (2026-09-20): four 1000 px tiles drawn with the
  // viewport set to the 232x200 map box were squeezed into the box.
  if ((Dev(kDevPaClVteCntl) & 0x3F) == 0) {
    vp.TopLeftX = 0;
    vp.TopLeftY = 0;
    vp.Width = float(w);
    vp.Height = float(h);
  }
  vp.Width = std::min(vp.Width, float(w) - vp.TopLeftX);
  vp.Height = std::min(vp.Height, float(h) - vp.TopLeftY);
  {  // [NEW FABLE VERSION] guest pixels -> host pixels.
    const ViewportRect scaled = ScaleViewport({vp.TopLeftX, vp.TopLeftY, vp.Width, vp.Height}, scale);
    vp.TopLeftX = scaled.left;
    vp.TopLeftY = scaled.top;
    vp.Width = scaled.width;
    vp.Height = scaled.height;
  }
  g.list->RSSetViewports(1, &vp);
  // Guest window scissor, clamped to the target (the scene targets are
  // 1024x576 under a 1280x720 scissor).
  const uint32_t stl = Dev(kDevPaScWindowScissorTl), sbr = Dev(kDevPaScWindowScissorBr);
  D3D12_RECT scissor = {LONG(std::min<uint32_t>(stl & 0x7FFF, w)), LONG(std::min<uint32_t>((stl >> 16) & 0x7FFF, h)),
                        LONG(std::min<uint32_t>(sbr & 0x7FFF, w)), LONG(std::min<uint32_t>((sbr >> 16) & 0x7FFF, h))};
  if (scissor.right <= scissor.left || scissor.bottom <= scissor.top) scissor = {0, 0, LONG(w), LONG(h)};
  {  // [NEW FABLE VERSION]
    const ScissorRect scaled = ScaleScissor({int32_t(scissor.left), int32_t(scissor.top), int32_t(scissor.right),
                                             int32_t(scissor.bottom)}, scale);
    scissor = {LONG(scaled.left), LONG(scaled.top), LONG(scaled.right), LONG(scaled.bottom)};
  }
  g.list->RSSetScissorRects(1, &scissor);
  *out_rt = rt;
  *out_ds = ds;
  return true;
}

struct DrawArgs {
  uint32_t prim;
  bool indexed;
  uint32_t base_vertex, start_index, count, start_vertex;
  // UP data
  const uint8_t* up_data = nullptr;
  uint32_t up_stride = 0;
};

uint32_t g_skip_log_budget = 60;
void SkipLog(const char* why, uint32_t a = 0, uint32_t b = 0) {
  g.stats.draws_skipped++;
  if (g_skip_log_budget) {
    --g_skip_log_budget;
    REXLOG_WARN("Native renderer: draw skipped: {} ({:08X} {:08X})", why, a, b);
  }
}

// Generates a triangle-list index buffer for quad lists in the upload ring.
// [NEW FABLE VERSION] Triangle fan -> triangle list indices in the upload ring.
// `guest_indices` (16/32-bit, big-endian, already offset to the fan's first
// index) turns an indexed fan into an indexed list; nullptr = plain vertices,
// so the indices are 0..n-1 relative and the caller passes the start vertex as
// the base vertex.
D3D12_GPU_VIRTUAL_ADDRESS GenerateFanIndices(uint32_t vertex_count, const uint8_t* guest_indices, bool index32,
                                             uint32_t& index_count) {
  if (vertex_count < 3) return 0;
  index_count = (vertex_count - 2) * 3;
  UploadRing& ring = g.upload[g.frame_index];
  const size_t off = ring.Allocate(size_t(index_count) * 4, 16);
  if (off == SIZE_MAX) return 0;
  g.stats.upload_bytes += size_t(index_count) * 4;
  auto src = [&](uint32_t i) -> uint32_t {
    if (!guest_indices) return i;
    return index32 ? rex::memory::load_and_swap<uint32_t>(guest_indices + size_t(i) * 4)
                   : rex::memory::load_and_swap<uint16_t>(guest_indices + size_t(i) * 2);
  };
  uint32_t* p = reinterpret_cast<uint32_t*>(ring.mapped + off);
  const uint32_t hub = src(0);
  for (uint32_t i = 1; i + 1 < vertex_count; ++i) {
    *p++ = hub;
    *p++ = src(i);
    *p++ = src(i + 1);
  }
  return ring.buffer->GetGPUVirtualAddress() + off;
}

D3D12_GPU_VIRTUAL_ADDRESS GenerateQuadIndices(uint32_t vertex_count, uint32_t& index_count) {
  const uint32_t quads = vertex_count / 4;
  index_count = quads * 6;
  UploadRing& ring = g.upload[g.frame_index];
  const size_t off = ring.Allocate(size_t(index_count) * 4, 16);
  if (off == SIZE_MAX) return 0;
  g.stats.upload_bytes += size_t(index_count) * 4;
  uint32_t* p = reinterpret_cast<uint32_t*>(ring.mapped + off);
  for (uint32_t q = 0; q < quads; ++q) {
    const uint32_t v = q * 4;
    *p++ = v; *p++ = v + 1; *p++ = v + 2;
    *p++ = v; *p++ = v + 2; *p++ = v + 3;
  }
  return ring.buffer->GetGPUVirtualAddress() + off;
}

uint32_t g_draw_dump_budget = 0;   // detailed (vertex data) dumps left in the dump frame
bool g_dump_active = false;         // dump frame in progress: one compact line per draw/clear/resolve
uint32_t g_dump_seq = 0;            // event index inside the dump frame
uint32_t g_dump_request_frame = 0;  // set by logs/native_dump.trigger
std::vector<uint32_t> g_dump_textures;  // CPU textures sampled in the dump frame (raw bytes written by WriteDumps)

// One line per draw of the dump frame: enough to follow a whole pass chain offline.
// The host texture for a fetch constant. Bases are normalized to physical (the
// header keeps the guest virtual base, the device shadow the physical one the
// XDK converted it to) so both spellings of one memory range share the key.
void InitTextureFromFetch(GuestTexture& t, uint32_t object) {
  t.info_valid = rex::graphics::TextureInfo::Prepare(t.fetch, &t.info);
  if (t.info_valid) {
    // TextureInfo keeps the fetch constant's "size minus one" fields.
    t.width = t.info.width + (REXCVAR_GET(dp_native_dump_frame) == -7 ? 0 : 1);
    t.height = t.info.height + (REXCVAR_GET(dp_native_dump_frame) == -7 ? 0 : 1);
    // Stacked (2D array) textures are dimension k2DOrStacked + the stacked bit
    // (size_2d.stack_depth), NOT k3D: the sun shadow cascades (5 x 1024^2) are
    // one of them, sampled with tfetch3D and z = slice centre / depth.
    const bool volume = t.info.dimension == xenos::DataDimension::k3D;
    t.is_stacked_3d = t.info.is_stacked;
    t.is_3d = volume || t.is_stacked_3d;
    t.depth = t.is_3d ? t.info.depth + 1 : 1;
    t.is_cube = t.info.dimension == xenos::DataDimension::kCube;
    // [NEW FABLE VERSION] host mip chain: guest levels 0..mip_max_level (Prepare
    // zeroes mip_max_level when the fetch constant has no mip memory).
    t.mip_levels = 1;
    if (!t.is_3d) {
      const uint32_t max_levels = std::max(1u, t.info.GetMaxMipLevels());
      t.mip_levels = std::max(1u, std::min(t.info.mip_max_level + 1, max_levels));
    }
    t.format = MapTextureFormat(t.info.format);
    if (t.format == DXGI_FORMAT_UNKNOWN && g.log_budget) {
      --g.log_budget;
      REXLOG_WARN("Native renderer: texture {:08X} format {} ({}x{}) unsupported", object, uint32_t(t.info.format),
                  t.width, t.height);
    }
  } else if (g.log_budget) {
    --g.log_budget;
    REXLOG_WARN("Native renderer: texture {:08X} fetch constant not understood ({:08X} {:08X} {:08X})", object,
                t.fetch.dword_0, t.fetch.dword_1, t.fetch.dword_2);
  }
}

GuestTexture* TextureForFetch(const uint32_t dwords[6], uint32_t object) {
  if ((dwords[0] & 3) != 2) return nullptr;
  uint32_t d[6];
  std::memcpy(d, dwords, sizeof(d));
  const uint32_t base = d[1] & ~0xFFFu, mip = d[5] & ~0xFFFu;
  d[1] = (d[1] & 0xFFFu) | (GuestPhysical(base) & ~0xFFFu);
  if (mip) d[5] = (d[5] & 0xFFFu) | (GuestPhysical(mip) & ~0xFFFu);
  // Everything that decides the host resource and its content: base, mips, format,
  // endian, size/dimension/stack, tiling, pitch. Swizzle/filters are per bind.
  uint32_t keysrc[5] = {d[0] & 0xFFC00003u /*pitch, tiled, type*/, d[1] & ~0x3C0u /*base, format, endian, stacked; not request size*/,
                        d[2], d[5] & ~0x1FFu /*mip base, dim*/, (d[3] >> 13) & 0x3Fu /*exp_adjust*/};
  const uint64_t key = XXH3_64bits(keysrc, sizeof(keysrc));
  std::lock_guard<std::mutex> lock(g_registry_mutex);
  if (object) g_object_key[object] = key;
  auto it = g_textures_by_key.find(key);
  if (it != g_textures_by_key.end()) return it->second.get();
  {
    const rex::graphics::FormatInfo* fi = rex::graphics::FormatInfo::Get(d[1] & 0x3Fu);
    if (!fi || !fi->bytes_per_block() || (d[0] >> 22) == 0) return nullptr;  // no pitch / unknown format
  }
  auto t = std::make_unique<GuestTexture>();
  t->guest = object;
  for (int i = 0; i < 6; ++i) (&t->fetch.dword_0)[i] = d[i];
  InitTextureFromFetch(*t, object);
  GuestTexture* raw = t.get();
  g_textures_by_key[key] = std::move(t);
  return raw;
}

// Fetch dwords of a texture object's header (+28), normalized by TextureForFetch.
GuestTexture* TextureForObject(uint32_t object) {
  if (!object) return nullptr;
  uint32_t d[6];
  for (int i = 0; i < 6; ++i) d[i] = LoadU32(object + 28 + i * 4);
  return TextureForFetch(d, object);
}

// [NEW FABLE VERSION] 2026-09-24: see dp_native_resolve_alias. The resolve
// destination that last wrote `t`'s memory, when that write is newer than any
// CPU write of it we saw and the two views agree on size and format.
GuestTexture* ResolvedAlias(const GuestTexture& t) {
  if (!REXCVAR_GET(dp_native_resolve_alias) || !t.info_valid || t.is_cube || t.is_3d) return nullptr;
  auto it = g_resolve_by_base.find(t.info.memory.base_address);
  if (it == g_resolve_by_base.end()) return nullptr;
  GuestTexture* r = it->second;
  if (r == &t || !r->resource || r->srv_index == UINT32_MAX) return nullptr;
  const bool same_shape = !r->is_cube && !r->is_3d && r->width == t.width && r->height == t.height &&
                          r->info.format == t.info.format;
  if (!ResolveIsNewestWriter(t.mem_seq, r->mem_seq, same_shape)) return nullptr;
  static uint32_t alias_log = 8;
  if (alias_log) {
    --alias_log;
    REXLOG_INFO("Native renderer: texture {:08X} ({:08X} {:08X} {:08X} {:08X} {:08X} {:08X}) samples resolve target {:08X} "
                "({:08X} {:08X} {:08X} {:08X} {:08X} {:08X}): same memory {:08X}, {}x{}",
                t.guest, t.fetch.dword_0, t.fetch.dword_1, t.fetch.dword_2, t.fetch.dword_3, t.fetch.dword_4,
                t.fetch.dword_5, r->guest, r->fetch.dword_0, r->fetch.dword_1, r->fetch.dword_2, r->fetch.dword_3,
                r->fetch.dword_4, r->fetch.dword_5, t.info.memory.base_address, t.width, t.height);
  }
  return r;
}

void DumpDrawLine(const DrawArgs& a, const GuestShader& vs, const GuestShader& ps, const GuestDecl& decl,
                  const GuestSurface* rt, const GuestSurface* ds) {
  std::string out = fmt::format(
      "Native drawline #{} prim {} idx {} b{} s{} n{} up {} | vs {:016X} ps {:016X} | rt {:08X} ({}x{} fmt {}) ds {:08X} | dc {:08X} cull {:X} vte {:03X} cc {:08X} bl {:08X}/{:08X} m {:X} vp {},{} {}x{} z {}..{} | decl {:016X} str {}/{} off {}/{} vb {:08X}/{:08X} |",
      g_dump_seq, a.prim, a.indexed, a.base_vertex, a.start_index, a.count, a.up_data != nullptr, vs.hash, ps.hash,
      st.rt0, rt ? rt->width : 0, rt ? rt->height : 0, rt ? uint32_t(rt->format) : 0, st.ds, Dev(kDevRbDepthControl),
      Dev(kDevPaSuScModeCntl) & 7, Dev(kDevPaClVteCntl) & 0xFFF, Dev(kDevRbColorControl), Dev(kDevRbBlendControl0),
      Dev(kDevBlendEnable), Dev(kDevRbColorMask) & 0xF, st.viewport.TopLeftX, st.viewport.TopLeftY, st.viewport.Width,
      st.viewport.Height, st.viewport.MinDepth, st.viewport.MaxDepth, decl.hash, st.streams[0].stride,
      st.streams[1].stride, st.streams[0].offset, st.streams[1].offset, st.streams[0].vb, st.streams[1].vb);

  for (uint32_t slot = 0; slot < 32; ++slot) {
    if (!st.textures[slot] || (Dev(kDevFetchConstants + slot * 24) & 3) != 2) continue;
    uint32_t fd[6];
    for (int i = 0; i < 6; ++i) fd[i] = Dev(kDevFetchConstants + slot * 24 + i * 4);
    GuestTexture* t = TextureForFetch(fd, st.textures[slot]);
    out += fmt::format(" s{}={:08X}({}x{} f{} sw{:03X} {})", slot, st.textures[slot], t ? t->width : 0, t ? t->height : 0,
                       t ? uint32_t(t->info.format) : 0, t ? uint32_t(t->fetch.swizzle) : 0,
                       !t ? "UNREG" : t->is_resolve_target ? "rsv" : t->resource ? "host" : "NOHOST");
    if (t && !t->is_resolve_target && t->info_valid &&
        std::find(g_dump_textures.begin(), g_dump_textures.end(), t->guest) == g_dump_textures.end()) {
      g_dump_textures.push_back(t->guest);
    }
  }
  REXLOG_INFO("{}", out);
}

void DumpDrawState(const DrawArgs& a, const GuestShader& vs, const GuestShader& ps, const GuestDecl& decl,
                   const GuestSurface* rt, const GuestSurface* ds) {
  std::string out = fmt::format("Native drawdump: prim {} indexed {} base {} start {} count {} startv {} up {} | vs {:016X} ps {:016X} | rt {:08X} ({}x{} fmt {}) ds {:08X} | depthctl {:08X} cull {:08X} vte {:08X} colorctl {:08X} blend {:08X}/{:08X} mask {:X} | vp {},{} {}x{} z {}..{} valid {}",
                                a.prim, a.indexed, a.base_vertex, a.start_index, a.count, a.start_vertex, a.up_data != nullptr,
                                vs.hash, ps.hash, st.rt0, rt ? rt->width : 0, rt ? rt->height : 0, rt ? uint32_t(rt->format) : 0, st.ds,
                                Dev(kDevRbDepthControl), Dev(kDevPaSuScModeCntl), Dev(kDevPaClVteCntl), Dev(kDevRbColorControl),
                                Dev(kDevBlendFactors), Dev(kDevBlendEnable), Dev(kDevRbColorMask) & 0xF, st.viewport.TopLeftX,
                                st.viewport.TopLeftY, st.viewport.Width, st.viewport.Height, st.viewport.MinDepth,
                                st.viewport.MaxDepth, st.viewport_valid);
  out += "\n  decl:";
  for (const auto& e : decl.elements) {
    out += fmt::format(" [s{} +{} type {} usage {}.{}]", e.stream, e.offset, e.type, e.usage, e.usage_index);
  }
  if (a.up_data) {
    out += fmt::format("\n  UP vertices: stride {} count {}", a.up_stride, a.count);
    for (uint32_t v = 0; v < std::min(a.count, 4u); ++v) {
      out += fmt::format("\n    v{}:", v);
      for (uint32_t d = 0; d < std::min(a.up_stride / 4u, 12u); ++d) {
        const uint32_t raw = rex::memory::load_and_swap<uint32_t>(a.up_data + size_t(v) * a.up_stride + d * 4);
        float f;
        std::memcpy(&f, &raw, 4);
        out += fmt::format(" {:08X}({:.3g})", raw, f);
      }
    }
  }
  for (uint32_t sidx = 0; sidx < 2; ++sidx) {
    const auto& str = st.streams[sidx];
    if (!str.vb) continue;
    GuestBuffer* vb = Lookup(g_buffers, str.vb);
    out += fmt::format("\n  stream {}: vb {:08X} offset {} stride {}", sidx, str.vb, str.offset, str.stride);
    if (vb) {
      RefreshBufferHeader(*vb);
      out += fmt::format(" (guest addr {:08X} size {}) header:", vb->address, vb->size);
      for (int d = 0; d < 8; ++d) out += fmt::format(" {:08X}", LoadU32(vb->guest + d * 4));
      const uint8_t* p = HeaderPtr(vb->address) + std::min(str.offset, vb->size);
      const uint32_t base = a.indexed ? a.base_vertex : a.start_vertex;
      for (uint32_t v = 0; v < 3; ++v) {
        out += fmt::format("\n    v{}:", base + v);
        const uint8_t* vp = p + size_t(base + v) * str.stride;
        for (uint32_t d = 0; d < std::min(str.stride / 4u, 8u); ++d) {
          const uint32_t raw = rex::memory::load_and_swap<uint32_t>(vp + d * 4);
          float f;
          std::memcpy(&f, &raw, 4);
          out += fmt::format(" {:08X}({:.3g})", raw, f);
        }
      }
    }
  }
  if (a.indexed) {
    GuestBuffer* ib = Lookup(g_buffers, st.ib);
    out += fmt::format("\n  ib {:08X}", st.ib);
    if (ib) {
      RefreshBufferHeader(*ib);
      out += fmt::format(" (guest addr {:08X} size {}) header:", ib->address, ib->size);
      for (int d = 0; d < 8; ++d) out += fmt::format(" {:08X}", LoadU32(ib->guest + d * 4));
      out += " first dwords:";
      const uint8_t* p = HeaderPtr(ib->address);
      for (int d = 0; d < 6; ++d) out += fmt::format(" {:08X}", rex::memory::load_and_swap<uint32_t>(p + d * 4));
    }
  }
  // Whole device block for offline analysis (register shadow offsets are
  // still being mapped: loop/bool constants, alpha ref...).
  {
    std::error_code ec;
    const std::filesystem::path dir = std::filesystem::current_path() / "logs" / "native_dump";
    std::filesystem::create_directories(dir, ec);
    std::ofstream out_dev(dir / fmt::format("f{}_draw{:04}_dev.bin", g.frame_number, g_dump_seq), std::ios::binary);
    out_dev.write(reinterpret_cast<const char*>(Mem()->TranslateVirtual(g_device)), 24576);
    // Raw guest buffers of the draw (offline vertex shader replay); 96 MB budget per dump.
    static uint32_t buffer_budget = 0, budget_frame = UINT32_MAX;
    const bool new_frame = budget_frame != g.frame_number;
    if (new_frame) { budget_frame = g.frame_number; buffer_budget = 256u << 20; }
    // Keyed by the buffer's current data address, not the object: a dynamic
    // pool buffer keeps its object while Lock hands out a new address (and new
    // contents) for every write, so an object-keyed .ref pointed at stale
    // bytes for every UI draw (minimap study, 2026-09-20).
    static std::unordered_map<uint64_t, uint32_t> dumped_buffers;  // (address, size) -> draw seq that wrote it (per frame)
    if (new_frame) dumped_buffers.clear();
    auto dump_buffer = [&](const GuestBuffer* b, const char* suffix) {
      if (!b || !b->size) return;
      // Content-keyed: the same pool address holds different bytes for later draws.
      const uint64_t dkey = XXH3_64bits(HeaderPtr(b->address), b->size) ^ (uint64_t(b->size) << 40);
      if (auto it = dumped_buffers.find(dkey); it != dumped_buffers.end()) {
        // Same buffer as an earlier draw: a hard link would need privileges; a tiny
        // redirect file names the draw whose file holds the bytes.
        std::ofstream out(dir / fmt::format("f{}_draw{:04}_{}.ref", g.frame_number, g_dump_seq, suffix));
        out << it->second;
        return;
      }
      if (b->size > buffer_budget) return;
      dumped_buffers[dkey] = g_dump_seq;
      buffer_budget -= b->size;
      std::ofstream out(dir / fmt::format("f{}_draw{:04}_{}.bin", g.frame_number, g_dump_seq, suffix), std::ios::binary);
      out.write(reinterpret_cast<const char*>(HeaderPtr(b->address)), b->size);
    };
    for (uint32_t sidx = 0; sidx < 2; ++sidx) {
      if (!st.streams[sidx].vb) continue;
      dump_buffer(Lookup(g_buffers, st.streams[sidx].vb), sidx == 0 ? "s0" : "s1");
    }
    if (a.indexed) dump_buffer(Lookup(g_buffers, st.ib), "ib");
    // The vertex shader container as the XDK left it after patching the vertex
    // fetches for this draw's declaration (offline decode of the real layout).
    if (vs.container && vs.container_size) {
      std::ofstream out(dir / fmt::format("f{}_draw{:04}_vs.bin", g.frame_number, g_dump_seq), std::ios::binary);
      out.write(reinterpret_cast<const char*>(Mem()->TranslateVirtual(vs.container)), vs.container_size);
    }
  }
  out += "\n  vs c0-c3:";
  for (int c = 0; c < 4; ++c) {
    for (int k = 0; k < 4; ++k) out += fmt::format(" {:.3g}", LoadF32(g_device + kDevVsConstants + (c * 4 + k) * 4));
    out += " |";
  }
  out += "\n  vs c244-c248:";
  for (int c = 244; c < 249; ++c) {
    for (int k = 0; k < 4; ++k) out += fmt::format(" {:.3g}", LoadF32(g_device + kDevVsConstants + (c * 4 + k) * 4));
    out += " |";
  }
  out += "\n  textures:";
  for (uint32_t slot = 0; slot < 32; ++slot) {
    if (st.textures[slot]) {
      GuestTexture* t = TextureForObject(st.textures[slot]);
      out += fmt::format(" s{}={:08X}({}x{} fmt {} {})", slot, st.textures[slot], t ? t->width : 0, t ? t->height : 0,
                         t ? uint32_t(t->format) : 0, t && t->resource ? "host" : "NOHOST");
    }
  }
  REXLOG_INFO("{}", out);
}

void RecordDump(const char* what, uint32_t guest, ID3D12Resource* res, D3D12_RESOURCE_STATES& state, uint32_t w,
                uint32_t h, DXGI_FORMAT format, uint32_t slice = 0, uint32_t slices = 1);
void ExecuteDraw(const DrawArgs& a) {
  ScopedCpuTimer cpu_timer;  // [NEW FABLE VERSION]
  RecordScope record_scope(__func__);  // [new_fix_24092026] 2.0.3
  if (!InitContext() || !BeginFrame()) {
    g.stats.draws_skipped++;
    return;
  }
  GuestShader* vs = Lookup(g_shaders, st.vs);
  GuestShader* ps = Lookup(g_shaders, st.ps);
  GuestDecl* decl = Lookup(g_decls, st.decl);
  if (!vs || !ps || !vs->entry || !ps->entry) return SkipLog("shader missing", st.vs, st.ps);
  if (!decl) return SkipLog("vertex declaration missing", st.decl);
  GuestSurface* rt = nullptr;
  GuestSurface* ds = nullptr;
  if (!BindTargets(&rt, &ds)) return SkipLog("no render target", st.rt0, st.ds);
  if (g_dump_active) {
    DumpDrawLine(a, *vs, *ps, *decl, rt, ds);
    if (g_draw_dump_budget) {
      // Detailed dump once per (pass, shader pair): every change of VS/PS/RT/decl.
      static uint64_t last_key = 0;
      const uint64_t key = vs->hash ^ (ps->hash * 31) ^ (uint64_t(st.rt0) << 32) ^ decl->hash;
      if (key != last_key || REXCVAR_GET(dp_native_dump_all)) {
        last_key = key;
        --g_draw_dump_budget;
        DumpDrawState(a, *vs, *ps, *decl, rt, ds);
      }
    }
    // Off-by-one fix (2026-09-20): the drawline and the draw's files (_dev/_s0/
    // _ib/_vs, d{seq} target dumps) now carry the same number.
    ++g_dump_seq;
  }

  // Topology.
  D3D12_PRIMITIVE_TOPOLOGY topo;
  uint32_t topo_type;
  bool quads = false;
  bool fan = false;  // [NEW FABLE VERSION] Xenos triangle fan (5)
  switch (a.prim) {
    case 1: topo = D3D_PRIMITIVE_TOPOLOGY_POINTLIST; topo_type = D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT; break;
    case 2: topo = D3D_PRIMITIVE_TOPOLOGY_LINELIST; topo_type = D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE; break;
    case 3: topo = D3D_PRIMITIVE_TOPOLOGY_LINESTRIP; topo_type = D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE; break;
    case 4: topo = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST; topo_type = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE; break;
    case 6: topo = D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP; topo_type = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE; break;
    case 13: topo = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST; topo_type = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE; quads = true; break;
    case 5: topo = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST; topo_type = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE; fan = true; break;  // [NEW FABLE VERSION]
    default: return SkipLog("primitive type unsupported", a.prim);
  }

  // Vertex buffers.
  D3D12_VERTEX_BUFFER_VIEW vbv[2] = {};
  uint32_t vbv_count = 0;
  uint32_t strides[2] = {0, 0};
  if (a.up_data) {
    UploadRing& ring = g.upload[g.frame_index];
    const size_t bytes = size_t(a.count) * a.up_stride;
    const size_t off = ring.Allocate((bytes + 3) & ~3u, 16);
    if (off == SIZE_MAX) return SkipLog("upload ring full (UP)");
    rex::memory::copy_and_swap_32_unaligned(ring.mapped + off, a.up_data, (bytes + 3) / 4);
    g.stats.upload_bytes += bytes;
    vbv[0] = {ring.buffer->GetGPUVirtualAddress() + off, UINT(bytes), a.up_stride};
    vbv_count = 1;
    strides[0] = a.up_stride;
  } else {
    for (uint32_t s = 0; s < 2; ++s) {
      if (!st.streams[s].vb) continue;
      GuestBuffer* vb = Lookup(g_buffers, st.streams[s].vb);
      if (!vb) return SkipLog("vertex buffer not registered", st.streams[s].vb);
      RefreshBufferHeader(*vb);
      if (vb->written.load(std::memory_order_acquire)) vb->dirty = true;
      if (vb->dirty && !UploadBuffer(*vb)) return SkipLog("vertex buffer upload failed", vb->guest);
      const uint32_t offset = std::min(st.streams[s].offset, vb->size);
      vbv[s] = {vb->resource->GetGPUVirtualAddress() + offset, vb->size - offset, st.streams[s].stride};
      strides[s] = st.streams[s].stride;
      vbv_count = s + 1;
    }
    if (!vbv_count) return SkipLog("no vertex stream");
  }
  // Index buffer.
  D3D12_INDEX_BUFFER_VIEW ibv = {};
  bool indexed = a.indexed;
  uint32_t index_count = a.count;
  uint32_t start_index = a.start_index;
  int32_t base_vertex = int32_t(a.base_vertex);
  bool strip_cut = false;
  // Index-scaled vertex fetch (cache flag bit 0): the VS fetches
  // record[index / 4] and derives the corner from index % 4 (particles),
  // which the input assembler cannot do. Draw one instance per record with a
  // per-instance layout and one quad's indices; the VS gets r0.x =
  // vertexId + g_IndexCount * instanceId (DP1 flamethrower, 2026-09-20).
  const bool index_scaled = (vs->entry->reserved & 1u) != 0;
  uint32_t instance_count = 1, start_instance = 0;
  bool instanced = false;
  if (quads && index_scaled && !a.indexed && !a.up_data) {
    ibv.BufferLocation = GenerateQuadIndices(4, index_count);
    if (!ibv.BufferLocation) return SkipLog("upload ring full (quads)");
    ibv.Format = DXGI_FORMAT_R32_UINT;
    ibv.SizeInBytes = index_count * 4;
    indexed = true;
    instanced = true;
    start_index = 0;
    base_vertex = 0;
    instance_count = a.count / 4;
    start_instance = a.start_vertex / 4;
  } else if (fan) {
    // [NEW FABLE VERSION] triangle fan: generated list indices; an indexed fan
    // reads its guest indices at record time (they are a handful of vertices).
    const uint8_t* guest_indices = nullptr;
    bool index32 = false;
    if (a.indexed) {
      GuestBuffer* ib = Lookup(g_buffers, st.ib);
      if (!ib) return SkipLog("index buffer not registered (fan)", st.ib);
      RefreshBufferHeader(*ib);
      if (!ib->size) return SkipLog("index buffer empty (fan)", ib->guest);
      index32 = ib->index32;
      const uint32_t index_bytes = index32 ? 4u : 2u;
      if (size_t(a.start_index + a.count) * index_bytes > ib->size) return SkipLog("fan indices out of range", ib->guest, a.count);
      guest_indices = HeaderPtr(ib->address) + size_t(a.start_index) * index_bytes;
    }
    ibv.BufferLocation = GenerateFanIndices(a.count, guest_indices, index32, index_count);
    if (!ibv.BufferLocation) return SkipLog("upload ring full (fan)", a.count);
    ibv.Format = DXGI_FORMAT_R32_UINT;
    ibv.SizeInBytes = index_count * 4;
    indexed = true;
    start_index = 0;
    base_vertex = a.indexed ? int32_t(a.base_vertex) : int32_t(a.start_vertex);
  } else if (quads) {
    if (a.indexed) return SkipLog("indexed quad list unsupported");
    ibv.BufferLocation = GenerateQuadIndices(a.count, index_count);
    if (!ibv.BufferLocation) return SkipLog("upload ring full (quads)");
    ibv.Format = DXGI_FORMAT_R32_UINT;
    ibv.SizeInBytes = index_count * 4;
    indexed = true;
    start_index = 0;
    base_vertex = int32_t(a.start_vertex);
  } else if (a.indexed) {
    GuestBuffer* ib = Lookup(g_buffers, st.ib);
    if (!ib) return SkipLog("index buffer not registered", st.ib);
    RefreshBufferHeader(*ib);
    if (ib->written.load(std::memory_order_acquire)) ib->dirty = true;
    if (ib->dirty && !UploadBuffer(*ib)) return SkipLog("index buffer upload failed", ib->guest);
    ibv = {ib->resource->GetGPUVirtualAddress(), ib->size, ib->index32 ? DXGI_FORMAT_R32_UINT : DXGI_FORMAT_R16_UINT};
    // [new_fix_24092026] 2.0.4: D3D12 reads 0 past the end of the view; the
    // console reads whatever memory follows. Logged, not guessed at.
    if (size_t(a.start_index) + a.count > ib->size / (ib->index32 ? 4u : 2u)) {
      static std::atomic<uint32_t> past_end{0};
      if (past_end.fetch_add(1, std::memory_order_relaxed) < 8) {
        REXLOG_WARN("Native renderer: draw reads indices {}..{} of index buffer {:08X}, which holds {} ({} bytes)",
                    a.start_index, a.start_index + a.count, ib->guest, ib->size / (ib->index32 ? 4u : 2u), ib->size);
      }
    }
    strip_cut = a.prim == 6;
    start_index = a.start_index;
  }

  // Textures + samplers -> shared constants.
  alignas(16) uint32_t shared[kSharedConstantsBytes / 4] = {};
  uint32_t* tex2d = shared + 0;      // g_Tex2DIdx[8] = 32 uints
  uint32_t* tex3d = shared + 32;     // g_Tex3DIdx
  uint32_t* texcube = shared + 64;   // g_TexCubeIdx
  uint32_t* sampidx = shared + 96;   // g_SampIdx
  uint32_t* bools = shared + 128;    // g_Bools[2] = 8 uints
  uint32_t* loops = shared + 136;    // g_Loops[8] = 32 uints
  for (uint32_t i = 0; i < 8; ++i) bools[i] = Dev(kDevBoolConstants + i * 4);
  for (uint32_t slot = 0; slot < 32; ++slot) {
    tex2d[slot] = g.null_srv_2d;
    tex3d[slot] = g.null_srv_3d;
    texcube[slot] = g.null_srv_cube;
    sampidx[slot] = 0;
    // The XDK unbinds a sampler by clearing the fetch constant's type (SetTexture
    // NULL leaves the rest of the constant behind): only type 2 is a texture.
    if ((Dev(kDevFetchConstants + slot * 24) & 3) != 2) continue;
    uint32_t fetch_dwords[6];
    for (int i = 0; i < 6; ++i) fetch_dwords[i] = Dev(kDevFetchConstants + slot * 24 + i * 4);
    GuestTexture* t = TextureForFetch(fetch_dwords, st.textures[slot]);
    if (!t) {
      if (g_dump_active) {
        REXLOG_INFO("Native texbind: s{} object {:08X} is a typed fetch constant (base {:08X}) but not a registered texture: sampled as zeros (draw #{})",
                    slot, st.textures[slot], Dev(kDevFetchConstants + slot * 24 + 4) & ~0xFFFu, g_dump_seq);
      }
      continue;
    }
    // Vertex-fetch slots hold the bone palettes (32x256 RGBA32F) the game
    // rewrites in place for every skinned mesh, without Lock/Unlock: hash
    // the guest memory at every bind and re-upload when it changed.
    if (t->written.exchange(false, std::memory_order_acq_rel)) {
      t->dirty = true;
      t->mem_seq = ++g_mem_seq;  // [NEW FABLE VERSION] the CPU wrote it: newer than any resolve so far
      g.stats.tex_changes++;
      if (g_dump_active) {
        REXLOG_INFO("Native texwatch: s{} {:08X} ({} bytes) written by the CPU before draw #{}", slot, t->guest,
                    t->info.memory.base_size, g_dump_seq);
      }
    }
    if (slot >= 16 && !t->is_resolve_target && t->info_valid && t->info.memory.base_size && !t->watched &&
        !WatchableAddress(t->info.memory.base_address)) {
      if (!t->unwatchable_logged) {
        t->unwatchable_logged = true;
        REXLOG_WARN("Native renderer: vertex texture {:08X} data at {:08X} is not in a physical heap: hashing it per draw",
                    t->guest, t->info.memory.base_address);
      }
      const uint64_t hash = XXH3_64bits(HeaderPtr(t->info.memory.base_address), t->info.memory.base_size);
      g.stats.tex_rehash++;
      if (hash != t->content_hash) {
        t->content_hash = hash;
        t->dirty = true;
        t->mem_seq = ++g_mem_seq;  // [NEW FABLE VERSION]
        g.stats.tex_changes++;
        if (g_dump_active) {
          REXLOG_INFO("Native texwatch: s{} {:08X} ({} bytes) content changed before draw #{}", slot, t->guest,
                      t->info.memory.base_size, g_dump_seq);
        }
      }
    }
    // [NEW FABLE VERSION] 2026-09-24: memory last written by a resolve through
    // another texture object (dp_native_resolve_alias).
    if (!t->is_resolve_target) {
      if (GuestTexture* r = ResolvedAlias(*t)) {
        t = r;
        g.stats.resolve_aliases++;
      }
    }
    if (t->dirty && !t->is_resolve_target) {
      if (!UploadTexture(*t)) continue;
    } else if (!t->resource) {
      continue;
    }
    if (t->state != (D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)) {
      Barrier(t->resource.Get(), t->state,
              D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }
    if (t->is_cube) texcube[slot] = t->srv_index;
    else if (t->depth > 1) tex3d[slot] = t->srv_index;
    else tex2d[slot] = t->srv_index;
    // Sampler state lives in the device's fetch constant shadow for this slot.
    xenos::xe_gpu_texture_fetch_t fetch;
    for (int i = 0; i < 6; ++i) (&fetch.dword_0)[i] = Dev(kDevFetchConstants + slot * 24 + i * 4);
    sampidx[slot] = SamplerIndex(fetch);
  }
  for (uint32_t i = 0; i < 32; ++i) loops[i] = Dev(kDevLoopConstants + i * 4);
  shared[168] = decl->swapped_texcoords;   // c42.x
  {
    // c42.yz g_HalfPixelOffset: the 360 rasterizer uses D3D9 pixel centres;
    // the recompiled VS adds this * w to clip xy (same values as
    // Unleashed/Marathon: +1/W, -1/H of the bound target).
    const float w = float(rt ? rt->width : ds->width), h = float(rt ? rt->height : ds->height);
    const float hp[2] = {1.0f / w, -1.0f / h};
    std::memcpy(&shared[169], hp, sizeof(hp));
  }
  const uint32_t color_control = Dev(kDevRbColorControl);
  // RB_COLORCONTROL bits 0-2 = alpha compare function, bit 3 = alpha test enable.
  // The recompiled PS only knows clip(alpha - threshold) = GEQUAL: GREATER (DP1's
  // hair and foliage, ref 0 / 0.941) needs the threshold nudged past the reference
  // so alpha == ref is discarded too; ALWAYS/NEVER are expressible; the rest are
  // not (logged once).
  bool alpha_test = (color_control & 8) != 0 && !REXCVAR_GET(dp_native_no_alphatest);
  float alpha_ref = LoadF32(g_device + kDevAlphaRef);
  if (alpha_test) {
    switch (color_control & 7) {
      case 7: alpha_test = false; break;                       // always
      case 0: alpha_ref = 2.0f; break;                         // never: discard everything
      case 6: break;                                           // gequal
      case 4: alpha_ref += 1.0f / 512.0f; break;               // greater (8-bit alpha steps are 1/255)
      default: {
        static uint32_t logged = 0;
        if (logged < 4) {
          ++logged;
          REXLOG_WARN("Native renderer: alpha test function {} not expressible as clip(alpha - ref), using gequal", color_control & 7);
        }
        break;
      }
    }
  }
  std::memcpy(&shared[171], &alpha_ref, 4);  // c42.w g_AlphaThreshold
  shared[172] = alpha_test ? 1u : 0u;        // c43.x g_AlphaTest
  shared[173] = decl->normal_mode;           // c43.y g_R11G11B10Normal (dp1: normal decode mode)
  shared[174] = 0;                            // c43.z swapped position
  shared[175] = 0;                            // c43.w swapped color
  shared[176] = decl->swapped_blend;         // c44.x g_SwappedBlend
  shared[177] = instanced ? 4u : 0u;         // c44.y g_IndexCount (vertices per instance, 0 = plain draw)
  // c45: NDC scale/offset. With PA_CL_VTE_CNTL scale/offset bits clear the
  // VS outputs screen-space pixels (sprites, UI, post quads): map them to clip
  // space against the bound target's size. Otherwise identity.
  {
    const uint32_t vte = Dev(kDevPaClVteCntl);
    float ndc[4] = {1.0f, 1.0f, 0.0f, 0.0f};
    if ((vte & 0x3F) == 0) {
      const float w = float(rt ? rt->width : ds->width), h = float(rt ? rt->height : ds->height);
      ndc[0] = 2.0f / w;
      ndc[1] = -2.0f / h;
      ndc[2] = -1.0f;
      ndc[3] = 1.0f;
    }
    std::memcpy(&shared[180], ndc, sizeof(ndc));
  }
  // c46: depth scale/offset. A guest viewport with MinZ > MaxZ (DP1 uses
  // MinZ 1, MaxZ 0) cannot be expressed on the host: flip z in the VS and use
  // the swapped range instead (same depth = z * (max - min) + min).
  {
    float zso[4] = {1.0f, 0.0f, 0.0f, 0.0f};
    if (st.viewport_valid && st.viewport.MinDepth > st.viewport.MaxDepth) {
      zso[0] = -1.0f;
      zso[1] = 1.0f;
    }
    // User clip plane 0 (the mirrored floor-reflection pass clips everything
    // below the floor): plane in c47, enable in c46.z; the VS writes SV_ClipDistance0.
    if (Dev(kDevPaClClipCntl) & 1) {
      zso[2] = 1.0f;
      for (int i = 0; i < 4; ++i) shared[188 + i] = Dev(kDevPaClUcp0 + i * 4);
    }
    std::memcpy(&shared[184], zso, sizeof(zso));
  }
  // [NEW FABLE VERSION] c48.x: the bound colour target is 7e3 (k_2_10_10_10_FLOAT);
  // the recompiled PS clamps its colour output to the console's 0..31.875 range
  // per draw (before this only the resolve clamped, so additive passes and the
  // lamp/flamethrower glows accumulated far beyond what the console could store).
  shared[192] = (rt && (rt->guest_format & 0x3F) == 63) ? 1u : 0u;
  {
    // [NEW FABLE VERSION] c48.w: 1 / internal resolution scale of the bound target,
    // so a pixel shader's VPOS stays in console pixels (the game's screen-space
    // constants are for 1024x576 / 1280x720). The VS half-pixel term is applied
    // in clip space and needs nothing.
    const float vpos_scale = 1.0f / float(BindScale(rt != nullptr, rt ? rt->scale : 1u, ds != nullptr, ds ? ds->scale : 1u));
    std::memcpy(&shared[195], &vpos_scale, 4);
  }

  // Constants: whole banks, byte-swapped, into the ring. [NEW FABLE VERSION] only
  // when the guest bank changed since the previous draw of this frame (a 4 KB
  // memcmp against the cached guest bytes is far cheaper than the byte-swapped
  // copy into write-combined memory it replaces).
  UploadRing& ring = g.upload[g.frame_index];
  if (g.bank_cache_frame != g.frame_number) {
    g.bank_cache_frame = g.frame_number;
    g.bank_cache_valid = false;  // the previous frame's ring is being reused
  }
  const uint8_t* vs_guest = Mem()->TranslateVirtual(g_device + kDevVsConstants);
  const uint8_t* ps_guest = Mem()->TranslateVirtual(g_device + kDevPsConstants);
  const bool vs_changed = !g.bank_cache_valid || std::memcmp(vs_guest, g.vs_bank_cache, 4096) != 0;
  const bool ps_changed = !g.bank_cache_valid || std::memcmp(ps_guest, g.ps_bank_cache, 4096) != 0;
  if (vs_changed) {
    const size_t vs_off = ring.Allocate(4096, 256);
    if (vs_off == SIZE_MAX) return SkipLog("upload ring full (constants)");
    rex::memory::copy_and_swap_32_unaligned(ring.mapped + vs_off, vs_guest, 1024);
    std::memcpy(g.vs_bank_cache, vs_guest, 4096);
    g.vs_bank_gpu = ring.buffer->GetGPUVirtualAddress() + vs_off;
    g.stats.upload_bytes += 4096;
    ++g.const_uploads;
  } else {
    ++g.const_reuses;
  }
  if (ps_changed) {
    const size_t ps_off = ring.Allocate(4096, 256);
    if (ps_off == SIZE_MAX) return SkipLog("upload ring full (constants)");
    rex::memory::copy_and_swap_32_unaligned(ring.mapped + ps_off, ps_guest, 1024);
    std::memcpy(g.ps_bank_cache, ps_guest, 4096);
    g.ps_bank_gpu = ring.buffer->GetGPUVirtualAddress() + ps_off;
    g.stats.upload_bytes += 4096;
    ++g.const_uploads;
  } else {
    ++g.const_reuses;
  }
  g.bank_cache_valid = true;
  const size_t sh_off = ring.Allocate(kSharedConstantsBytes, 256);
  if (sh_off == SIZE_MAX) return SkipLog("upload ring full (constants)");
  std::memcpy(ring.mapped + sh_off, shared, kSharedConstantsBytes);
  g.stats.upload_bytes += kSharedConstantsBytes;

  // Pipeline.
  PipelineKey key = {};
  key.vs = vs->hash;
  key.ps = ps->hash;
  key.decl = decl->hash;
  key.rt_format = rt ? uint32_t(rt->format) : 0;
  key.ds_format = ds ? uint32_t(ds->format) : 0;  // [NEW FABLE VERSION] DSV format of the surface (D24S8 or D32)
  key.blend = Dev(kDevRbBlendControl0);
  key.blend_enable = 0;
  key.color_mask = rt ? (Dev(kDevRbColorMask) & 0xF) : 0;
  key.depth_control = Dev(kDevRbDepthControl) & 0x76;
  key.cull = Dev(kDevPaSuScModeCntl) & 7;
  {
    // Polygon offset, the emulator's conversion (GetPreferredFacePolygonOffset +
    // GetD3D10IntegerPolygonOffset): front face unless culled, else back;
    // non-polygonal primitives use the front pair behind poly_offset_para_enable.
    const uint32_t mode = Dev(kDevPaSuScModeCntl);
    const bool polygonal = topo_type == D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    float scale = 0.0f, offset = 0.0f;
    auto devf = [&](uint32_t off) { const uint32_t u = Dev(off); float f; std::memcpy(&f, &u, 4); return f; };
    if (polygonal) {
      if ((mode & (1u << 11)) && !(mode & 1u)) {
        scale = devf(kDevPaSuPolyOffsetFrontScale);
        offset = devf(kDevPaSuPolyOffsetFrontOffset);
      }
      if ((mode & (1u << 12)) && !(mode & 2u) && scale == 0.0f && offset == 0.0f) {
        scale = devf(kDevPaSuPolyOffsetBackScale);
        offset = devf(kDevPaSuPolyOffsetBackOffset);
      }
    } else if (mode & (1u << 13)) {
      scale = devf(kDevPaSuPolyOffsetFrontScale);
      offset = devf(kDevPaSuPolyOffsetFrontOffset);
    }
    const bool float24 = ds && (ds->guest_format & 0x3F) == 23;  // k_24_8_FLOAT
    int32_t bias = int32_t(std::ceil(std::abs(offset) * (float24 ? float(1u << 24) * (1.0f / 8.0f) : float((1u << 24) - 1))));
    if (float24) bias <<= 3;
    key.depth_bias = ds ? (offset < 0 ? -bias : bias) : 0;
    const float slope = ds ? scale * (1.0f / 16.0f) : 0.0f;
    std::memcpy(&key.slope_bias_bits, &slope, 4);
  }
  key.topology = topo_type;
  key.strip_cut = strip_cut ? (ibv.Format == DXGI_FORMAT_R32_UINT ? 2 : 1) : 0;
  key.strides[0] = strides[0];
  key.strides[1] = strides[1];
  key.instanced = instanced ? 1u : 0u;
  ID3D12PipelineState* pso = GetPipeline(key, *vs, *ps, *decl);
  if (!pso) return SkipLog("pipeline unavailable");

  if (pso != st.bound_pso) {
    g.list->SetPipelineState(pso);
    st.bound_pso = pso;
  }
  if (!st.root_bound) {  // [NEW FABLE VERSION] once per list / after a blit
    g.list->SetGraphicsRootSignature(g.root_signature.Get());
    g.list->SetGraphicsRootDescriptorTable(3, g.views.Gpu(0));
    g.list->SetGraphicsRootDescriptorTable(4, g.samplers.Gpu(0));
    st.root_bound = true;
  }
  g.list->SetGraphicsRootConstantBufferView(0, g.vs_bank_gpu);
  g.list->SetGraphicsRootConstantBufferView(1, g.ps_bank_gpu);
  g.list->SetGraphicsRootConstantBufferView(2, ring.buffer->GetGPUVirtualAddress() + sh_off);
  g.list->IASetPrimitiveTopology(topo);
  g.list->IASetVertexBuffers(0, vbv_count, vbv);
  if (GpuMarkersOn()) {  // [new_fix_24092026]
    GpuMarker(fmt::format("draw {} vs {:016X} ps {:016X} rt {:08X} ds {:08X} prim {}", g.stats.draws, vs->hash,
                          ps->hash, st.rt0, st.ds, uint32_t(topo)));
  }
  if (indexed) {
    g.list->IASetIndexBuffer(&ibv);
    g.list->DrawIndexedInstanced(index_count, instance_count, start_index, base_vertex, start_instance);
  } else {
    g.list->DrawInstanced(a.count, 1, a.start_vertex, 0);
  }
  g.stats.draws++;
  if (rt && (Dev(kDevRbColorMask) & 0xF)) rt->write_seq = ++g_edram_seq;  // [NEW FABLE VERSION] EDRAM writer order
  if (g_dump_active && rt) {
    static uint64_t watch_ps = 0; static bool parsed = false;
    if (!parsed) { parsed = true; const std::string v = REXCVAR_GET(dp_native_dump_ps); if (!v.empty()) watch_ps = std::strtoull(v.c_str(), nullptr, 16); }
    static uint32_t budget_frame = UINT32_MAX; static uint32_t budget = 0;
    if (budget_frame != g.frame_number) { budget_frame = g.frame_number; budget = 48; }
    if (watch_ps && ps->hash == watch_ps && budget) {
      --budget;
      // the draw just issued (g_dump_seq already advanced past it)
      RecordDump(fmt::format("d{:04}", g_dump_seq ? g_dump_seq - 1 : 0).c_str(), st.rt0, rt->resource.Get(), rt->state, rt->hw(), rt->hh(), rt->format);
    }
  }
}

// Full-screen copy of `src_srv` into `dst_rtv` (dst must be in RENDER_TARGET state).
void Blit(uint32_t src_srv, D3D12_CPU_DESCRIPTOR_HANDLE dst_rtv, DXGI_FORMAT dst_format, uint32_t width,
          uint32_t height, uint32_t mode = 0) {
  ID3D12PipelineState* pso = BlitPso(dst_format);
  if (!pso) return;
  g.list->OMSetRenderTargets(1, &dst_rtv, FALSE, nullptr);
  D3D12_VIEWPORT vp = {0, 0, float(width), float(height), 0, 1};
  D3D12_RECT sc = {0, 0, LONG(width), LONG(height)};
  g.list->RSSetViewports(1, &vp);
  g.list->RSSetScissorRects(1, &sc);
  g.list->SetPipelineState(pso);
  g.list->SetGraphicsRootSignature(g.blit_root_signature.Get());
  g.list->SetGraphicsRootDescriptorTable(0, g.views.Gpu(src_srv));
  // [new_fix_24092026] t1 is read only in mode 4; any valid 2D SRV fills it otherwise.
  g.list->SetGraphicsRootDescriptorTable(2, g.views.Gpu(g_gamma_lut_srv != UINT32_MAX ? g_gamma_lut_srv : src_srv));
  g.list->SetGraphicsRoot32BitConstant(1, mode, 0);
  g.list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  if (GpuMarkersOn()) {  // [new_fix_24092026]
    GpuMarker(fmt::format("blit mode {} src srv {} lut srv {} {}x{} fmt {}", mode, src_srv, g_gamma_lut_srv, width,
                          height, uint32_t(dst_format)));
  }
  g.list->DrawInstanced(3, 1, 0, 0);
  // Invalidate the game-side mirror: the next draw rebinds targets/PSO.
  st.bound_rt0 = st.bound_ds = UINTPTR_MAX;  // [new_fix_24092026] 2.0.3
  st.bound_pso = nullptr;
  st.root_bound = false;  // [NEW FABLE VERSION] the blit used its own root signature
}

// [NEW FABLE VERSION] 2026-09-23: see dp_native_edram_alias. Called before a
// colour target is drawn to (or resolved); cheap when this surface is the
// newest EDRAM writer, which is every draw after the first of a pass.
void SyncEdramAlias(GuestSurface& s) {
  if (s.depth || !s.resource || !REXCVAR_GET(dp_native_edram_alias)) return;
  if (s.write_seq == g_edram_seq) return;
  // [NEW FABLE VERSION] 2.0.1: scanned since the last EDRAM write: same answer.
  if (s.alias_checked_seq == g_edram_seq) return;
  s.alias_checked_seq = g_edram_seq;
  const uint32_t span = EdramTileSpan(s.width, s.height, SurfaceIs64bpp(s.guest_format));
  // [NEW FABLE VERSION] 2.0.1: the registry lock is held until the transfer is
  // recorded, so OnRelease on another thread cannot free `src` in between
  // (nothing below takes the lock again: Barrier, Blit and the log do not).
  std::lock_guard<std::mutex> lock(g_registry_mutex);
  GuestSurface* src = nullptr;
  for (auto& entry : g_surfaces) {
    GuestSurface* o = entry.second.get();
    if (o == &s || o->depth || o->write_seq <= s.write_seq) continue;
    if (!EdramOverlap(s.edram_base, span, o->edram_base,
                      EdramTileSpan(o->width, o->height, SurfaceIs64bpp(o->guest_format)))) {
      continue;
    }
    if (!src || o->write_seq > src->write_seq) src = o;
  }
  if (!src) return;
  const bool same_geometry = src->resource && src->edram_base == s.edram_base && src->width == s.width &&
                             src->height == s.height && src->scale == s.scale;
  const EdramTransfer kind = EdramTransferKind(src->guest_format, s.guest_format, same_geometry);
  if (kind == EdramTransfer::kNone) {
    static uint32_t skip_log = 12;
    if (skip_log) {
      --skip_log;
      REXLOG_INFO("Native renderer: EDRAM tiles of surface {:08X} ({}x{} fmt {:08X} base {}) were last written by "
                  "{:08X} ({}x{} fmt {:08X} base {}): no exact transfer, the target keeps its own content",
                  s.guest, s.width, s.height, s.guest_format, s.edram_base, src->guest, src->width, src->height,
                  src->guest_format, src->edram_base);
    }
    return;
  }
  Barrier(src->resource.Get(), src->state,
          D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  Barrier(s.resource.Get(), s.state, D3D12_RESOURCE_STATE_RENDER_TARGET);
  // 7e3 -> 8888: the stored 7e3 value, saturated by the UNORM target (the SDK's
  // DP1 transfer: saturate(7e3 float) * 255 + 0.5).
  const uint32_t mode = (kind == EdramTransfer::kSaturate7e3 && REXCVAR_GET(dp_native_7e3_resolve)) ? 1u : 0u;
  Blit(src->srv_index, g.rtvs.Cpu(s.view_index), s.format, s.hw(), s.hh(), mode);
  s.write_seq = ++g_edram_seq;
  g.stats.edram_transfers++;
  static uint32_t transfer_log = 6;
  if (transfer_log) {
    --transfer_log;
    REXLOG_INFO("Native renderer: EDRAM transfer {:08X} (fmt {:08X}) -> {:08X} (fmt {:08X}) base {} {}x{} ({})",
                src->guest, src->guest_format, s.guest, s.guest_format, s.edram_base, s.width, s.height,
                kind == EdramTransfer::kSaturate7e3 ? "7e3 -> 8888 saturate" : "copy");
  }
}

}  // namespace

namespace {

// ---------------------------------------------------------------------------
// Frame dump (dp_native_dump_frame): readback of RGBA8/BGRA8/R16F/R32F textures.
// ---------------------------------------------------------------------------
struct DumpItem {
  std::string name;
  ComPtr<ID3D12Resource> readback;
  uint32_t width, height, pitch;
  DXGI_FORMAT format;
};
std::vector<DumpItem> g_dump_items;

void RecordDump(const char* what, uint32_t guest, ID3D12Resource* res, D3D12_RESOURCE_STATES& state, uint32_t w,
                uint32_t h, DXGI_FORMAT format, uint32_t slice, uint32_t slices) {
  uint32_t bpp = 4;
  if (format == DXGI_FORMAT_R16G16B16A16_FLOAT) bpp = 8;
  else if (format == DXGI_FORMAT_R32_FLOAT) bpp = 4;
  else if (format == DXGI_FORMAT_R8_UNORM) bpp = 1;
  else if (format != DXGI_FORMAT_R8G8B8A8_UNORM && format != DXGI_FORMAT_B8G8R8A8_UNORM) return;
  const uint32_t pitch = (w * bpp + 255) & ~255u;
  D3D12_RESOURCE_DESC desc = {};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  desc.Width = UINT64(pitch) * h;
  desc.Height = 1;
  desc.DepthOrArraySize = 1;
  desc.MipLevels = 1;
  desc.SampleDesc.Count = 1;
  desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  D3D12_HEAP_PROPERTIES heap = {D3D12_HEAP_TYPE_READBACK};
  DumpItem item;
  if (FAILED(g.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                               IID_PPV_ARGS(&item.readback)))) {
    return;
  }
  item.readback->SetName(L"DP1 dump readback");  // [new_fix_24092026] 2.0.3
  const D3D12_RESOURCE_STATES before = state;
  Barrier(res, state, D3D12_RESOURCE_STATE_COPY_SOURCE);
  D3D12_TEXTURE_COPY_LOCATION dst = {}, src = {};
  dst.pResource = item.readback.Get();
  dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
  dst.PlacedFootprint.Footprint = {format, w, h, 1, pitch};
  src.pResource = res;
  src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  if (slices > 1) {
    // Texture3D (volume or stacked array on the host): one depth slice.
    D3D12_BOX box = {0, 0, slice, w, h, slice + 1};
    g.list->CopyTextureRegion(&dst, 0, 0, 0, &src, &box);
  } else {
    g.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
  }
  Barrier(res, state, before == D3D12_RESOURCE_STATE_COPY_SOURCE ? D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE : before);
  item.name = slices > 1 ? fmt::format("{}_{:08X}_z{}_{}x{}", what, guest, slice, w, h) : fmt::format("{}_{:08X}_{}x{}", what, guest, w, h);
  item.width = w;
  item.height = h;
  item.pitch = pitch;
  item.format = format;
  g_dump_items.push_back(std::move(item));
}

void WriteDumps(uint32_t frame) {
  if (g_dump_items.empty()) return;
  const UINT64 last = g.fences.LastSubmitted();  // [new_fix_24092026] 2.0.3
  if (g.fence->GetCompletedValue() < last) {
    g.fence->SetEventOnCompletion(last, g.fence_event);
    WaitForSingleObject(g.fence_event, INFINITE);
  }
  std::filesystem::path dir = std::filesystem::current_path() / "logs" / "native_dump";
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  for (auto& item : g_dump_items) {
    uint8_t* data = nullptr;
    D3D12_RANGE range = {0, SIZE_T(item.pitch) * item.height};
    if (FAILED(item.readback->Map(0, &range, reinterpret_cast<void**>(&data)))) continue;
    if (item.format == DXGI_FORMAT_R32_FLOAT || item.format == DXGI_FORMAT_R16G16B16A16_FLOAT) {
      // Raw rows as well: the .ppm clamps to 0..1 and HDR / log-luminance values matter.
      std::ofstream raw(dir / fmt::format("f{}_{}.raw", frame, item.name), std::ios::binary);
      const uint32_t bpp = item.format == DXGI_FORMAT_R32_FLOAT ? 4 : 8;
      for (uint32_t y = 0; y < item.height; ++y) {
        raw.write(reinterpret_cast<const char*>(data + size_t(y) * item.pitch), size_t(item.width) * bpp);
      }
    }
    std::ofstream out(dir / fmt::format("f{}_{}.ppm", frame, item.name), std::ios::binary);
    out << "P6\n" << item.width << " " << item.height << "\n255\n";
    std::vector<uint8_t> row(item.width * 3);
    for (uint32_t y = 0; y < item.height; ++y) {
      const uint8_t* src = data + size_t(y) * item.pitch;
      for (uint32_t x = 0; x < item.width; ++x) {
        uint8_t r = 0, gg = 0, b = 0;
        switch (item.format) {
          case DXGI_FORMAT_R8G8B8A8_UNORM: r = src[x * 4]; gg = src[x * 4 + 1]; b = src[x * 4 + 2]; break;
          case DXGI_FORMAT_B8G8R8A8_UNORM: b = src[x * 4]; gg = src[x * 4 + 1]; r = src[x * 4 + 2]; break;
          case DXGI_FORMAT_R8_UNORM: r = gg = b = src[x]; break;
          case DXGI_FORMAT_R32_FLOAT: {
            float f; std::memcpy(&f, src + x * 4, 4);
            r = gg = b = uint8_t(std::clamp(f, 0.0f, 1.0f) * 255.0f); break;
          }
          case DXGI_FORMAT_R16G16B16A16_FLOAT: {
            auto h2f = [](uint16_t hv) {
              const uint32_t e = (hv >> 10) & 0x1F, m = hv & 0x3FF, sgn = (hv >> 15) & 1;
              float f = e == 0 ? std::ldexp(float(m), -24) : e == 31 ? 1e9f : std::ldexp(float(m + 1024), int(e) - 25);
              return sgn ? -f : f;
            };
            const uint16_t* px = reinterpret_cast<const uint16_t*>(src + x * 8);
            r = uint8_t(std::clamp(h2f(px[0]), 0.0f, 1.0f) * 255.0f);
            gg = uint8_t(std::clamp(h2f(px[1]), 0.0f, 1.0f) * 255.0f);
            b = uint8_t(std::clamp(h2f(px[2]), 0.0f, 1.0f) * 255.0f);
            break;
          }
          default: break;
        }
        row[x * 3] = r; row[x * 3 + 1] = gg; row[x * 3 + 2] = b;
      }
      out.write(reinterpret_cast<const char*>(row.data()), row.size());
    }
    item.readback->Unmap(0, nullptr);
  }
  // Raw guest textures: 64-byte header (6 fetch dwords, width, height, format,
  // tiled, endian, block pitch h/v, bytes per block, faces, mip0 bytes) + data.
  uint32_t raw = 0;
  for (uint32_t guest : g_dump_textures) {
    GuestTexture* t = TextureForObject(guest);
    if (!t || !t->info_valid || !t->info.format_info()) continue;
    const rex::graphics::TextureExtent ext = t->info.GetMipExtent(0, true);
    const uint32_t bpb = t->info.format_info()->bytes_per_block();
    const uint32_t faces = t->is_cube ? 6 : 1;
    const uint32_t face_bytes = ext.block_pitch_h * ext.block_pitch_v * bpb;
    if (!face_bytes || face_bytes * size_t(faces) > (64u << 20)) continue;
    uint32_t header[16] = {};
    for (int i = 0; i < 6; ++i) header[i] = (&t->fetch.dword_0)[i];
    header[6] = t->width;
    header[7] = t->height;
    header[8] = uint32_t(t->info.format);
    header[9] = t->info.is_tiled ? 1u : 0u;
    header[10] = uint32_t(t->info.endianness);
    header[11] = ext.block_pitch_h;
    header[12] = ext.block_pitch_v;
    header[13] = bpb;
    header[14] = faces;
    header[15] = face_bytes;
    std::ofstream out(dir / fmt::format("f{}_tex_{:08X}_{}x{}_f{}.bin", frame, guest, t->width, t->height, header[8]),
                      std::ios::binary);
    out.write(reinterpret_cast<const char*>(header), sizeof(header));
    out.write(reinterpret_cast<const char*>(HeaderPtr(t->info.memory.base_address)), size_t(face_bytes) * faces);
    ++raw;
  }
  g_dump_textures.clear();
  REXLOG_INFO("Native renderer: frame {} dumped {} images and {} raw textures to {}", frame, g_dump_items.size(), raw,
              dir.string());
  g_dump_items.clear();
}

}  // namespace

// ===========================================================================
// Public entry points
// ===========================================================================

void OnCreateDevice() {
  g_device = LoadU32(kDevicePointerGlobal);
  REXLOG_INFO("Native renderer: XDK device block at {:08X}", g_device);
}

void OnCreateTexture(PPCContext& /*ctx*/, uint32_t texture) {
  if (!texture) return;
  // Objects are views: the host texture is created on first use from the fetch
  // constant in force at that moment. Creating it here would freeze the base
  // address the game later re-points.
  (void)TextureForObject(texture);
}

void OnCreateSurface(uint32_t width, uint32_t height, uint32_t format, uint32_t /*multisample*/, uint32_t params,
                     uint32_t surface) {
  if (!surface) return;
  auto s = std::make_unique<GuestSurface>();
  s->guest = surface;
  s->width = width;
  s->height = height;
  s->guest_format = format;
  s->edram_base = params ? LoadU32(params) : 0;
  const SurfaceFormat f = MapSurfaceFormat(format);
  s->format = f.format;
  s->depth = f.depth;
  if (!IsKnownSurfaceFormat(format)) {  // [NEW FABLE VERSION] a silent RGBA8 fallback hid k_16_16_16_16_FLOAT
    static uint32_t unknown_log = 8;
    if (unknown_log) {
      --unknown_log;
      REXLOG_WARN("Native renderer: surface {:08X} {}x{} guest format {:08X} (texture format {}) has no host mapping, using RGBA8",
                  surface, width, height, format, format & 0x3F);
    }
  }
  REXLOG_INFO("Native renderer: surface {:08X} {}x{} format {:08X} ({}) edram base {}", surface, width, height, format,
              f.depth ? "depth" : "colour", s->edram_base);
  std::lock_guard<std::mutex> lock(g_registry_mutex);
  // [new_fix_24092026] 2.0.3: a surface object re-created at the address of one
  // we never saw released: its host target can still be in use by a submitted
  // frame, so it is retired like a Release instead of destroyed here.
  if (auto it = g_surfaces.find(surface); it != g_surfaces.end()) {
    g_released_surfaces.push_back(std::move(it->second));  // torn down at the next frame start
    g_recreated_count.fetch_add(1, std::memory_order_relaxed);
  }
  g_surfaces[surface] = std::move(s);
}

static void RegisterBuffer(uint32_t guest, bool index) {
  if (!guest) return;
  auto b = std::make_unique<GuestBuffer>();
  b->guest = guest;
  b->index = index;
  RefreshBufferHeader(*b);  // see the header layout note there
  b->dirty = true;
  static uint32_t buffer_log = 12;
  if (buffer_log) {
    --buffer_log;
    REXLOG_INFO("Native renderer: {} buffer {:08X}: address {:08X} size {} endian {} index32 {}", index ? "index" : "vertex",
                guest, b->address, b->size, b->endian, b->index32);
  }
  std::lock_guard<std::mutex> lock(g_registry_mutex);
  // [new_fix_24092026] 2.0.3: as OnCreateSurface (the old entry, its resource
  // and its page watch are torn down at the next frame start).
  if (auto it = g_buffers.find(guest); it != g_buffers.end()) {
    g_released_buffers.push_back(std::move(it->second));
    g_recreated_count.fetch_add(1, std::memory_order_relaxed);
  }
  g_buffers[guest] = std::move(b);
}
void OnCreateVertexBuffer(uint32_t vb) { RegisterBuffer(vb, false); }
void OnCreateIndexBuffer(uint32_t ib) { RegisterBuffer(ib, true); }

void OnCreateVertexDeclaration(uint32_t elements, uint32_t decl) {
  if (!decl || !elements) return;
  auto d = std::make_unique<GuestDecl>();
  d->guest = decl;
  // Xbox 360 D3DVERTEXELEMENT9 = 12 bytes: u16 stream, u16 offset, u32 type
  // (GPU encoding), u8 method, u8 usage, u8 usageIndex; terminator stream 0xFF.
  for (uint32_t i = 0; i < 32; ++i) {
    const uint32_t e = elements + i * 12;
    const uint32_t w0 = LoadU32(e), w1 = LoadU32(e + 4), w2 = LoadU32(e + 8);
    const uint16_t stream = uint16_t(w0 >> 16);
    if (stream == 0xFF) break;
    GuestDecl::Element el;
    el.stream = stream;
    el.offset = uint16_t(w0 & 0xFFFF);
    el.type = DecodeDeclType(w1);
    el.method = uint8_t(w2 >> 24);
    el.usage = uint8_t(w2 >> 16);
    el.usage_index = uint8_t(w2 >> 8);
    d->elements.push_back(el);
  }
  for (const auto& el : d->elements) {
    if (el.usage >= 14 || el.stream >= 2) continue;
    D3D12_INPUT_ELEMENT_DESC ie = {};
    ie.SemanticName = kUsageSemantics[el.usage];
    ie.SemanticIndex = el.usage_index;
    ie.Format = MapDeclType(el.type, el.usage);
    ie.InputSlot = el.stream;
    ie.AlignedByteOffset = el.offset;
    ie.InputSlotClass = D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;
    if (ie.Format == DXGI_FORMAT_UNKNOWN) continue;
    // Duplicate (usage, index) across streams: keep the first.
    bool dup = false;
    for (const auto& o : d->layout) {
      if (o.SemanticIndex == ie.SemanticIndex && std::strcmp(o.SemanticName, ie.SemanticName) == 0) dup = true;
    }
    if (dup) continue;
    d->layout.push_back(ie);
    switch (el.usage) {
      case kTexCoord:
        if (el.type == kFloat16_2 || el.type == kFloat16_4 || el.type == kShort2 || el.type == kShort4 ||
            el.type == kShort2N || el.type == kShort4N || el.type == kUShort2N || el.type == kUShort4N) {
          d->swapped_texcoords |= 1u << el.usage_index;
        }
        break;
      case kNormal:
      case kTangent:
      case kBinormal:
        if (el.type == kFloat16_4) d->normal_mode = 20;
        else if (el.type == kFloat16_2) d->normal_mode = 21;
        break;
      case kBlendIndices:
      case kBlendWeight: {
        // After the per-dword byte swap the 8-bit lanes are exactly what the
        // 8in32 vertex fetch hands the ucode (x = last memory byte), and the
        // recompiled shader already applies the vfetch's own swizzle (.zyxw
        // for D3DCOLOR): no extra swap for either type. (Mode 2 here double-
        // swizzled the bone indices: every vertex used bone 0 = T-pose.)
        break;
      }
      default:
        break;
    }
  }
  // Every semantic a DP1 vertex shader can declare (18 input signatures over
  // 565 shaders, H_shader_feature_audit.tsv) gets a dummy element on the
  // never-bound slot 15 when the declaration lacks it, so one recompiled VS
  // binds to any declaration (reads yield zeros). Same trick as the references.
  {
    struct Dummy { const char* name; uint8_t index; bool uint_lanes; };
    static const Dummy kDummies[] = {{"POSITION", 0, false},     {"TEXCOORD", 0, false},     {"TEXCOORD", 1, false},
                                     {"COLOR", 0, false},        {"NORMAL", 0, true},        {"TANGENT", 0, true},
                                     {"BINORMAL", 0, true},      {"BLENDINDICES", 0, true},  {"BLENDWEIGHT", 0, true}};
    for (const Dummy& dm : kDummies) {
      bool present = false;
      for (const auto& o : d->layout) {
        if (o.SemanticIndex == dm.index && std::strcmp(o.SemanticName, dm.name) == 0) present = true;
      }
      if (present) continue;
      D3D12_INPUT_ELEMENT_DESC ie = {};
      ie.SemanticName = dm.name;
      ie.SemanticIndex = dm.index;
      ie.Format = dm.uint_lanes ? DXGI_FORMAT_R32_UINT : DXGI_FORMAT_R32_FLOAT;
      ie.InputSlot = 15;
      ie.AlignedByteOffset = 0;
      ie.InputSlotClass = D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;
      d->layout.push_back(ie);
    }
  }
  d->hash = XXH3_64bits(d->elements.data(), d->elements.size() * sizeof(GuestDecl::Element));
  REXLOG_INFO("Native renderer: vertex declaration {:08X}: {} elements, {} in layout, texcoord swap {:X}, normal mode {}, "
              "blend mode {}",
              decl, d->elements.size(), d->layout.size(), d->swapped_texcoords, d->normal_mode, d->swapped_blend);
  std::lock_guard<std::mutex> lock(g_registry_mutex);
  g_decls[decl] = std::move(d);
}

// Xenos vertex format -> the declaration type our MapDeclType understands.
uint8_t VfetchDeclType(uint32_t format, bool is_signed, bool integer) {
  switch (format) {
    case 6: return integer ? 5 /*UBYTE4*/ : 4 /*D3DCOLOR*/;        // k_8_8_8_8
    case 25: return integer ? 6 /*SHORT2*/ : (is_signed ? 9 /*SHORT2N*/ : 11 /*USHORT2N*/);   // k_16_16
    case 26: return integer ? 7 /*SHORT4*/ : (is_signed ? 10 /*SHORT4N*/ : 12 /*USHORT4N*/);  // k_16_16_16_16
    case 31: return 15;  // k_16_16_FLOAT
    case 32: return 16;  // k_16_16_16_16_FLOAT
    case 36: return 0;   // k_32_FLOAT
    case 37: return 1;   // k_32_32_FLOAT
    case 57: return 2;   // k_32_32_32_FLOAT
    case 38: return 3;   // k_32_32_32_32_FLOAT
    case 7: return 14;   // k_2_10_10_10 -> DEC3N
    case 4: return 14;   // k_11_11_10 (HEND3N) -> handled as a uint lane by the shader
    default: return 17;
  }
}

// Walk the vertex shader's control flow and collect its vertex fetches. The
// container layout and instruction bitfields are XenosRecomp's (shader.h,
// shader_code.h); the vertex element table maps a fetch's instruction
// address to its usage.
void ScanVertexFetches(const uint8_t* base, size_t size, std::vector<UcodeVfetch>& out) {
  auto be32 = [&](size_t off) -> uint32_t {
    if (off + 4 > size) return 0;
    return (uint32_t(base[off]) << 24) | (uint32_t(base[off + 1]) << 16) | (uint32_t(base[off + 2]) << 8) | base[off + 3];
  };
  const uint32_t flags = be32(0);
  if ((flags & 0xFFFFFF00u) != 0x102A1100u || (flags & 1u) == 0) return;  // not a vertex shader container
  const uint32_t virtual_size = be32(4), shader_offset = be32(24);
  if (!shader_offset || shader_offset + 36 > size) return;
  const uint32_t physical_offset = be32(shader_offset), cf_bytes = be32(shader_offset + 4);
  const uint32_t field18 = be32(shader_offset + 24), element_count = be32(shader_offset + 28);
  std::unordered_map<uint32_t, std::pair<uint8_t, uint8_t>> usage_by_address;
  for (uint32_t i = 0; i < element_count && i < 64; ++i) {
    const uint32_t v = be32(shader_offset + 36 + (field18 + i) * 4);
    usage_by_address[v & 0xFFF] = {uint8_t((v >> 12) & 0xF), uint8_t((v >> 16) & 0xF)};
  }
  const size_t code = size_t(virtual_size) + physical_offset;
  if (code >= size) return;
  std::unordered_map<uint32_t, bool> seen;
  uint32_t last_const = 0, last_stride = 0;
  for (uint32_t cf_off = 0; cf_off + 12 <= cf_bytes && code + cf_off + 12 <= size; cf_off += 12) {
    const uint32_t d0 = be32(code + cf_off), d1 = be32(code + cf_off + 4), d2 = be32(code + cf_off + 8);
    const uint32_t lo[2] = {d0, (d1 >> 16) | (d2 << 16)}, hi[2] = {d1 & 0xFFFF, d2 >> 16};
    for (int k = 0; k < 2; ++k) {
      const uint32_t opcode = (hi[k] >> 12) & 0xF;
      const bool exec = opcode == 1 || opcode == 2 || opcode == 3 || opcode == 4 || opcode == 5 || opcode == 6 || opcode == 13 || opcode == 14;
      if (!exec) continue;
      const uint32_t address = lo[k] & 0xFFF, count = (lo[k] >> 12) & 7, sequence = (lo[k] >> 16) & 0xFFF;
      for (uint32_t i = 0; i < count; ++i) {
        if (((sequence >> (2 * i)) & 1) == 0) continue;  // ALU
        const size_t at = code + size_t(address + i) * 12;
        if (at + 12 > size) break;
        const uint32_t f0 = be32(at), f1 = be32(at + 4), f2 = be32(at + 8);
        if ((f0 & 0x1F) != 0) continue;  // not a vertex fetch
        const bool mini = (f1 >> 30) & 1;
        if (!mini) {
          last_const = ((f0 >> 20) & 0x1F) * 3 + ((f0 >> 25) & 3);
          last_stride = f2 & 0xFF;
        }
        const uint32_t format = (f1 >> 16) & 0x3F;
        const bool is_signed = (f1 >> 12) & 1;
        const bool integer = (f1 >> 13) & 1;
        const uint32_t offset_dwords = (f2 >> 8) & 0x7FFFFF;
        auto it = usage_by_address.find(address + i);
        if (it == usage_by_address.end()) continue;
        const uint32_t key = (uint32_t(it->second.first) << 8) | it->second.second;
        if (seen.count(key)) continue;
        seen[key] = true;
        UcodeVfetch v;
        v.usage = it->second.first;
        v.usage_index = it->second.second;
        v.stream = uint8_t(last_const <= 95 ? 95 - last_const : 255);
        v.decl_type = VfetchDeclType(format, is_signed, integer);
        v.offset = offset_dwords * 4;
        v.stride = last_stride * 4;
        out.push_back(v);
      }
    }
  }
}

void OnCreateShader(uint32_t container, uint32_t shader, bool pixel) {
  if (!container || !shader) return;
  g_shader_cache.Load();
  auto s = std::make_unique<GuestShader>();
  s->guest = shader;
  s->pixel = pixel;
  const uint32_t virtual_size = LoadU32(container + 4), physical_size = LoadU32(container + 8);
  const size_t size = size_t(virtual_size) + physical_size;
  if (size > 0 && size < 0x100000) {
    s->container = container;
    s->container_size = uint32_t(size);
    s->hash = XXH3_64bits(Mem()->TranslateVirtual(container), size);
    if (!pixel) {
      ScanVertexFetches(Mem()->TranslateVirtual(container), size, s->vfetches);
      if (g.log_budget > 0 && !s->vfetches.empty()) {
        std::string list;
        for (const auto& v : s->vfetches) {
          list += fmt::format(" {}{}:s{}+{}/t{}/st{}", v.usage < 14 ? kUsageSemantics[v.usage] : "?", v.usage_index, v.stream, v.offset, v.decl_type, v.stride);
        }
        REXLOG_INFO("Native renderer: vertex shader {:016X} fetches:{}", s->hash, list);
      }
    }
    s->entry = g_shader_cache.Find(s->hash);
  }
  if (!s->entry && g.log_budget) {
    --g.log_budget;
    REXLOG_WARN("Native renderer: {} shader {:08X} (container {:08X}, {} bytes, hash {:016X}) not in the cache", pixel ? "pixel" : "vertex",
                shader, container, size, s->hash);
  }
  std::lock_guard<std::mutex> lock(g_registry_mutex);
  g_shaders[shader] = std::move(s);
}

void OnRelease(uint32_t object, uint32_t refcount_before) {
  if (refcount_before != 1) return;
  std::lock_guard<std::mutex> lock(g_registry_mutex);
  g_object_key.erase(object);
  if (auto it = g_textures.find(object); it != g_textures.end()) {
    ForgetWatch(it->second.get());
    RetireDescriptor(g.views, it->second->srv_index);  // [new_fix_24092026] 2.0.3
    g.rtvs.Free(it->second->rtv_index);
    for (uint32_t r : it->second->slice_rtvs) g.rtvs.Free(r);
    RetireResource(std::move(it->second->resource));
    g_textures.erase(it);
    return;
  }
  // [new_fix_24092026] 2.0.3: unlinked here, torn down at the next frame start
  // (DestroyReleasedObjects): this can be a loading thread, and the recording
  // thread may be using the object right now.
  if (auto it = g_buffers.find(object); it != g_buffers.end()) {
    g_released_buffers.push_back(std::move(it->second));
    g_buffers.erase(it);
    return;
  }
  if (auto it = g_surfaces.find(object); it != g_surfaces.end()) {
    g_released_surfaces.push_back(std::move(it->second));
    g_surfaces.erase(it);
    return;
  }
  g_shaders.erase(object);
  g_decls.erase(object);
}

void OnUnlock(uint32_t object) {
  // Buffers first: a vertex buffer header's dword at +28 (size|endian) can pass
  // the fetch-type test and Prepare() divides by zero on the garbage.
  if (GuestBuffer* b = Lookup(g_buffers, object)) { b->dirty = true; return; }
  bool known;
  {
    std::lock_guard<std::mutex> lock(g_registry_mutex);
    known = g_object_key.count(object) != 0;
  }
  if (known) {
    if (GuestTexture* t = TextureForObject(object)) t->dirty = true;
  }
}

void OnSetRenderTarget(uint32_t index, uint32_t surface) {
  if (index == 0) st.rt0 = surface;
}
void OnSetDepthStencilSurface(uint32_t surface) { st.ds = surface; }
void OnSetViewport(uint32_t vp) {
  if (!vp) {
    st.viewport_valid = false;
    return;
  }
  st.viewport.TopLeftX = float(int32_t(LoadU32(vp)));
  st.viewport.TopLeftY = float(int32_t(LoadU32(vp + 4)));
  st.viewport.Width = float(int32_t(LoadU32(vp + 8)));
  st.viewport.Height = float(int32_t(LoadU32(vp + 12)));
  st.viewport.MinDepth = LoadF32(vp + 16);
  st.viewport.MaxDepth = LoadF32(vp + 20);
  st.viewport_valid = true;
}
void OnSetTexture(uint32_t sampler, uint32_t texture) {
  if (sampler < 32) st.textures[sampler] = texture;
}
void OnSetStreamSource(uint32_t stream, uint32_t vb, uint32_t offset, uint32_t stride) {
  if (stream < 2) st.streams[stream] = {vb, offset, stride};
}
void OnSetIndices(uint32_t ib) { st.ib = ib; }
void OnSetVertexDeclaration(uint32_t decl) { st.decl = decl; }
void OnSetVertexShader(uint32_t vs) { st.vs = vs; }
void OnSetPixelShader(uint32_t ps) { st.ps = ps; }

void OnDrawVertices(uint32_t prim, uint32_t start_vertex, uint32_t count) {
  DrawArgs a = {};
  a.prim = prim;
  a.indexed = false;
  a.start_vertex = start_vertex;
  a.count = count;
  ExecuteDraw(a);
}
void OnDrawIndexedVertices(uint32_t prim, uint32_t base_vertex, uint32_t start_index, uint32_t count) {
  DrawArgs a = {};
  a.prim = prim;
  a.indexed = true;
  a.base_vertex = base_vertex;
  a.start_index = start_index;
  a.count = count;
  ExecuteDraw(a);
}
void OnDrawVerticesUP(uint32_t prim, uint32_t count, uint32_t data, uint32_t stride) {
  if (!data || !stride || !count) return;
  DrawArgs a = {};
  a.prim = prim;
  a.indexed = false;
  a.count = count;
  a.up_data = Mem()->TranslateVirtual(data);
  a.up_stride = stride;
  ExecuteDraw(a);
}

void OnClear(uint32_t flags, uint32_t color_ptr, float z, uint32_t stencil) {
  ScopedCpuTimer cpu_timer;  // [NEW FABLE VERSION]
  RecordScope record_scope(__func__);  // [new_fix_24092026] 2.0.3
  if (!InitContext() || !BeginFrame()) return;
  GuestSurface* rt = nullptr;
  GuestSurface* ds = nullptr;
  if (!BindTargets(&rt, &ds, (flags & 0xF) != 0)) return;
  g.stats.clears++;
  if (g_dump_active) {
    REXLOG_INFO("Native drawline #{} Clear flags {:08X} color {:08X} ({} {} {} {}) z {} stencil {} -> rt {:08X} ds {:08X}",
                g_dump_seq++, flags, color_ptr, color_ptr ? LoadF32(color_ptr) : 0.f, color_ptr ? LoadF32(color_ptr + 4) : 0.f,
                color_ptr ? LoadF32(color_ptr + 8) : 0.f, color_ptr ? LoadF32(color_ptr + 12) : 0.f, z, stencil, st.rt0, st.ds);
  }
  if (GpuMarkersOn()) GpuMarker(fmt::format("clear flags {:08X} rt {:08X} ds {:08X}", flags, st.rt0, st.ds));  // [new_fix_24092026]
  // Xbox 360 D3DCLEAR flags: bits 0-3 = TARGET0..3, 0x10 = ZBUFFER, 0x20 = STENCIL.
  if ((flags & 0xF) && rt) {
    float color[4] = {0, 0, 0, 0};
    if (color_ptr) {
      for (int i = 0; i < 4; ++i) color[i] = LoadF32(color_ptr + i * 4);
    }
    g.list->ClearRenderTargetView(g.rtvs.Cpu(rt->view_index), color, 0, nullptr);
    rt->write_seq = ++g_edram_seq;  // [NEW FABLE VERSION]
  }
  if ((flags & 0x10) && ds) {
    g.list->ClearDepthStencilView(g.dsvs.Cpu(ds->view_index), D3D12_CLEAR_FLAG_DEPTH, z, 0, 0, nullptr);
  }
}

// RTV of one array slice / cube face of a resolve destination (created lazily).
static uint32_t SliceRtv(GuestTexture& t, uint32_t slice) {
  const uint32_t slices = t.is_cube ? 6 : std::max(1u, t.depth);
  if (slices <= 1) return t.rtv_index;
  if (slice >= slices) slice = 0;
  if (t.slice_rtvs.size() < slices) t.slice_rtvs.resize(slices, UINT32_MAX);
  if (t.slice_rtvs[slice] == UINT32_MAX) {
    t.slice_rtvs[slice] = g.rtvs.Allocate();
    if (t.slice_rtvs[slice] == UINT32_MAX) return UINT32_MAX;
    D3D12_RENDER_TARGET_VIEW_DESC rtv = {};
    rtv.Format = t.format;
    if (t.is_3d) {
      rtv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE3D;
      rtv.Texture3D.FirstWSlice = slice;
      rtv.Texture3D.WSize = 1;
    } else {
      rtv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
      rtv.Texture2DArray.FirstArraySlice = slice;
      rtv.Texture2DArray.ArraySize = 1;
    }
    g.device->CreateRenderTargetView(t.resource.Get(), &rtv, g.rtvs.Cpu(t.slice_rtvs[slice]));
  }
  return t.slice_rtvs[slice];
}

// D3DDevice_Resolve(dev, Flags, pSourceRect, pDestTexture, pDestPoint, DestLevel,
// DestSliceOrFace, pClearColor, ClearZ (f1), ClearStencil, pParameters).
// Flags: 0-3 = RT index, 4 = DEPTHSTENCIL, 0x70 = fragment (MSAA sample)
// select, 0x100 = CLEARRENDERTARGET, 0x200 = CLEARDEPTHSTENCIL.
// [NEW FABLE VERSION] 2026-09-23: 0x10 is D3DRESOLVE_FRAGMENT0, NOT a clear.
// Proof: the XDK body (sub_824E5DB8) ORs 0x10 / 0x50 / 0x70 into the flags when
// (flags & 0x70) == 0, picked from the surface's MSAA mode (1x / 2x / 4x), and
// Downpour's UE3 source resolves depth with DEPTHSTENCIL | FRAGMENT0 (= 0x14,
// the exact value DP1 passes). DP1 only ever passes 0x00, 0x10 and 0x14, so it
// never asks a resolve to clear: EDRAM keeps its contents, and the light pass
// depends on that (resolve -> R-only draw -> resolve must keep G = prepass
// depth, which the tonemap reads as the DoF input). Clearing on 0x10 wiped it.
void OnResolve(uint32_t flags, uint32_t rect_ptr, uint32_t dest_texture, uint32_t dest_point_ptr,
               uint32_t dest_level, uint32_t dest_slice, uint32_t clear_color_ptr, float clear_z) {
  ScopedCpuTimer cpu_timer;  // [NEW FABLE VERSION]
  RecordScope record_scope(__func__);  // [new_fix_24092026] 2.0.3
  if (!InitContext() || !BeginFrame()) return;
  GuestTexture* t = TextureForObject(dest_texture);
  const bool depth = (flags & 4) != 0;
  GuestSurface* src = Lookup(g_surfaces, depth ? st.ds : st.rt0);
  if (GpuMarkersOn()) {  // [new_fix_24092026]
    GpuMarker(fmt::format("resolve flags {:08X} src {:08X} dest texture {:08X} level {} slice {}", flags,
                          depth ? st.ds : st.rt0, dest_texture, dest_level, dest_slice));
  }
  // [NEW FABLE VERSION] the blit copies the whole surface into mip 0; count (and
  // log the first few) resolves that ask for a sub-rectangle, a destination
  // point or a mip level, so the gap is measurable instead of assumed.
  {
    bool partial = dest_level != 0;
    int32_t rc[4] = {0, 0, 0, 0}, pt[2] = {0, 0};
    if (rect_ptr) {
      for (int i = 0; i < 4; ++i) rc[i] = int32_t(LoadU32(rect_ptr + i * 4));
      if (rc[0] != 0 || rc[1] != 0 || (src && (uint32_t(rc[2]) != src->width || uint32_t(rc[3]) != src->height))) partial = true;
    }
    if (dest_point_ptr) {
      pt[0] = int32_t(LoadU32(dest_point_ptr));
      pt[1] = int32_t(LoadU32(dest_point_ptr + 4));
      if (pt[0] != 0 || pt[1] != 0) partial = true;
    }
    if (partial) {
      g.stats.resolve_partial++;
      static uint32_t partial_log = 8;
      if (partial_log) {
        --partial_log;
        REXLOG_WARN("Native renderer: partial resolve ignored: rect ({},{})-({},{}) point ({},{}) level {} dest {:08X} from {}x{}",
                    rc[0], rc[1], rc[2], rc[3], pt[0], pt[1], dest_level, dest_texture, src ? src->width : 0, src ? src->height : 0);
      }
    }
  }
  // The clears happen even when the copy cannot (e.g. no destination texture).
  auto do_clears = [&]() {
    if (ResolveClearsTarget(flags)) {  // [NEW FABLE VERSION] was flags & 0x10 (FRAGMENT0)
      if (GuestSurface* rt = Lookup(g_surfaces, st.rt0); rt && EnsureSurfaceResource(*rt)) {
        float color[4] = {0, 0, 0, 0};
        if (clear_color_ptr) for (int i = 0; i < 4; ++i) color[i] = LoadF32(clear_color_ptr + i * 4);
        Barrier(rt->resource.Get(), rt->state, D3D12_RESOURCE_STATE_RENDER_TARGET);
        g.list->ClearRenderTargetView(g.rtvs.Cpu(rt->view_index), color, 0, nullptr);
        rt->write_seq = ++g_edram_seq;  // [NEW FABLE VERSION]
        g.stats.clears++;
      }
    }
    if (ResolveClearsDepth(flags)) {  // [NEW FABLE VERSION] was flags & 0x20 (FRAGMENT1)
      if (GuestSurface* ds = Lookup(g_surfaces, st.ds); ds && EnsureSurfaceResource(*ds)) {
        Barrier(ds->resource.Get(), ds->state, D3D12_RESOURCE_STATE_DEPTH_WRITE);
        g.list->ClearDepthStencilView(g.dsvs.Cpu(ds->view_index), D3D12_CLEAR_FLAG_DEPTH, clear_z, 0, 0, nullptr);
        g.stats.clears++;
      }
    }
  };
  struct ClearsAtExit { decltype(do_clears)& f; ~ClearsAtExit() { f(); } } clears_at_exit{do_clears};
  static uint32_t resolve_log = 40;
  const uint32_t seq = g_dump_active ? g_dump_seq++ : 0;
  if (resolve_log || g_dump_active) {
    if (resolve_log) --resolve_log;
    REXLOG_INFO("Native drawline #{} Resolve flags {:08X} dest {:08X} level {} slice {} clear ({} {} {} {}) z {} (registered {}, info {}, fmt {}, {}x{}) from {} {:08X} ({})",
                seq, flags, dest_texture, dest_level, dest_slice, clear_color_ptr ? LoadF32(clear_color_ptr) : 0.f,
                clear_color_ptr ? LoadF32(clear_color_ptr + 4) : 0.f, clear_color_ptr ? LoadF32(clear_color_ptr + 8) : 0.f,
                clear_color_ptr ? LoadF32(clear_color_ptr + 12) : 0.f, clear_z, t != nullptr, t && t->info_valid,
                t ? uint32_t(t->format) : 0, t ? t->width : 0, t ? t->height : 0, depth ? "DS" : "RT0", depth ? st.ds : st.rt0,
                src && src->resource ? "host ok" : "no host");
  }
  if (!t) return;
  // [NEW FABLE VERSION] a colour target resolved right after being re-bound over
  // tiles another target wrote (no draw in between) resolves those tiles.
  if (src && !depth && src->resource) SyncEdramAlias(*src);
  if (!src || !src->resource) return;
  // [NEW FABLE VERSION] a replaced texture becoming a resolve destination gets
  // a guest-format resource again (the PNG's RGBA8 size would not match).
  if (t->replaced) DropHostTexture(*t);
  if (!t->is_resolve_target && t->resource && t->srv_index != UINT32_MAX && !t->is_cube && (t->depth <= 1 || t->is_3d)) {
    // Was sampled as a CPU texture before: the blit writes logical RGBA, drop the fetch swizzle.
    t->is_resolve_target = true;
    D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
    srv.Format = t->format;
    srv.Shader4ComponentMapping = SrvMapping(*t);
    if (t->is_3d) {
      srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
      srv.Texture3D.MipLevels = 1;
    } else {
      srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
      srv.Texture2D.MipLevels = 1;
    }
    // [new_fix_24092026] 2.0.3: a new slot; the old one can still be read by a
    // submitted frame (native_retire.h). A full heap keeps the 2.0.2 rewrite.
    const uint32_t fresh = g.views.Allocate();
    if (fresh != UINT32_MAX) {
      g.device->CreateShaderResourceView(t->resource.Get(), &srv, g.views.Cpu(fresh));
      RetireDescriptor(g.views, t->srv_index);
      t->srv_index = fresh;
    } else {
      g.device->CreateShaderResourceView(t->resource.Get(), &srv, g.views.Cpu(t->srv_index));
    }
  }
  t->is_resolve_target = true;
  t->dirty = false;
  // [NEW FABLE VERSION] a texture that was uploaded from guest memory before (its
  // resource carries the guest size and a mip chain) and is now a resolve
  // destination is recreated: the blit writes one level at the host scale, and
  // the mips below it would otherwise keep stale CPU content forever.
  if (t->resource && (t->mip_levels > 1 || t->scale != ScaleFor(t->width, t->height))) {
    ForgetWatch(t);
    RetireDescriptor(g.views, t->srv_index);  // [new_fix_24092026] 2.0.3
    g.rtvs.Free(t->rtv_index);
    for (uint32_t r : t->slice_rtvs) g.rtvs.Free(r);
    t->slice_rtvs.clear();
    t->srv_index = UINT32_MAX;
    t->rtv_index = UINT32_MAX;
    RetireResource(std::move(t->resource));
    t->resource.Reset();
    t->state = D3D12_RESOURCE_STATE_COMMON;
    t->watched = false;
  }
  const bool block_compressed = t->format == DXGI_FORMAT_BC1_UNORM || t->format == DXGI_FORMAT_BC2_UNORM ||
                                t->format == DXGI_FORMAT_BC3_UNORM;
  if (!EnsureTextureResource(*t) || block_compressed ||
      (!t->is_cube && t->depth <= 1 && !EnsureTextureRtv(*t))) {  // [new_fix_24092026] 2.0.3: RTV on first resolve
    if (g.log_budget) {
      --g.log_budget;
      REXLOG_WARN("Native renderer: resolve destination {:08X} has no host RTV (format {})", dest_texture, uint32_t(t->format));
    }
    return;
  }
  const uint32_t rtv = SliceRtv(*t, dest_slice);
  if (rtv == UINT32_MAX) return;
  g.stats.resolves++;
  Barrier(src->resource.Get(), src->state,
          D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  Barrier(t->resource.Get(), t->state, D3D12_RESOURCE_STATE_RENDER_TARGET);
  // 7e3 colour targets: the console stored 7e3 per channel; bring the RGBA16F
  // host copy back into that range so the bloom chain and the tonemap see
  // what they saw on the console (dp_native_7e3_resolve).
  const bool src_7e3 = !src->depth && (src->guest_format & 0x3F) == 63;
  // [NEW FABLE VERSION] host sizes: when the source surface is scaled and the
  // destination is not (a small target below the scale threshold), the blit's
  // bilinear filter downsamples, which is what the console's resolve did.
  Blit(src->srv_index, g.rtvs.Cpu(rtv), t->format, t->hw(), t->hh(),
       ((src_7e3 && REXCVAR_GET(dp_native_7e3_resolve)) ? 1u : 0u) |
           ((src_7e3 && REXCVAR_GET(dp_native_7e3_alpha)) ? 2u : 0u));  // [NEW FABLE VERSION] fixed-point alpha
  Barrier(t->resource.Get(), t->state,
          D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  // [NEW FABLE VERSION] 2026-09-24: this resolve is now the newest write of its memory.
  t->mem_seq = ++g_mem_seq;
  if (t->info_valid) g_resolve_by_base[t->info.memory.base_address] = t;
  if (g_dump_active && !t->is_cube && (t->depth <= 1 || t->is_3d)) {
    // Intermediate result of the pass chain (the end-of-frame dump only shows the last content).
    RecordDump(fmt::format("r{:03}", seq).c_str(), dest_texture, t->resource.Get(), t->state, t->hw(), t->hh(), t->format,
               t->is_3d ? dest_slice : 0, t->is_3d ? std::max(1u, t->depth) : 1);
  }
}

RendererStats OnSwap(uint32_t front_buffer_texture) {
  const int64_t swap_t0 = Qpc();  // [NEW FABLE VERSION]
  RecordScope record_scope(__func__);  // [new_fix_24092026] 2.0.3
  RendererStats stats = g.stats;
  stats.watch_hits = g_watch_hits.exchange(0, std::memory_order_relaxed);
  if (!InitContext()) return stats;
  if (const int32_t f = REXCVAR_GET(dp_native_test_gpu_loss_frame); f > 0 && g.frame_number == uint32_t(f)) {
    ComPtr<ID3D12Device5> d5;  // [new_fix_24092026] test hook, see dp_native_test_gpu_loss_frame
    if (SUCCEEDED(g.device->QueryInterface(IID_PPV_ARGS(&d5)))) {
      REXLOG_WARN("Native renderer: test: removing the D3D12 device at frame {}", f);
      d5->RemoveDevice();
    }
  }
  auto* system = NativeGraphicsSystem::instance();
  GuestTexture* front = TextureForObject(front_buffer_texture);
  if (g.frame_number < 3 || g.frame_number % 600 == 0) {
    REXLOG_INFO("Native renderer: Swap front {:08X}: registered {}, resource {}, srv {}, {}x{} fmt {}, resolve target {}",
                front_buffer_texture, front != nullptr, front && front->resource, front ? front->srv_index : 0,
                front ? front->width : 0, front ? front->height : 0, front ? uint32_t(front->format) : 0,
                front ? front->is_resolve_target : false);
  }
  if (BeginFrame()) {
    const int32_t dump_frame = REXCVAR_GET(dp_native_dump_frame);
    // logs/native_dump.trigger (created by hand while the game runs) dumps the next frame.
    if (g.frame_number % 30 == 0) {
      std::error_code ec;
      const std::filesystem::path trigger = std::filesystem::current_path() / "logs" / "native_dump.trigger";
      if (std::filesystem::exists(trigger, ec)) {
        std::filesystem::remove(trigger, ec);
        g_dump_request_frame = g.frame_number + 1;
        REXLOG_INFO("Native renderer: dump trigger seen, dumping frame {}", g_dump_request_frame);
      }
    }
    const bool dump = (dump_frame > 0 && int32_t(g.frame_number) == dump_frame) ||
                      (g_dump_request_frame && g.frame_number == g_dump_request_frame);
    const bool next_is_dump = (dump_frame > 0 && int32_t(g.frame_number) + 1 == dump_frame) ||
                              (g_dump_request_frame && g.frame_number + 1 == g_dump_request_frame);
    if (dump) {
      std::lock_guard<std::mutex> lock(g_registry_mutex);
      for (auto& [key, t] : g_textures_by_key) {
        const uint32_t guest = t->guest;
        if (t->resource && (t->is_resolve_target || t.get() == front)) {
          if (t->is_3d) {
            // Texture3D: a whole-subresource copy into a 2D footprint removes the device; per slice.
            for (uint32_t z = 0; z < std::max(1u, t->depth); ++z) {
              RecordDump("resolve", guest, t->resource.Get(), t->state, t->hw(), t->hh(), t->format, z, std::max(1u, t->depth));
            }
          } else if (!t->is_cube && t->depth <= 1) {
            RecordDump(t.get() == front ? "front" : "resolve", guest, t->resource.Get(), t->state, t->hw(),
                       t->hh(), t->format);
          }
        }
      }
      for (auto& [guest, sf] : g_surfaces) {
        if (sf->resource && !sf->depth) RecordDump("surface", guest, sf->resource.Get(), sf->state, sf->hw(), sf->hh(), sf->format);
      }
    }
    if (front && front->resource && system && system->presenter()) {
      // Present: the presenter callback records the blit and submits the frame.
      Barrier(front->resource.Get(), front->state,
              D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
      system->PresentFromRenderer(front->srv_index, front->hw(), front->hh());  // [NEW FABLE VERSION] host size
    }
    if (g.recording) SubmitFrame();
    if (dump) WriteDumps(g.frame_number);
    g_dump_active = next_is_dump;
    if (next_is_dump) {
      g_draw_dump_budget = REXCVAR_GET(dp_native_dump_all) ? 2000 : 400;
      g_dump_seq = 0;
    }
  }
  g.frame_number++;
  st.bound_rt0 = st.bound_ds = UINTPTR_MAX;  // [new_fix_24092026] 2.0.3
  st.bound_pso = nullptr;
  st.root_bound = false;  // [NEW FABLE VERSION] new command list next frame
  // The game's vertex streaming pools (4 x 1.3 MB, FixBufferCopy threads) are
  // written without Lock, several times per frame. Watched buffers re-upload
  // at the next draw after a CPU write (WatchBuffer); a buffer whose pages
  // cannot be watched keeps the old once-per-frame re-upload.
  {
    std::lock_guard<std::mutex> lock(g_registry_mutex);
    for (auto& [guest, b] : g_buffers) {
      if (!b->watched) b->dirty = true;
    }
  }
  // [NEW FABLE VERSION] per-frame timing samples: frame interval between Swaps,
  // native CPU time (draws + clears + resolves + this Swap), fence waits.
  {
    const int64_t now = Qpc();
    g_perf.cpu_ticks += now - swap_t0;
    if (g_perf.last_swap) g_perf.frame_ms.push_back(float(QpcToMs(now - g_perf.last_swap)));
    g_perf.last_swap = now;
    g_perf.cpu_ms.push_back(float(QpcToMs(g_perf.cpu_ticks)));
    g_perf.wait_ms.push_back(float(QpcToMs(g_perf.wait_ticks)));
    // [new_fix_24092026] CPU the Swap thread (the game's main thread) really
    // used since the previous Swap: game code + XDK + native, waits excluded.
    ULONG64 cycles = 0;
    if (QueryThreadCycleTime(GetCurrentThread(), &cycles)) {
      const double per_ms = TscPerMs();
      if (g_perf.last_thread_cycles && per_ms > 0.0 && cycles >= g_perf.last_thread_cycles) {
        g_perf.thread_ms.push_back(float(double(cycles - g_perf.last_thread_cycles) / per_ms));
      }
      g_perf.last_thread_cycles = cycles;
    }
    stats.draw_cpu_us = uint64_t(QpcToMs(g_perf.cpu_ticks) * 1000.0);
    g_perf.cpu_ticks = 0;
    g_perf.wait_ticks = 0;
  }
  stats.psos_total = uint32_t(g.psos.size());
  stats.const_uploads = g.const_uploads;  // [NEW FABLE VERSION]
  stats.const_reuses = g.const_reuses;
  g.const_uploads = g.const_reuses = 0;
  return stats;
}

// [NEW FABLE VERSION]
std::string PerfSummaryAndReset() {
  auto pct = [](std::vector<float>& v) -> std::array<float, 3> {
    if (v.empty()) return {0.0f, 0.0f, 0.0f};
    std::sort(v.begin(), v.end());
    auto at = [&](double q) { return v[std::min(v.size() - 1, size_t(q * double(v.size())))]; };
    return {at(0.5), at(0.9), at(0.99)};
  };
  const size_t frames = g_perf.frame_ms.size(), gpu_frames = g_perf.gpu_ms.size();
  const auto f = pct(g_perf.frame_ms), c = pct(g_perf.cpu_ms), w = pct(g_perf.wait_ms), gpu = pct(g_perf.gpu_ms);
  const auto m = pct(g_perf.thread_ms);  // [new_fix_24092026]
  std::string s = fmt::format(
      "frame ms p50/p90/p99 {:.1f}/{:.1f}/{:.1f} ({} frames), main thread cpu ms {:.2f}/{:.2f}/{:.2f}, native cpu ms "
      "{:.2f}/{:.2f}/{:.2f}, fence wait ms {:.2f}/{:.2f}/{:.2f}, gpu ms {:.2f}/{:.2f}/{:.2f} ({} frames)",
      f[0], f[1], f[2], frames, m[0], m[1], m[2], c[0], c[1], c[2], w[0], w[1], w[2], gpu[0], gpu[1], gpu[2],
      gpu_frames);
  if (REXCVAR_GET(dp_native_perf_threads)) s += "; " + ThreadCpuReport();  // [new_fix_24092026]
  // [new_fix_24092026] 2.0.3: host object lifetime (native_retire.h), shader
  // view heap use, and the recording-path turn check (RecordScope).
  s += fmt::format("; retired {} freed {} waiting {}, re-created without Release {}, views {}/{} rtv {}/{} dsv {}/{}, recording overlaps {}",
                   g_retired_count.exchange(0, std::memory_order_relaxed),
                   g_freed_count.exchange(0, std::memory_order_relaxed), Retired().size(),
                   g_recreated_count.load(std::memory_order_relaxed), g.views.InUse(), kViewHeapSize, g.rtvs.InUse(),
                   kRtvHeapSize, g.dsvs.InUse(), kDsvHeapSize,
                   g_record_overlaps.load(std::memory_order_relaxed));
  g_perf.thread_ms.clear();
  g_perf.frame_ms.clear();
  g_perf.cpu_ms.clear();
  g_perf.wait_ms.clear();
  g_perf.gpu_ms.clear();
  return s;
}

void RendererSubmitCurrentFrame() { SubmitFrame(); }

bool GammaRampEnabled() { return REXCVAR_GET(dp_native_gamma_ramp); }  // [new_fix_24092026]

// [new_fix_24092026] Before the D3D12 device exists: DRED breadcrumb contexts
// (the markers) when dp_native_gpu_markers is on. The SDK provider already
// forces auto-breadcrumbs and page-fault data.
void PrepareGpuDiagnostics() {
  if (!REXCVAR_GET(dp_native_gpu_markers)) return;
  HMODULE d3d12 = LoadLibraryW(L"d3d12.dll");
  auto get = d3d12 ? reinterpret_cast<PFN_D3D12_GET_DEBUG_INTERFACE>(GetProcAddress(d3d12, "D3D12GetDebugInterface"))
                   : nullptr;
  ComPtr<ID3D12DeviceRemovedExtendedDataSettings1> settings;
  if (get && SUCCEEDED(get(IID_PPV_ARGS(&settings)))) {
    settings->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
    settings->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
    settings->SetBreadcrumbContextEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
    REXLOG_INFO("Native renderer: GPU markers on (dp_native_gpu_markers): DRED breadcrumb contexts forced on");
  } else {
    REXLOG_WARN("Native renderer: GPU markers requested but DRED breadcrumb contexts are unavailable");
  }
}

namespace {
const char* BreadcrumbOpName(D3D12_AUTO_BREADCRUMB_OP op) {
  static const char* const kNames[] = {"SetMarker", "BeginEvent", "EndEvent", "DrawInstanced", "DrawIndexedInstanced",
                                       "ExecuteIndirect", "Dispatch", "CopyBufferRegion", "CopyTextureRegion",
                                       "CopyResource", "CopyTiles", "ResolveSubresource", "ClearRenderTargetView",
                                       "ClearUnorderedAccessView", "ClearDepthStencilView", "ResourceBarrier",
                                       "ExecuteBundle", "Present", "ResolveQueryData", "BeginSubmission",
                                       "EndSubmission"};
  return uint32_t(op) < sizeof(kNames) / sizeof(kNames[0]) ? kNames[op] : "op";
}
std::string Narrow(const wchar_t* w) {
  std::string s;
  if (!w) return s;
  for (; *w; ++w) s.push_back(*w < 128 ? char(*w) : '?');
  return s;
}
}  // namespace

namespace {
// [new_fix_24092026] The newest deadlyprem*.log next to the exe = this session's.
std::wstring CurrentLogPath() {
  wchar_t exe[MAX_PATH] = {};
  if (!GetModuleFileNameW(nullptr, exe, MAX_PATH)) return L"logs";
  const std::filesystem::path dir = std::filesystem::path(exe).parent_path() / "logs";
  std::filesystem::path best;
  std::filesystem::file_time_type best_time{};
  std::error_code ec;
  for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
    const std::wstring name = e.path().filename().wstring();
    if (name.rfind(L"deadlyprem", 0) != 0 || e.path().extension() != L".log") continue;
    const auto t = e.last_write_time(ec);
    if (!ec && (best.empty() || t > best_time)) {
      best = e.path();
      best_time = t;
    }
  }
  return best.empty() ? dir.wstring() : best.wstring();
}

// [new_fix_24092026] A lost D3D12 device does not come back and the native
// renderer cannot recreate its resources: tell the player where the log is and
// close, instead of the frozen window the game used to leave behind. The box
// runs on a thread of its own (the caller may be the UI thread inside a paint;
// a message loop there could run the app's shutdown underneath it), and that
// thread ends the process.
void ExitAfterGpuLoss() {
  rex::FlushLogging();
  std::thread([] {
  const std::wstring text =
      L"The graphics card stopped responding while the native renderer was drawing (GPU lost).\n\n"
      L"The game has to close. Please report it at github.com/LittleBitUA/DPRecomp/issues and attach this log "
      L"(it records what the graphics card was doing):\n\n" +
      CurrentLogPath() +
      L"\n\nUntil it is fixed you can play with the emulated renderer: launcher -> Settings -> Native -> "
      L"turn off \"Native Renderer (test)\".";
  MessageBoxW(nullptr, text.c_str(), L"Deadly Premonition Recompilation - GPU lost",
              MB_OK | MB_ICONERROR | MB_TOPMOST | MB_SETFOREGROUND);
  rex::FlushLogging();
  std::_Exit(3);
  }).detach();
}
}  // namespace

// [new_fix_24092026] Once per session, from the presenter's loss callback:
// reason + DRED to the log, then the message and exit.
void LogDeviceRemoval() {
  static std::atomic<bool> done{false};
  if (done.exchange(true)) return;
  if (!g.device) {
    REXLOG_ERROR("Native renderer: GPU lost before the renderer had a device");
    ExitAfterGpuLoss();
    return;
  }
  const HRESULT reason = g.device->GetDeviceRemovedReason();
  const char* what = reason == DXGI_ERROR_DEVICE_HUNG ? "DEVICE_HUNG (a GPU operation ran too long: TDR)"
                   : reason == DXGI_ERROR_DEVICE_REMOVED ? "DEVICE_REMOVED (page fault or driver)"
                   : reason == DXGI_ERROR_DEVICE_RESET ? "DEVICE_RESET (bad command)"
                   : reason == DXGI_ERROR_DRIVER_INTERNAL_ERROR ? "DRIVER_INTERNAL_ERROR"
                   : reason == DXGI_ERROR_INVALID_CALL ? "INVALID_CALL"
                   : reason == S_OK ? "S_OK (device still alive)" : "other";
  REXLOG_ERROR("Native renderer: GPU loss reason 0x{:08X} {} (frame {}, draws this frame {})", uint32_t(reason), what,
               g.frame_number, g.stats.draws);
  ComPtr<ID3D12DeviceRemovedExtendedData1> dred;
  if (FAILED(g.device->QueryInterface(IID_PPV_ARGS(&dred)))) {
    REXLOG_ERROR("Native renderer: no DRED data");
    ExitAfterGpuLoss();
    return;
  }
  D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT1 crumbs = {};
  if (SUCCEEDED(dred->GetAutoBreadcrumbsOutput1(&crumbs))) {
    uint32_t lists = 0;
    for (const D3D12_AUTO_BREADCRUMB_NODE1* n = crumbs.pHeadAutoBreadcrumbNode; n; n = n->pNext) {
      const uint32_t count = n->BreadcrumbCount;
      const uint32_t completed = n->pLastBreadcrumbValue ? *n->pLastBreadcrumbValue : 0;
      if (!n->pCommandHistory || completed >= count) continue;  // finished lists
      if (++lists > 16) break;
      REXLOG_ERROR("Native renderer: DRED list '{}' on queue '{}': {} of {} ops completed",
                   n->pCommandListDebugNameA ? n->pCommandListDebugNameA : Narrow(n->pCommandListDebugNameW),
                   n->pCommandQueueDebugNameA ? n->pCommandQueueDebugNameA : Narrow(n->pCommandQueueDebugNameW),
                   completed, count);
      const uint32_t from = completed > 6 ? completed - 6 : 0, to = std::min(count, completed + 4);
      for (uint32_t i = from; i < to; ++i) {
        REXLOG_ERROR("Native renderer: DRED   [{}] {}{}", i, BreadcrumbOpName(n->pCommandHistory[i]),
                     i == completed ? "   <-- first op not completed" : "");
      }
      // The markers: the last few at or before the first unfinished op, and the next one.
      int32_t last = -1;
      for (uint32_t c = 0; c < n->BreadcrumbContextsCount; ++c) {
        if (n->pBreadcrumbContexts[c].BreadcrumbIndex <= completed) last = int32_t(c);
      }
      for (int32_t c = std::max(0, last - 3); c >= 0 && c <= last + 1 && uint32_t(c) < n->BreadcrumbContextsCount; ++c) {
        REXLOG_ERROR("Native renderer: DRED   marker at op {}: {}{}", n->pBreadcrumbContexts[c].BreadcrumbIndex,
                     Narrow(n->pBreadcrumbContexts[c].pContextString), c == last ? "   <-- running" : "");
      }
    }
    if (!lists) REXLOG_ERROR("Native renderer: DRED: no unfinished command list recorded");
  }
  D3D12_DRED_PAGE_FAULT_OUTPUT1 fault = {};
  if (SUCCEEDED(dred->GetPageFaultAllocationOutput1(&fault)) && fault.PageFaultVA) {
    REXLOG_ERROR("Native renderer: DRED page fault at GPU VA 0x{:016X}", fault.PageFaultVA);
    uint32_t k = 0;
    for (const D3D12_DRED_ALLOCATION_NODE1* a = fault.pHeadExistingAllocationNode; a && k < 8; a = a->pNext, ++k) {
      REXLOG_ERROR("Native renderer: DRED   live allocation near it: '{}' type {}",
                   a->ObjectNameA ? a->ObjectNameA : Narrow(a->ObjectNameW), int(a->AllocationType));
    }
    k = 0;
    for (const D3D12_DRED_ALLOCATION_NODE1* a = fault.pHeadRecentFreedAllocationNode; a && k < 8; a = a->pNext, ++k) {
      REXLOG_ERROR("Native renderer: DRED   recently FREED allocation near it: '{}' type {}",
                   a->ObjectNameA ? a->ObjectNameA : Narrow(a->ObjectNameW), int(a->AllocationType));
    }
  } else {
    REXLOG_ERROR("Native renderer: DRED: no page fault recorded");
  }
  ExitAfterGpuLoss();
}

// Used by NativeGraphicsSystem::PresentFromRenderer to draw the blit into the
// presenter texture with the renderer's list (same frame).
void RendererBlitToPresenter(uint32_t src_srv, ID3D12Resource* dest, uint32_t width, uint32_t height) {
  // The presenter's guest output texture (R10G10B10A2, UAV) is not RTV-capable:
  // blit into a scratch RTV texture, then copy.
  static ComPtr<ID3D12Resource> scratch;
  static uint32_t scratch_rtv = UINT32_MAX, scratch_w = 0, scratch_h = 0;
  static D3D12_RESOURCE_STATES scratch_state = D3D12_RESOURCE_STATE_RENDER_TARGET;
  if (!scratch || scratch_w != width || scratch_h != height) {
    RetireResource(std::move(scratch));
    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = width;
    desc.Height = height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = rex::ui::d3d12::D3D12Presenter::kGuestOutputFormat;
    desc.SampleDesc.Count = 1;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    D3D12_CLEAR_VALUE clear = {};
    clear.Format = desc.Format;
    if (FAILED(g.device->CreateCommittedResource(&rex::ui::d3d12::util::kHeapPropertiesDefault, D3D12_HEAP_FLAG_NONE,
                                                 &desc, D3D12_RESOURCE_STATE_RENDER_TARGET, &clear,
                                                 IID_PPV_ARGS(&scratch)))) {
      return;
    }
    scratch->SetName(L"DP1 present scratch");  // [new_fix_24092026] 2.0.3
    scratch_state = D3D12_RESOURCE_STATE_RENDER_TARGET;
    if (scratch_rtv == UINT32_MAX) scratch_rtv = g.rtvs.Allocate();
    if (scratch_rtv == UINT32_MAX) {  // [new_fix_24092026] 2.0.3
      HeapFull("render target view");
      return;
    }
    g.device->CreateRenderTargetView(scratch.Get(), nullptr, g.rtvs.Cpu(scratch_rtv));
    scratch_w = width;
    scratch_h = height;
  }
  Barrier(scratch.Get(), scratch_state, D3D12_RESOURCE_STATE_RENDER_TARGET);
  // [new_fix_24092026] the console's display gamma ramp (dp_native_gamma_ramp):
  // upload the captured table when it changes, apply it in the blit (mode 4).
  uint32_t present_mode = 0;
  if (REXCVAR_GET(dp_native_gamma_ramp)) {
    static ComPtr<ID3D12Resource> lut;
    static D3D12_RESOURCE_STATES lut_state = D3D12_RESOURCE_STATE_COPY_DEST;
    static uint32_t lut_version = UINT32_MAX;
    std::array<uint32_t, 256> table;
    auto* sys = NativeGraphicsSystem::instance();
    const uint32_t version = sys ? sys->CopyGammaRamp(table) : 0;
    if (version != 0 && !lut) {
      D3D12_RESOURCE_DESC ld = {};
      ld.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
      ld.Width = 256;
      ld.Height = 1;
      ld.DepthOrArraySize = 1;
      ld.MipLevels = 1;
      ld.Format = DXGI_FORMAT_R10G10B10A2_UNORM;
      ld.SampleDesc.Count = 1;
      if (SUCCEEDED(g.device->CreateCommittedResource(&rex::ui::d3d12::util::kHeapPropertiesDefault,
                                                      D3D12_HEAP_FLAG_NONE, &ld, D3D12_RESOURCE_STATE_COPY_DEST,
                                                      nullptr, IID_PPV_ARGS(&lut)))) {
        lut_state = D3D12_RESOURCE_STATE_COPY_DEST;
        lut->SetName(L"DP1 display gamma ramp");  // [new_fix_24092026] 2.0.3
        if (g_gamma_lut_srv == UINT32_MAX) g_gamma_lut_srv = g.views.Allocate();
        if (g_gamma_lut_srv != UINT32_MAX) {  // [new_fix_24092026] 2.0.3
          g.device->CreateShaderResourceView(lut.Get(), nullptr, g.views.Cpu(g_gamma_lut_srv));
        } else {
          HeapFull("shader view");
          lut.Reset();  // never used by a command list yet
        }
      }
    }
    if (lut && version != 0 && version != lut_version) {
      UploadRing& ring = g.upload[g.frame_index];
      const size_t off = ring.Allocate(256 * 4, 512);
      if (off != SIZE_MAX) {
        uint32_t* dst = reinterpret_cast<uint32_t*>(ring.mapped + off);
        for (uint32_t i = 0; i < 256; ++i) dst[i] = Lut30ToR10G10B10A2(table[i]);
        Barrier(lut.Get(), lut_state, D3D12_RESOURCE_STATE_COPY_DEST);
        D3D12_TEXTURE_COPY_LOCATION ldst = {}, lsrc = {};
        ldst.pResource = lut.Get();
        ldst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        lsrc.pResource = ring.buffer.Get();
        lsrc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        lsrc.PlacedFootprint.Offset = off;
        lsrc.PlacedFootprint.Footprint = {DXGI_FORMAT_R10G10B10A2_UNORM, 256, 1, 1, 256 * 4};
        g.list->CopyTextureRegion(&ldst, 0, 0, 0, &lsrc, nullptr);
        Barrier(lut.Get(), lut_state,
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        lut_version = version;
      }
    }
    // The last uploaded table stays valid when this frame's upload found no ring space.
    if (lut && lut_version != UINT32_MAX) present_mode = 4;
  }
  Blit(src_srv, g.rtvs.Cpu(scratch_rtv), rex::ui::d3d12::D3D12Presenter::kGuestOutputFormat, width, height,
       present_mode);
  Barrier(scratch.Get(), scratch_state, D3D12_RESOURCE_STATE_COPY_SOURCE);
  D3D12_RESOURCE_BARRIER to_copy = {};
  to_copy.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  to_copy.Transition.pResource = dest;
  to_copy.Transition.StateBefore = rex::ui::d3d12::D3D12Presenter::kGuestOutputInternalState;
  to_copy.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
  to_copy.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  g.list->ResourceBarrier(1, &to_copy);
  g.list->CopyResource(dest, scratch.Get());
  std::swap(to_copy.Transition.StateBefore, to_copy.Transition.StateAfter);
  g.list->ResourceBarrier(1, &to_copy);
}

}  // namespace dp::native
