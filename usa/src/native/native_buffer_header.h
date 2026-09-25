// [new_fix_24092026] 2.0.4: the guest vertex/index buffer headers (GitHub
// issue #35: pillows with triangular holes, door handles and trees pulled into
// long spikes, York's tie showing through his back).
//
// Both are an XDK D3DResource (24 bytes: Common, ReferenceCount, Fence,
// ReadFence, Identifier, BaseFlush) followed by two dwords, which mean
// different things:
//  - D3DVertexBuffer: a vertex fetch constant. +24 = address | fetch type
//    (bits 0-1), +28 = size in DWORDS << 2 | endian (bits 0-1).
//  - D3DIndexBuffer: +24 = address (any 2-byte alignment), +28 = size in BYTES
//    (XGSetIndexBufferHeader's Length). The 16/32-bit format and the endian
//    swap live in Common (+0) bits 31 and 29-30. The XDK DrawIndexedVertices
//    (PAL sub_825CDBD8) reads only +0 and +24: the GPU gets the index count.
// Up to 2.0.3 the index buffer's size went through the vertex formula, which
// rounds it down to a multiple of 4: a buffer with an odd number of 16-bit
// indices lost its last one, the draw read index 0 there (D3D12 returns 0 past
// the end of an index buffer view), and the last triangle of the strip was
// stretched to vertex 0 (a spike) or turned away and culled (a hole).
//
// Header-only and free of D3D so tests/native_buffer_header_test.cpp can check it.
#pragma once

#include <cstdint>

namespace dp::native {

struct BufferHeader {
  uint32_t address = 0;  // guest virtual address of the data
  uint32_t size = 0;     // bytes
  uint32_t endian = 0;   // VB: fetch dword 1 bits 0-1; IB: Common bits 29-30 (1 = 8in16, 2 = 8in32)
  bool index32 = false;  // index buffers only
};

// common = +0, dw0 = +24, dw1 = +28 of the guest header.
inline BufferHeader DecodeBufferHeader(bool index_buffer, uint32_t common, uint32_t dw0, uint32_t dw1) {
  BufferHeader h;
  if (index_buffer) {
    h.address = dw0;
    h.size = dw1;
    h.endian = (common >> 29) & 3u;
    h.index32 = (common & 0x80000000u) != 0;
  } else {
    h.address = dw0 & ~3u;
    h.size = ((dw1 >> 2) & 0xFFFFFFu) * 4u;
    h.endian = dw1 & 3u;
  }
  return h;
}

}  // namespace dp::native
