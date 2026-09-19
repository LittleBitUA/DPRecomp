// Deadly Premonition Recompilation - native renderer (experimental, 2026-09-18).
//
// Milestone 0 ("skeleton"): a graphics system that replaces the Xenos
// emulator plugin when the launcher switch `dp_native_render` is on. It owns
// the host D3D12 device and the SDK presenter, keeps the guest's vblank
// interrupt chain alive (the game's frame limiter waits on it), answers the
// few GPU registers the XDK reads, and drains the XDK's ring buffer so that
// any D3D function that is not hooked yet writes its packets into the void
// instead of blocking. No PM4 packet is ever parsed here.
//
// Design rules carried over from the Downpour attempt (docs/native_render_
// design_2026-09-18.md section F.5): one architecture, commands in submission
// order, resources owned from the Create hooks, no ungated guards in the draw
// path, and the default (emulator) path costs exactly one branch per hook.
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>

#include <rex/system/interfaces/graphics.h>
#include <rex/system/xthread.h>
#include <rex/ui/graphics_provider.h>
#include <rex/ui/presenter.h>

namespace rex::memory {
class Memory;
}
namespace rex::runtime {
class FunctionDispatcher;
}
namespace rex::ui::d3d12 {
class D3D12Provider;
}

namespace dp::native {

// True when the game runs on the native renderer (read once at startup from
// the `dp_native_render` cvar; the launcher writes it to deadlyprem.toml).
bool Enabled();

class NativeGraphicsSystem final : public rex::system::IGraphicsSystem {
 public:
  NativeGraphicsSystem();
  ~NativeGraphicsSystem() override;

  rex::X_STATUS SetupPresentation(rex::ui::WindowedAppContext* app_context) override;
  rex::X_STATUS SetupGuestGpu(rex::runtime::FunctionDispatcher* function_dispatcher,
                         rex::system::KernelState* kernel_state) override;
  bool has_presentation() const override { return presenter_ != nullptr; }
  rex::ui::GraphicsProvider* provider() const override;
  rex::ui::Presenter* presenter() const override { return presenter_.get(); }
  void SetInterruptCallback(uint32_t callback, uint32_t user_data) override;
  void InitializeRingBuffer(uint32_t ptr, uint32_t size_log2) override;
  void EnableReadPointerWriteBack(uint32_t ptr, uint32_t block_size_log2) override;
  void Shutdown() override;

  // Milestone 0 placeholder frame (static dark slate) when the renderer has
  // no front buffer yet (before the first resolve into it).
  void PresentFrame(uint32_t front_buffer_texture);
  // Milestone 1: presents the renderer's front buffer SRV through the presenter
  // (the renderer's command list is submitted inside the refresh callback).
  void PresentFromRenderer(uint32_t src_srv, uint32_t width, uint32_t height);
  // End of a guest Swap: frame counters, placeholder frame if nothing was presented.
  void OnSwapDone(uint32_t front_buffer_texture);

  uint32_t frame_count() const { return frame_count_.load(std::memory_order_relaxed); }

  static NativeGraphicsSystem* instance() { return instance_; }

 private:
  static uint32_t ReadRegisterThunk(void* ppc_context, void* self, uint32_t addr);
  static void WriteRegisterThunk(void* ppc_context, void* self, uint32_t addr, uint32_t value);
  uint32_t ReadRegister(uint32_t addr);
  void WriteRegister(uint32_t addr, uint32_t value);
  void MarkVblank();

  static NativeGraphicsSystem* instance_;

  std::unique_ptr<rex::ui::GraphicsProvider> provider_;
  std::unique_ptr<rex::ui::Presenter> presenter_;
  rex::ui::WindowedAppContext* app_context_ = nullptr;
  rex::memory::Memory* memory_ = nullptr;
  rex::runtime::FunctionDispatcher* function_dispatcher_ = nullptr;
  rex::system::KernelState* kernel_state_ = nullptr;

  uint32_t interrupt_callback_ = 0;
  uint32_t interrupt_callback_data_ = 0;
  uint32_t ring_ptr_ = 0;
  uint32_t ring_size_log2_ = 0;
  uint32_t read_ptr_writeback_ptr_ = 0;
  std::atomic<uint32_t> ring_kicks_{0};
  // PM4 side-effect walker (no drawing): the XDK's ring-space accounting,
  // fences and callbacks live in memory the command processor writes.
  uint32_t ring_read_index_ = 0;  // dwords
  uint32_t scratch_umsk_ = 0;
  uint32_t scratch_addr_ = 0;
  std::atomic<uint32_t> pending_cp_interrupts_{0};
  std::atomic<uint32_t> ring_packets_{0};
  std::atomic<uint32_t> ring_fence_writes_{0};
  std::atomic<uint32_t> ring_mem_writes_{0};
  std::atomic<uint32_t> ring_scratch_writes_{0};
  std::atomic<uint32_t> ring_ibs_{0};
  std::atomic<uint32_t> ring_alu_vs_writes_{0};
  std::atomic<uint32_t> ring_alu_ps_writes_{0};
  std::atomic<uint32_t> ring_opcodes_[128]{};        // type-3 opcode histogram
  std::atomic<uint32_t> ring_const_loads_{0};        // SET_CONSTANT / LOAD_ALU_CONSTANT packets
  std::atomic<uint32_t> ring_const_dwords_{0};
  std::atomic<uint32_t> ring_const_log_budget_{48};
  uint32_t ring_diag_budget_ = 40;
 public:
  std::string RingStats() const;
 private:
  void WalkRing(uint32_t write_index);
  void WalkPackets(const uint8_t* base, uint32_t capacity_bytes, uint32_t read_bytes, uint32_t write_bytes, int depth);
  void WalkRegisterWrite(uint32_t index, uint32_t value);

  std::atomic<bool> vsync_worker_running_{false};
  rex::system::object_ref<rex::system::XHostThread> vsync_worker_thread_;

  std::atomic<uint32_t> frame_count_{0};
  bool presented_this_frame_ = false;

  // Host-side frame output (D3D12). Opaque here so this header stays free of
  // the D3D12 headers.
  struct FrameOutput;
  std::unique_ptr<FrameOutput> output_;
  std::mutex present_mutex_;
};

}  // namespace dp::native
