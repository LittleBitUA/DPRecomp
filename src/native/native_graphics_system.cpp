// Deadly Premonition Recompilation - native renderer skeleton (2026-09-18).
// See native_graphics_system.h for the role of this file.

#include "native_graphics_system.h"
#include "native_renderer.h"

#include <algorithm>
#include <chrono>
#include <cmath>

#include <rex/chrono/clock.h>
#include <rex/cvar.h>
#include <rex/kernel/xboxkrnl/video.h>
#include <rex/logging.h>
#include <rex/graphics/xenos.h>
#include <rex/memory/ring_buffer.h>
#include <rex/memory/utils.h>
#include <rex/system/function_dispatcher.h>
#include <rex/system/function_dispatcher.h>
#include <rex/system/kernel_state.h>
#include <rex/system/thread_state.h>
#include <rex/system/xmemory.h>
#include <rex/thread.h>
#include <rex/ui/d3d12/d3d12_presenter.h>
#include <rex/ui/d3d12/d3d12_provider.h>
#include <rex/ui/d3d12/d3d12_util.h>
#include <rex/ui/windowed_app_context.h>

#include <windows.h>
#include <wrl/client.h>

REXCVAR_DEFINE_INT32(dp_native_thread_dump_at, 0, "DP1",
                     "Native renderer diagnostics: N seconds after boot log every guest thread's guest stack once "
                     "(0 = off)");
REXCVAR_DEFINE_BOOL(dp_native_render, false, "DP1",
                    "EXPERIMENTAL: render through the native D3D12 renderer instead of the Xenos "
                    "GPU emulator (milestone 0: test pattern only). Off = the default emulated path, "
                    "byte for byte");

namespace dp::native {

using Microsoft::WRL::ComPtr;
using rex::X_STATUS;

bool Enabled() {
#if defined(DP_REGION_USA)
  // The USA build has no native hooks yet (PAL addresses only); the switch
  // is ignored there so the game keeps the emulated path.
  static const bool enabled = [] {
    if (REXCVAR_GET(dp_native_render)) {
      REXLOG_WARN("dp_native_render is not available on the USA build yet; using the Xenos emulator");
    }
    return false;
  }();
#else
  static const bool enabled = REXCVAR_GET(dp_native_render);
#endif
  return enabled;
}

NativeGraphicsSystem* NativeGraphicsSystem::instance_ = nullptr;

namespace {
uint32_t FindFunctionStart(rex::runtime::FunctionDispatcher* d, uint32_t address) {
  uint32_t a = address & ~3u;
  for (uint32_t n = 0; n < 0x20000 / 4 && a >= 0x80000000u; ++n, a -= 4) {
    if (d->GetFunction(a)) return a;
  }
  return 0;
}
void DumpGuestThreads(rex::system::KernelState* ks) {
  auto* dispatcher = ks->function_dispatcher();
  auto* memory = ks->memory();
  auto threads = ks->object_table()->GetObjectsByType<rex::system::XThread>(rex::system::XObject::Type::Thread);
  REXLOG_INFO("Native diag: {} guest threads", threads.size());
  for (auto& t : threads) {
    auto* ts = t->thread_state();
    const PPCContext* c = ts ? ts->context() : nullptr;
    if (!c) continue;
    std::string out = fmt::format("Native diag: thread '{}' lr={:08X} r1={:08X}:", t->name(), uint32_t(c->lr), c->r1.u32);
    auto describe = [&](uint32_t addr) {
      const uint32_t start = FindFunctionStart(dispatcher, addr);
      out += start ? fmt::format(" sub_{:08X}+0x{:X}", start, addr - start) : fmt::format(" {:08X}?", addr);
    };
    describe(uint32_t(c->lr));
    auto readable = [&](uint32_t guest) {
      if (guest < 0x10000u || guest >= 0xE0000000u || (guest & 3)) return false;
      MEMORY_BASIC_INFORMATION mbi = {};
      if (!VirtualQuery(memory->TranslateVirtual(guest), &mbi, sizeof(mbi))) return false;
      return mbi.State == MEM_COMMIT && (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) == 0 && mbi.Protect != 0;
    };
    uint32_t sp = c->r1.u32;
    for (int depth = 0; depth < 12; ++depth) {
      if (!readable(sp)) break;
      const uint32_t back = rex::memory::load_and_swap<uint32_t>(memory->TranslateVirtual(sp));
      if (back <= sp || !readable(back - 8)) break;
      const uint32_t ret = rex::memory::load_and_swap<uint32_t>(memory->TranslateVirtual(back - 8));
      if (ret < 0x80000000u || ret >= 0xE0000000u || (ret & 3)) break;
      describe(ret);
      sp = back;
    }
    REXLOG_INFO("{}", out);
  }
}
}  // namespace

// ---------------------------------------------------------------------------
// Host frame output (milestone 0: a colour that cycles with the frame count,
// copied into the presenter's guest-output texture).
// ---------------------------------------------------------------------------
struct NativeGraphicsSystem::FrameOutput {
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList> list;
  ComPtr<ID3D12Fence> fence;
  HANDLE fence_event = nullptr;
  UINT64 fence_value = 0;
  ComPtr<ID3D12DescriptorHeap> rtv_heap;
  ComPtr<ID3D12Resource> texture;  // kGuestOutputFormat, RTV-capable
  D3D12_RESOURCE_STATES texture_state = D3D12_RESOURCE_STATE_COMMON;
  uint32_t width = 0, height = 0;

