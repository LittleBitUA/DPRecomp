// [new_fix_03102026_clipplane] Where the XDK device keeps the user clip planes
// (DPRecomp #46, #41).
//
// D3DDevice_SetClipPlane (PAL sub_824CDB80, USA sub_824CD2D0, same body) stores
// plane `index` as four floats at dev + (index + 642) * 16 and sets dirty bit
// (index + 9) of the qword at dev + 32; the XDK flush copies the block into
// PA_CL_UCP_<index> unchanged. Plane 0 therefore lives at dev + 10272, right
// after the loop constants (10144 + 32 dwords).
//
// Up to 2.0.12 the native renderer read dev + 8528, found by scanning one
// floor-reflection frame for the plane's value. That address is pixel shader
// constant c157 (6016 + 157 * 16): a value the game happened to leave equal to
// the plane in that frame and unrelated in every other. All planar reflections
// (the station floor, Emily's mirror, the lake) were clipped by it: the mirror
// pass kept the wall behind the mirror and lost York (#46), the lake kept
// what is under the water (#41).
#pragma once

#include <cstdint>

namespace dp::native {

inline constexpr uint32_t kDevClipPlaneBase = 642;    // dev + (642 + i) * 16
inline constexpr uint32_t kDevClipPlaneCount = 6;     // PA_CL_UCP_0..5

constexpr uint32_t DevClipPlaneOffset(uint32_t index) { return (kDevClipPlaneBase + index) * 16; }

}  // namespace dp::native
