// [new_fix_24092026]
// Deadly Premonition Recompilation - native renderer: the console's display
// gamma ramp.
//
// On the Xbox 360 the display controller passes the front buffer through a
// gamma ramp before scan-out. The XDK programs it through the DC_LUT registers
// (256-entry table for an 8_8_8_8 front buffer, PWL for 2_10_10_10), adjusted
// to the display type the kernel reports through VdGetCurrentDisplayGamma; the
// SDK reports 2 = TV (BT.709). The emulated path captures those writes in the
// command processor and applies the ramp at swap (rexglue-sdk
// src/graphics/command_processor.cpp, d3d12/command_processor.cpp). This is
// the same capture for the native renderer, which has no command processor:
// NativeGraphicsSystem feeds it every guest register write (ring and MMIO).
//
// Pure state machine, tested without a GPU (tests/native_gamma_test.cpp).
#pragma once

#include <array>
#include <cstdint>

namespace dp::native {

// Register indices (rexglue-sdk include/rex/graphics/register_table.inc).
inline constexpr uint32_t kRegDcLutRwMode = 0x1921;
inline constexpr uint32_t kRegDcLutRwIndex = 0x1922;
inline constexpr uint32_t kRegDcLutSeqColor = 0x1923;
inline constexpr uint32_t kRegDcLutPwlData = 0x1924;
inline constexpr uint32_t kRegDcLut30Color = 0x1925;
inline constexpr uint32_t kRegDcLutWriteEnMask = 0x1927;

// DC_LUT_30_COLOR layout: blue bits 0-9, green 10-19, red 20-29.
constexpr uint32_t PackLut30(uint32_t r, uint32_t g, uint32_t b) {
  return (b & 0x3FF) | ((g & 0x3FF) << 10) | ((r & 0x3FF) << 20);
}
constexpr uint32_t Lut30Red(uint32_t v) { return (v >> 20) & 0x3FF; }
constexpr uint32_t Lut30Green(uint32_t v) { return (v >> 10) & 0x3FF; }
constexpr uint32_t Lut30Blue(uint32_t v) { return v & 0x3FF; }
// The same entry as DXGI_FORMAT_R10G10B10A2_UNORM (red in bits 0-9), alpha 1.
constexpr uint32_t Lut30ToR10G10B10A2(uint32_t v) {
  return Lut30Red(v) | (Lut30Green(v) << 10) | (Lut30Blue(v) << 20) | (3u << 30);
}

class GuestGammaRamp {
 public:
  // The SDK's default: the identity the XDK sets for an sRGB display
  // (i * 0x3FF / 0xFF in every channel).
  GuestGammaRamp() { Reset(); }
  void Reset() {
    for (uint32_t i = 0; i < 256; ++i) {
      const uint32_t v = i * 0x3FF / 0xFF;
      table_[i] = PackLut30(v, v, v);
    }
    rw_mode_ = 0;
    rw_index_ = 0;
    write_en_mask_ = 7;
    component_ = 0;
    version_ = 0;
    pwl_writes_ = 0;
    writes_ = 0;
  }

  // One guest register write. Returns true when the 256-entry table changed.
  bool Write(uint32_t index, uint32_t value) {
    ++writes_;
    switch (index) {
      case kRegDcLutRwMode:
        rw_mode_ = value;
        return false;
      case kRegDcLutRwIndex:
        rw_index_ = value & 0xFF;
        component_ = 0;  // resets the sequential component (M56 DC_LUT_SEQ_COLOR)
        return false;
      case kRegDcLutWriteEnMask:
        write_en_mask_ = value & 7;
        return false;
      case kRegDcLut30Color: {
        if (rw_mode_ & 1) return false;  // PWL mode: not the table
        bool changed = false;
        if (write_en_mask_) {
          uint32_t& e = table_[rw_index_];
          const uint32_t old = e;
          const uint32_t r = (write_en_mask_ & 4) ? Lut30Red(value) : Lut30Red(e);
          const uint32_t g = (write_en_mask_ & 2) ? Lut30Green(value) : Lut30Green(e);
          const uint32_t b = (write_en_mask_ & 1) ? Lut30Blue(value) : Lut30Blue(e);
          e = PackLut30(r, g, b);
          changed = e != old;
        }
        rw_index_ = (rw_index_ + 1) & 0xFF;  // a full DC_LUT_RW_INDEX write in the SDK
        component_ = 0;
        if (changed) ++version_;
        return changed;
      }
      case kRegDcLutSeqColor: {
        if (rw_mode_ & 1) return false;
        // Red, green, blue in that order; the write-enable mask is blue, green, red.
        bool changed = false;
        if (write_en_mask_ & (1u << (2 - component_))) {
          uint32_t& e = table_[rw_index_];
          const uint32_t old = e;
          const uint32_t c = (value & 0xFFFF) >> 6;  // bits 0:5 hardwired to zero
          uint32_t r = Lut30Red(e), g = Lut30Green(e), b = Lut30Blue(e);
          if (component_ == 0) r = c; else if (component_ == 1) g = c; else b = c;
          e = PackLut30(r, g, b);
          changed = e != old;
        }
        if (++component_ >= 3) {
          component_ = 0;
          rw_index_ = (rw_index_ + 1) & 0xFF;
        }
        if (changed) ++version_;
        return changed;
      }
      case kRegDcLutPwlData:
        ++pwl_writes_;  // an 8_8_8_8 front buffer uses the table; PWL is counted only
        return false;
      default:
        return false;
    }
  }

  const std::array<uint32_t, 256>& table() const { return table_; }
  uint32_t version() const { return version_; }
  uint32_t pwl_writes() const { return pwl_writes_; }
  uint32_t writes() const { return writes_; }  // every DC_LUT register write seen
  bool IsIdentity() const {
    for (uint32_t i = 0; i < 256; ++i) {
      const uint32_t v = i * 0x3FF / 0xFF;
      if (table_[i] != PackLut30(v, v, v)) return false;
    }
    return true;
  }

 private:
  std::array<uint32_t, 256> table_{};
  uint32_t rw_mode_ = 0, rw_index_ = 0, write_en_mask_ = 7, component_ = 0;
  uint32_t version_ = 0, pwl_writes_ = 0, writes_ = 0;
};

}  // namespace dp::native