  ~FrameOutput() {
    WaitIdle();
    if (fence_event) CloseHandle(fence_event);
  }

  void WaitIdle() {
    if (!fence || !fence_event) return;
    if (fence->GetCompletedValue() < fence_value) {
      fence->SetEventOnCompletion(fence_value, fence_event);
      WaitForSingleObject(fence_event, INFINITE);
    }
  }

  bool Initialize(ID3D12Device* device) {
    if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)))) return false;
    if (FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
                                         IID_PPV_ARGS(&list)))) {
      return false;
    }
    list->Close();
    if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)))) return false;
    fence_event = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    if (!fence_event) return false;
    D3D12_DESCRIPTOR_HEAP_DESC heap_desc = {};
    heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    heap_desc.NumDescriptors = 1;
    if (FAILED(device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&rtv_heap)))) return false;
    return true;
  }

  bool EnsureTexture(const rex::ui::d3d12::D3D12Provider& provider, uint32_t w, uint32_t h) {
    if (texture && width == w && height == h) return true;
    WaitIdle();
    texture.Reset();
    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = w;
    desc.Height = h;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = rex::ui::d3d12::D3D12Presenter::kGuestOutputFormat;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    D3D12_CLEAR_VALUE clear = {};
    clear.Format = desc.Format;
    if (FAILED(provider.GetDevice()->CreateCommittedResource(
            &rex::ui::d3d12::util::kHeapPropertiesDefault, provider.GetHeapFlagCreateNotZeroed(), &desc,
            D3D12_RESOURCE_STATE_RENDER_TARGET, &clear, IID_PPV_ARGS(&texture)))) {
      return false;
    }
    texture_state = D3D12_RESOURCE_STATE_RENDER_TARGET;
    width = w;
    height = h;
    provider.GetDevice()->CreateRenderTargetView(texture.Get(), nullptr,
                                                 rtv_heap->GetCPUDescriptorHandleForHeapStart());
    return true;
  }

  static D3D12_RESOURCE_BARRIER Transition(ID3D12Resource* r, D3D12_RESOURCE_STATES from,
                                           D3D12_RESOURCE_STATES to) {
    D3D12_RESOURCE_BARRIER b = {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = r;
    b.Transition.StateBefore = from;
    b.Transition.StateAfter = to;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    return b;
  }
};

// ---------------------------------------------------------------------------

NativeGraphicsSystem::NativeGraphicsSystem() { instance_ = this; }

NativeGraphicsSystem::~NativeGraphicsSystem() {
  Shutdown();
  if (instance_ == this) instance_ = nullptr;
}

rex::ui::GraphicsProvider* NativeGraphicsSystem::provider() const { return provider_.get(); }

rex::X_STATUS NativeGraphicsSystem::SetupPresentation(rex::ui::WindowedAppContext* app_context) {
  if (presenter_) return X_STATUS_SUCCESS;
  if (!provider_) {
    provider_ = rex::ui::d3d12::D3D12Provider::Create();
    if (!provider_) {
      REXLOG_ERROR("Native renderer: unable to create the D3D12 provider");
      return X_STATUS_UNSUCCESSFUL;
    }
  }
  app_context_ = app_context;
  auto loss_cb = [](bool is_responsible, bool /*statically_from_ui_thread*/) {
    REXLOG_ERROR("Native renderer: host GPU lost (responsible: {})", is_responsible);
  };
  if (app_context_) {
    // Presenter creation must happen on the UI thread (same rule as the emulator).
    app_context_->CallInUIThreadSynchronous([this, loss_cb]() { presenter_ = provider_->CreatePresenter(loss_cb); });
  } else {
    presenter_ = provider_->CreatePresenter(loss_cb);
  }
  if (!presenter_) {
    REXLOG_ERROR("Native renderer: unable to create the presenter");
    return X_STATUS_UNSUCCESSFUL;
  }
  REXLOG_INFO("Native renderer: presentation ready (D3D12), milestone 0 skeleton");
  return X_STATUS_SUCCESS;
}

