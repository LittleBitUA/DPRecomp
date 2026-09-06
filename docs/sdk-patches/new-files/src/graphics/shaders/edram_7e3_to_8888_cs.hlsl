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
// Bound with the resolve EDRAM clear root signature: root constants at b0,
// uint4 UAV of the EDRAM buffer at u0.
//
// Build: fxc /T cs_5_1 /E main /O3 /Fh bytecode/d3d12_5_1/edram_7e3_to_8888_cs.h
//            /Vn edram_7e3_to_8888_cs edram_7e3_to_8888_cs.hlsl

cbuffer push_consts_xe : register(b0) {
  // First uint4 (4 dwords = 4 32bpp samples) of the range in the EDRAM buffer.
  uint xe_convert_first_uint4;
  // Number of uint4 elements to convert.
  uint xe_convert_uint4_count;
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

uint XeConvert7e3To8888(uint p) {
  uint r = XeUnorm8(XeFloat7e3To32(p & 0x3FFu));
  uint g = XeUnorm8(XeFloat7e3To32((p >> 10u) & 0x3FFu));
  uint b = XeUnorm8(XeFloat7e3To32((p >> 20u) & 0x3FFu));
  // 2-bit alpha 0..3 -> 0, 85, 170, 255.
  uint a = (p >> 30u) * 85u;
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
    v.x = XeConvert7e3To8888(v.x);
    v.y = XeConvert7e3To8888(v.y);
    v.z = XeConvert7e3To8888(v.z);
    v.w = XeConvert7e3To8888(v.w);
    xe_edram[element] = v;
  }
}
