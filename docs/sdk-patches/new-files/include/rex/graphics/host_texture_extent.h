// DP1 2026-09-14 (DPRecomp #22): host resource size of a guest texture -
// the guest size, multiplied by the draw resolution scale when the texture is
// loaded from the scaled resolve buffer (TextureKey::scaled_resolve). Every
// resource the loader writes into must be created through this: CreateTexture
// and the 3D-as-2D wrapper in both the D3D12 and the Vulkan texture cache. In
// #22 the D3D12 wrapper used the unscaled size and the scaled 64x64 upload
// into its 32x32 level removed the device. No platform headers on purpose.
#pragma once

#include <cstdint>

namespace rex::gpu::texture_copy {

struct Extent2 {
  uint32_t width, height;
};

inline Extent2 HostResourceExtent(uint32_t guest_width, uint32_t guest_height, bool scaled_resolve,
                                  uint32_t draw_resolution_scale_x,
                                  uint32_t draw_resolution_scale_y) {
  Extent2 e{guest_width, guest_height};
  if (scaled_resolve) {
    e.width *= draw_resolution_scale_x;
    e.height *= draw_resolution_scale_y;
  }
  return e;
}

}  // namespace rex::gpu::texture_copy