rex::X_STATUS NativeGraphicsSystem::SetupGuestGpu(rex::runtime::FunctionDispatcher* function_dispatcher,
                                             rex::system::KernelState* kernel_state) {
  memory_ = function_dispatcher->memory();
  function_dispatcher_ = function_dispatcher;
  kernel_state_ = kernel_state;
  if (!provider_) {
    provider_ = rex::ui::d3d12::D3D12Provider::Create();
    if (!provider_) return X_STATUS_UNSUCCESSFUL;
  }

  // GPU registers 0x7FC80000..0x7FC8FFFF: an unregistered range makes loads
  // return garbage; the XDK reads the interrupt status (0x1951) in its
  // interrupt handler and kicks the ring through CP_RB_WPTR (0x01C5).
  memory_->AddVirtualMappedRange(0x7FC80000, 0xFFFF0000, 0x0000FFFF, this,
                                 reinterpret_cast<rex::runtime::MMIOReadCallback>(ReadRegisterThunk),
                                 reinterpret_cast<rex::runtime::MMIOWriteCallback>(WriteRegisterThunk));

  // Guest vblank timer: the game's frame limiter (sub_825224B0) waits on the
  // counters its vblank callback (sub_8252ECE8) keeps, which the XDK interrupt
  // handler (sub_824D2A50) drives from this interrupt. Same pacing as the
  // emulator: refresh rate with vsync, 1 kHz without.
  vsync_worker_running_ = true;
  vsync_worker_thread_ = rex::system::object_ref<rex::system::XHostThread>(
      new rex::system::XHostThread(kernel_state_, 128 * 1024, 0, [this]() {
        rex::system::X_VIDEO_MODE video_mode;
        rex::kernel::xboxkrnl::VdQueryVideoMode(&video_mode);
        const double refresh_rate_hz = std::max(1.0, double(float(video_mode.refresh_rate)));
        const uint64_t tick_frequency = rex::chrono::Clock::guest_tick_frequency();
        const uint64_t vsync_interval = std::max(uint64_t(1), uint64_t(double(tick_frequency) / refresh_rate_hz));
        const uint64_t no_vsync_interval = std::max(uint64_t(1), tick_frequency / 1000);
        const bool vsync_on = rex::cvar::GetFlagByName("vsync") != "false";
        uint64_t last = rex::chrono::Clock::QueryGuestTickCount();
        uint64_t last_dump = last;
        while (vsync_worker_running_) {
          const uint64_t now = rex::chrono::Clock::QueryGuestTickCount();
          const int32_t dump_at = REXCVAR_GET(dp_native_thread_dump_at);
          if (dump_at > 0 && now - last_dump >= uint64_t(dump_at) * tick_frequency) {
            last_dump = now;
            DumpGuestThreads(kernel_state_);
          }
          // `vsync` is defined by the emulator plugin, which is not loaded here; the
          // launcher still writes it to the toml, so read the registry by name.
          const uint64_t interval = vsync_on ? vsync_interval : no_vsync_interval;
          while (now - last >= interval) {
            MarkVblank();
            last += interval;
          }
          rex::thread::Sleep(std::chrono::milliseconds(1));
        }
        return 0;
      }));
  vsync_worker_thread_->set_name("Native VSync");
  vsync_worker_thread_->Create();
  REXLOG_INFO("Native renderer: guest GPU side ready (MMIO 0x7FC80000 registered, vblank thread started)");
  return X_STATUS_SUCCESS;
}

void NativeGraphicsSystem::Shutdown() {
  if (vsync_worker_thread_) {
    vsync_worker_running_ = false;
    vsync_worker_thread_->Wait(0, 0, 0, nullptr);
    vsync_worker_thread_.reset();
  }
  {
    std::lock_guard<std::mutex> lock(present_mutex_);
    output_.reset();
  }
  if (presenter_) {
    if (app_context_) {
      app_context_->CallInUIThreadSynchronous([this]() { presenter_.reset(); });
    }
    presenter_.reset();
  }
  provider_.reset();
}

