// DP1 fix (pixel shader interlock / ROV path): in-place reinterpretation of an
// EDRAM tile range last written as 7e3 color (k_2_10_10_10_FLOAT or
// k_2_10_10_10_FLOAT_AS_16_16_16_16) into k_8_8_8_8 with value clipping.
//
// This is the ROV counterpart of the host-render-target ownership transfer fix
// in d3d12/render_target_cache.cpp (GetOrCreateTransferPipelines): with host
// render targets the 7e3 -> 8888 transfer shader saturates the unpacked floats
// and writes them as UNORM8; with ROV the EDRAM buffer is shared bit-for-bit,
// so the same conversion has to be applied to the buffer contents themselves
// right before the first 8888 draw over tiles still holding 7e3 data.
//
// DP1 shadow alpha (2026-09-07): the second half of the EDRAM buffer holds a
// shadow dword per 32bpp sample for 7e3 render targets - 16-bit UNORM alpha in
// the low half, a 16-bit tag of the main dword (low 16 bits XOR high 16 bits)
// in the high half. When the tag matches and the 16-bit alpha quantizes to the
// 2-bit alpha of the main dword, the 8-bit alpha is taken from the shadow
// (smooth), otherwise from the 2 bits (0, 85, 170, 255).
//
// Bound with the resolve EDRAM clear root signature: root constants at b0,
// uint4 UAV of the EDRAM buffer at u0.
//
// Build (PowerShell, not Git Bash):
//   fxc /T cs_5_1 /E main /O3 /Fh bytecode/d3d12_5_1/edram_7e3_to_8888_cs.h
//       /Vn edram_7e3_to_8888_cs edram_7e3_to_8888_cs.hlsl

cbuffer push_consts_xe : register(b0) {
  // First uint4 (4 dwords = 4 32bpp samples) of the range in the EDRAM buffer.
  uint xe_convert_first_uint4;
  // Number of uint4 elements to convert.
  uint xe_convert_uint4_count;
  // uint4 offset of the shadow alpha half of the buffer.
  uint xe_convert_shadow_uint4_offset;
};

RWBuffer<uint4> xe_edram : register(u0);

// Unsigned 7e3 float (7-bit mantissa, 3-bit exponent, bias 3) to float32.
float XeFloat7e3To32(uint f10) {
  uint mantissa = f10 & 0x7Fu;
  uint exponent = f10 >> 7u;
  return exponent != 0u
             ? ldexp(1.0f + float(mantissa) * (1.0f / 128.0f), int(exponent) - 3)
             : float(mantissa) * (1.0f / 512.0f);
}

uint XeUnorm8(float v) { return uint(saturate(v) * 255.0f + 0.5f); }

// 8-bit alpha from the shadow dword if it is valid for the main dword,
// otherwise from the 2-bit alpha of the main dword.
uint XeShadowAlpha8(uint p, uint shadow) {
  uint tag = (p ^ (p >> 16u)) & 0xFFFFu;
  uint alpha16 = shadow & 0xFFFFu;
  bool valid = (shadow >> 16u) == tag &&
               ((alpha16 * 3u + 32767u) / 65535u) == (p >> 30u);
  return valid ? (alpha16 * 255u + 32767u) / 65535u : (p >> 30u) * 85u;
}

uint XeConvert7e3To8888(uint p, uint shadow) {
  uint r = XeUnorm8(XeFloat7e3To32(p & 0x3FFu));
  uint g = XeUnorm8(XeFloat7e3To32((p >> 10u) & 0x3FFu));
  uint b = XeUnorm8(XeFloat7e3To32((p >> 20u) & 0x3FFu));
  uint a = XeShadowAlpha8(p, shadow);
  return r | (g << 8u) | (b << 16u) | (a << 24u);
}

#define XE_CONVERT_GROUP_SIZE 64u
#define XE_CONVERT_UINT4_PER_THREAD 4u

[numthreads(XE_CONVERT_GROUP_SIZE, 1, 1)]
void main(uint3 xe_group_id : SV_GroupID, uint3 xe_group_thread_id : SV_GroupThreadID) {
  uint group_first = xe_group_id.x * (XE_CONVERT_GROUP_SIZE * XE_CONVERT_UINT4_PER_THREAD);
  [unroll]
  for (uint j = 0u; j < XE_CONVERT_UINT4_PER_THREAD; ++j) {
    // Coalesced: consecutive threads touch consecutive elements.
    uint index = group_first + j * XE_CONVERT_GROUP_SIZE + xe_group_thread_id.x;
    if (index >= xe_convert_uint4_count) {
      return;
    }
    uint element = xe_convert_first_uint4 + index;
    uint4 v = xe_edram[element];
    uint4 s = xe_edram[element + xe_convert_shadow_uint4_offset];
    v.x = XeConvert7e3To8888(v.x, s.x);
    v.y = XeConvert7e3To8888(v.y, s.y);
    v.z = XeConvert7e3To8888(v.z, s.z);
    v.w = XeConvert7e3To8888(v.w, s.w);
    xe_edram[element] = v;
  }
}