void NativeGraphicsSystem::SetInterruptCallback(uint32_t callback, uint32_t user_data) {
  interrupt_callback_ = callback;
  interrupt_callback_data_ = user_data;
  REXLOG_INFO("Native renderer: SetInterruptCallback({:08X}, {:08X})", callback, user_data);
}

void NativeGraphicsSystem::InitializeRingBuffer(uint32_t ptr, uint32_t size_log2) {
  ring_ptr_ = ptr;
  ring_size_log2_ = size_log2;
  REXLOG_INFO("Native renderer: XDK ring buffer at {:08X} (2^{} bytes) - packets are discarded, never parsed", ptr,
              size_log2);
}

void NativeGraphicsSystem::EnableReadPointerWriteBack(uint32_t ptr, uint32_t block_size_log2) {
  read_ptr_writeback_ptr_ = ptr;
  REXLOG_INFO("Native renderer: ring read-pointer write-back at {:08X} (block 2^{})", ptr, block_size_log2);
}

uint32_t NativeGraphicsSystem::ReadRegisterThunk(void* /*ppc_context*/, void* self, uint32_t addr) {
  return static_cast<NativeGraphicsSystem*>(self)->ReadRegister(addr);
}

void NativeGraphicsSystem::WriteRegisterThunk(void* /*ppc_context*/, void* self, uint32_t addr, uint32_t value) {
  static_cast<NativeGraphicsSystem*>(self)->WriteRegister(addr, value);
}

uint32_t NativeGraphicsSystem::ReadRegister(uint32_t addr) {
  const uint32_t r = (addr & 0xFFFF) / 4;
  switch (r) {
    case 0x0F00:  // RB_EDRAM_TIMING
      return 0x08100748;
    case 0x0F01:  // RB_BC_CONTROL
      return 0x0000200E;
    case 0x194C: {  // R500_D1MODE_V_COUNTER
      rex::system::X_VIDEO_MODE video_mode;
      rex::kernel::xboxkrnl::VdQueryVideoMode(&video_mode);
      return std::min(uint32_t(video_mode.display_height), uint32_t(0x0FFF));
    }
    case 0x1951:  // interrupt status: vblank
      return 1;
    case 0x1961: {  // AVIVO_D1MODE_VIEWPORT_SIZE
      rex::system::X_VIDEO_MODE video_mode;
      rex::kernel::xboxkrnl::VdQueryVideoMode(&video_mode);
      return (std::min(uint32_t(video_mode.display_width), uint32_t(0x0FFF)) << 16) |
             std::min(uint32_t(video_mode.display_height), uint32_t(0x0FFF));
    }
    default:
      return 0;
  }
}

void NativeGraphicsSystem::WriteRegister(uint32_t addr, uint32_t value) {
  const uint32_t r = (addr & 0xFFFF) / 4;
  if (r == 0x01C5) {  // CP_RB_WPTR: the XDK kicked the ring.
    // Walk the new packets for their memory side effects (scratch/fence
    // write-backs the XDK polls: sub_824D2158/sub_824D2730 spin on them once
    // the ring wraps), then report the ring drained through the read-pointer
    // write-back word.
    WalkRing(value);
    if (read_ptr_writeback_ptr_ && memory_) {
      rex::memory::store_and_swap<uint32_t>(memory_->TranslatePhysical(read_ptr_writeback_ptr_), value);
    }
    ring_kicks_.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  WalkRegisterWrite(r, value);
  // 0x1844 (display surface address) and the init-time registers are ignored.
}

// Register writes that matter without a GPU: the scratch write-back set-up and
// the scratch registers themselves (mirrored to memory, like the emulator's
// CommandProcessor::WriteRegister).
void NativeGraphicsSystem::WalkRegisterWrite(uint32_t index, uint32_t value) {
  constexpr uint32_t kScratchUmsk = 0x01DC, kScratchAddr = 0x01DD, kScratchReg0 = 0x0578;
  if (index >= 0x4000 && index < 0x4400) ring_alu_vs_writes_.fetch_add(1, std::memory_order_relaxed);
  if (index >= 0x4400 && index < 0x4800) ring_alu_ps_writes_.fetch_add(1, std::memory_order_relaxed);
  if (index == kScratchUmsk) {
    scratch_umsk_ = value;
  } else if (index == kScratchAddr) {
    scratch_addr_ = value;
  } else if (index >= kScratchReg0 && index < kScratchReg0 + 8) {
    const uint32_t reg = index - kScratchReg0;
    if ((scratch_umsk_ & (1u << reg)) && memory_) {
      rex::memory::store_and_swap<uint32_t>(memory_->TranslatePhysical(scratch_addr_ + reg * 4), value);
      ring_scratch_writes_.fetch_add(1, std::memory_order_relaxed);
    }
  }
}

std::string NativeGraphicsSystem::RingStats() const {
  std::string ops;
  for (uint32_t i = 0; i < 128; ++i) {
    const uint32_t n = ring_opcodes_[i].load(std::memory_order_relaxed);
    if (n) ops += fmt::format(" {:02X}:{}", i, n);
  }
  return fmt::format("ring kicks {} packets {} ibs {} fence writes {} mem writes {} scratch writes {} alu vs {} ps {} const loads {} dwords {} umsk {:X} addr {:08X}; ops{}",
                     ring_kicks_.load(), ring_packets_.load(), ring_ibs_.load(), ring_fence_writes_.load(),
                     ring_mem_writes_.load(), ring_scratch_writes_.load(), ring_alu_vs_writes_.load(),
                     ring_alu_ps_writes_.load(), ring_const_loads_.load(), ring_const_dwords_.load(), scratch_umsk_, scratch_addr_, ops);
}

void NativeGraphicsSystem::WalkRing(uint32_t write_index) {
  if (!memory_ || !ring_ptr_) return;
  const uint32_t size_bytes = 1u << (ring_size_log2_ + 3);
  const uint32_t read_bytes = ring_read_index_ * 4, write_bytes = write_index * 4;
  if (ring_diag_budget_) {
    --ring_diag_budget_;
    REXLOG_INFO("Native ring: kick read {:06X} write {:06X} (ring {:08X} size {:X}); {}", read_bytes, write_bytes, ring_ptr_,
                size_bytes, RingStats());
  }
  if (read_bytes != write_bytes && write_bytes < size_bytes) {
    WalkPackets(memory_->TranslatePhysical(ring_ptr_), size_bytes, read_bytes, write_bytes, 0);
  } else if (write_bytes >= size_bytes) {
    REXLOG_WARN("Native ring: write index {:X} outside the ring size {:X} - not walked", write_bytes, size_bytes);
  }
  ring_read_index_ = write_index;
}

void NativeGraphicsSystem::WalkPackets(const uint8_t* base, uint32_t capacity_bytes, uint32_t read_bytes,
                                       uint32_t write_bytes, int depth) {
  namespace xenos = rex::graphics::xenos;
  if (depth > 4) return;
  rex::memory::RingBuffer reader(const_cast<uint8_t*>(base), capacity_bytes);
  reader.set_read_offset(read_bytes);
  reader.set_write_offset(write_bytes);
  auto swap = [](uint32_t v, uint32_t endian) { return xenos::GpuSwap(v, static_cast<xenos::Endian>(endian)); };
  auto phys = [this](uint32_t a) { return memory_->TranslatePhysical(a); };
  // Like the emulator's do/while: a full buffer (read == write) is not empty.
  bool first = (read_bytes % capacity_bytes) == (write_bytes % capacity_bytes);
  while (first || reader.read_count() >= 4) {
    first = false;
    const uint32_t packet = reader.ReadAndSwap<uint32_t>();
    ring_packets_.fetch_add(1, std::memory_order_relaxed);
    if (packet == 0) continue;
    const uint32_t type = packet >> 30;
    if (type == 0) {
      const uint32_t count = ((packet >> 16) & 0x3FFF) + 1;
      if (reader.read_count() < count * 4) {
        REXLOG_WARN("Native ring: type0 packet {:08X} overruns the walked range (depth {})", packet, depth);
        return;
      }
      const uint32_t base_index = packet & 0x7FFF;
      const bool one_reg = (packet >> 15) & 1;
      for (uint32_t m = 0; m < count; ++m) {
        WalkRegisterWrite(one_reg ? base_index : base_index + m, reader.ReadAndSwap<uint32_t>());
      }
    } else if (type == 1) {
      if (reader.read_count() < 8) return;
      WalkRegisterWrite(packet & 0x7FF, reader.ReadAndSwap<uint32_t>());
      WalkRegisterWrite((packet >> 11) & 0x7FF, reader.ReadAndSwap<uint32_t>());
    } else if (type == 2) {
      continue;
    } else {
      const uint32_t opcode = (packet >> 8) & 0x7F;
      const uint32_t count = ((packet >> 16) & 0x3FFF) + 1;
      ring_opcodes_[opcode].fetch_add(1, std::memory_order_relaxed);
      if (reader.read_count() < count * 4) {
        REXLOG_WARN("Native ring: type3 packet {:08X} (opcode {:02X}) overruns the walked range (depth {})", packet, opcode,
                    depth);
        return;
      }
      const size_t end_offset = (reader.read_offset() + count * 4) % capacity_bytes;
      switch (opcode) {
        case xenos::PM4_SET_CONSTANT: {
          // data[0] = offset_type: bits 16-23 = type (0 alu, 1 fetch, 2 bool, 3 loop, 4 regs), bits 0-10 = index.
          const uint32_t offset_type = reader.ReadAndSwap<uint32_t>();
          static const uint32_t kBase[] = {0x4000, 0x4800, 0x4900, 0x4908, 0x2000};
          const uint32_t type = (offset_type >> 16) & 0xFF;
          const uint32_t base = (type < 5 ? kBase[type] : 0) + (offset_type & 0x7FF);
          ring_const_loads_.fetch_add(1, std::memory_order_relaxed);
          ring_const_dwords_.fetch_add(count - 1, std::memory_order_relaxed);
          if (ring_const_log_budget_.load(std::memory_order_relaxed) > 0) {
            ring_const_log_budget_.fetch_sub(1, std::memory_order_relaxed);
            REXLOG_INFO("Native ring: SET_CONSTANT type {} index {} ({} dwords) -> reg {:04X} (frame {})", type, offset_type & 0x7FF, count - 1, base,
                        frame_count_.load(std::memory_order_relaxed));
          }
          for (uint32_t i = 0; i + 1 < count; ++i) WalkRegisterWrite(base + i, reader.ReadAndSwap<uint32_t>());
          break;
        }
        case xenos::PM4_LOAD_ALU_CONSTANT: {
          // data[0] = guest address of the constants, data[1] = offset_type, data[2] = size in dwords.
          const uint32_t addr = reader.ReadAndSwap<uint32_t>();
          const uint32_t offset_type = reader.ReadAndSwap<uint32_t>();
          const uint32_t size = reader.ReadAndSwap<uint32_t>() & 0xFFF;
          static const uint32_t kBase[] = {0x4000, 0x4800, 0x4900, 0x4908, 0x2000};
          const uint32_t type = (offset_type >> 16) & 0xFF;
          const uint32_t base = (type < 5 ? kBase[type] : 0) + (offset_type & 0x7FF);
          ring_const_loads_.fetch_add(1, std::memory_order_relaxed);
          ring_const_dwords_.fetch_add(size, std::memory_order_relaxed);
          if (ring_const_log_budget_.load(std::memory_order_relaxed) > 0) {
            ring_const_log_budget_.fetch_sub(1, std::memory_order_relaxed);
            REXLOG_INFO("Native ring: LOAD_ALU_CONSTANT addr {:08X} type {} index {} ({} dwords) -> reg {:04X} (frame {})", addr, type,
                        offset_type & 0x7FF, size, base, frame_count_.load(std::memory_order_relaxed));
          }
          for (uint32_t i = 0; i < size; ++i) WalkRegisterWrite(base + i, 0);
          break;
        }
        case xenos::PM4_INDIRECT_BUFFER:
        case xenos::PM4_INDIRECT_BUFFER_PFD: {
          const uint32_t list_ptr = xenos::CpuToGpu(reader.ReadAndSwap<uint32_t>());
          const uint32_t list_len = reader.ReadAndSwap<uint32_t>() & 0xFFFFF;
          ring_ibs_.fetch_add(1, std::memory_order_relaxed);
          if (list_len) WalkPackets(phys(list_ptr), list_len * 4, 0, list_len * 4, depth + 1);
          break;
        }
        case xenos::PM4_MEM_WRITE: {
          uint32_t addr = reader.ReadAndSwap<uint32_t>();
          for (uint32_t i = 0; i + 1 < count; ++i) {
            const uint32_t data = swap(reader.ReadAndSwap<uint32_t>(), addr & 3);
            rex::memory::store(phys(addr & ~3u), data);
            ring_mem_writes_.fetch_add(1, std::memory_order_relaxed);
            addr += 4;
          }
          break;
        }
        case xenos::PM4_EVENT_WRITE_SHD: {
          const uint32_t initiator = reader.ReadAndSwap<uint32_t>();
          const uint32_t addr = reader.ReadAndSwap<uint32_t>();
          uint32_t value = reader.ReadAndSwap<uint32_t>();
          if ((initiator >> 31) & 1) value = frame_count_.load(std::memory_order_relaxed);  // "counter" write
          rex::memory::store(phys(addr & ~3u), swap(value, addr & 3));
          const uint32_t n = ring_fence_writes_.fetch_add(1, std::memory_order_relaxed);
          if (n < 24) REXLOG_INFO("Native ring: EVENT_WRITE_SHD init {:08X} addr {:08X} value {:08X}", initiator, addr, value);
          break;
        }
        case xenos::PM4_EVENT_WRITE_EXT: {
          reader.ReadAndSwap<uint32_t>();
          const uint32_t addr = reader.ReadAndSwap<uint32_t>();
          const uint16_t extents[] = {0, xenos::kTexture2DCubeMaxWidthHeight >> 3, 0,
                                      xenos::kTexture2DCubeMaxWidthHeight >> 3, 0, 1};
          rex::memory::copy_and_swap_16_unaligned(phys(addr & ~3u), extents, rex::countof(extents));
          break;
        }
        case xenos::PM4_COND_WRITE: {
          const uint32_t wait_info = reader.ReadAndSwap<uint32_t>();
          const uint32_t poll_addr = reader.ReadAndSwap<uint32_t>();
          const uint32_t ref = reader.ReadAndSwap<uint32_t>();
          const uint32_t mask = reader.ReadAndSwap<uint32_t>();
          const uint32_t write_addr = reader.ReadAndSwap<uint32_t>();
          const uint32_t write_data = reader.ReadAndSwap<uint32_t>();
          uint32_t value = 0;
          if (wait_info & 0x10) value = swap(rex::memory::load<uint32_t>(phys(poll_addr & ~3u)), poll_addr & 3);
          bool matched = false;
          switch (wait_info & 7) {
            case 1: matched = (value & mask) < ref; break;
            case 2: matched = (value & mask) <= ref; break;
            case 3: matched = (value & mask) == ref; break;
            case 4: matched = (value & mask) != ref; break;
            case 5: matched = (value & mask) >= ref; break;
            case 6: matched = (value & mask) > ref; break;
            case 7: matched = true; break;
            default: break;
          }
          if (matched && (wait_info & 0x100)) rex::memory::store(phys(write_addr & ~3u), swap(write_data, write_addr & 3));
          break;
        }
        case xenos::PM4_INTERRUPT: {
          reader.ReadAndSwap<uint32_t>();  // cpu mask
          // Deliver in packet order, right here: the XDK's callback record
          // (MEM_WRITE into the dev+10900 block just before this packet) is
          // consumed and poisoned (0x0BADF00D) by the ISR, so a delayed or
          // duplicated delivery calls the poison value.
          pending_cp_interrupts_.fetch_add(1, std::memory_order_relaxed);
          if (interrupt_callback_ && function_dispatcher_) {
            if (auto* thread = rex::system::XThread::GetCurrentThread()) {
              uint64_t cp_args[] = {1 /* source: command processor */, interrupt_callback_data_};
              function_dispatcher_->ExecuteInterrupt(thread->thread_state(), interrupt_callback_, cp_args, 2);
            }
          }
          break;
        }
        default:
          break;
      }
      reader.set_read_offset(end_offset);
    }
  }
}

void NativeGraphicsSystem::MarkVblank() {
  if (!interrupt_callback_) return;
  auto* thread = rex::system::XThread::GetCurrentThread();
  if (!thread) return;
  thread->SetActiveCpu(2);
  uint64_t args[] = {0 /* source: vblank */, interrupt_callback_data_};
  function_dispatcher_->ExecuteInterrupt(thread->thread_state(), interrupt_callback_, args, 2);
}

void NativeGraphicsSystem::PresentFromRenderer(uint32_t src_srv, uint32_t width, uint32_t height) {
  if (!presenter_ || !provider_) return;
  rex::system::X_VIDEO_MODE video_mode;
  rex::kernel::xboxkrnl::VdQueryVideoMode(&video_mode);
  const uint32_t display_width = std::max(uint32_t(1), uint32_t(video_mode.display_width));
  const uint32_t display_height = std::max(uint32_t(1), uint32_t(video_mode.display_height));
  presenter_->RefreshGuestOutput(width, height, display_width, display_height,
                                 [this, src_srv, width, height](rex::ui::Presenter::GuestOutputRefreshContext& context) {
                                   auto& ctx = static_cast<rex::ui::d3d12::D3D12Presenter::D3D12GuestOutputRefreshContext&>(context);
                                   context.SetIs8bpc(true);
                                   RendererBlitToPresenter(src_srv, ctx.resource_uav_capable(), width, height);
                                   RendererSubmitCurrentFrame();
                                   presented_this_frame_ = true;
                                   return true;
                                 });
}

void NativeGraphicsSystem::OnSwapDone(uint32_t front_buffer_texture) {
  frame_count_.fetch_add(1, std::memory_order_relaxed);
  rex::SetFrameNumber(rex::GetFrameNumber() + 1);
  if (!presented_this_frame_) PresentFrame(front_buffer_texture);
  presented_this_frame_ = false;
}

void NativeGraphicsSystem::PresentFrame(uint32_t front_buffer_texture) {
  const uint32_t frame = frame_count_.load(std::memory_order_relaxed);
  if (!presenter_ || !provider_) return;
  std::lock_guard<std::mutex> lock(present_mutex_);
  auto& provider = static_cast<rex::ui::d3d12::D3D12Provider&>(*provider_);
  if (!output_) {
    output_ = std::make_unique<FrameOutput>();
    if (!output_->Initialize(provider.GetDevice())) {
      REXLOG_ERROR("Native renderer: failed to create the frame output objects");
      output_.reset();
      return;
    }
  }
  if (frame == 1 || frame % 600 == 0) {
    REXLOG_INFO("Native renderer: Swap #{} (front buffer texture {:08X}, ring kicks {}, packets walked {})", frame,
                front_buffer_texture, ring_kicks_.load(std::memory_order_relaxed),
                ring_packets_.load(std::memory_order_relaxed));
  }

  rex::system::X_VIDEO_MODE video_mode;
  rex::kernel::xboxkrnl::VdQueryVideoMode(&video_mode);
  const uint32_t display_width = std::max(uint32_t(1), uint32_t(video_mode.display_width));
  const uint32_t display_height = std::max(uint32_t(1), uint32_t(video_mode.display_height));
  constexpr uint32_t kWidth = 1280, kHeight = 720;

  presenter_->RefreshGuestOutput(
      kWidth, kHeight, display_width, display_height,
      [this, &provider, frame](rex::ui::Presenter::GuestOutputRefreshContext& context) -> bool {
        auto& ctx = static_cast<rex::ui::d3d12::D3D12Presenter::D3D12GuestOutputRefreshContext&>(context);
        FrameOutput& out = *output_;
        context.SetIs8bpc(true);
        if (!out.EnsureTexture(provider, kWidth, kHeight)) return false;
        out.WaitIdle();
        out.allocator->Reset();
        out.list->Reset(out.allocator.Get(), nullptr);
        ID3D12Resource* guest_output = ctx.resource_uav_capable();

        // Milestone 0 placeholder frame: a static dark slate (the draw calls are
        // counted, not executed yet); a one-second blink of the top-left corner
        // would be the only motion, so keep it plain until milestone 1 draws.
        (void)frame;
        const float colour[4] = {0.06f, 0.08f, 0.10f, 1.0f};
        if (out.texture_state != D3D12_RESOURCE_STATE_RENDER_TARGET) {
          auto b = FrameOutput::Transition(out.texture.Get(), out.texture_state, D3D12_RESOURCE_STATE_RENDER_TARGET);
          out.list->ResourceBarrier(1, &b);
          out.texture_state = D3D12_RESOURCE_STATE_RENDER_TARGET;
        }
        out.list->ClearRenderTargetView(out.rtv_heap->GetCPUDescriptorHandleForHeapStart(), colour, 0, nullptr);
        D3D12_RESOURCE_BARRIER to_copy[2] = {
            FrameOutput::Transition(out.texture.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
                                    D3D12_RESOURCE_STATE_COPY_SOURCE),
            FrameOutput::Transition(guest_output, rex::ui::d3d12::D3D12Presenter::kGuestOutputInternalState,
                                    D3D12_RESOURCE_STATE_COPY_DEST)};
        out.list->ResourceBarrier(2, to_copy);
        out.texture_state = D3D12_RESOURCE_STATE_COPY_SOURCE;
        out.list->CopyResource(guest_output, out.texture.Get());
        auto back = FrameOutput::Transition(guest_output, D3D12_RESOURCE_STATE_COPY_DEST,
                                            rex::ui::d3d12::D3D12Presenter::kGuestOutputInternalState);
        out.list->ResourceBarrier(1, &back);
        out.list->Close();
        ID3D12CommandList* lists[] = {out.list.Get()};
        provider.GetDirectQueue()->ExecuteCommandLists(1, lists);
        provider.GetDirectQueue()->Signal(out.fence.Get(), ++out.fence_value);
        return true;
      });
}

}  // namespace dp::native
