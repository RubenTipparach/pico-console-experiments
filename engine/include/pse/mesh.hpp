#pragma once

#include <cstdint>

namespace pse {

// Geometry emitted by tools/obj2cpp.py. Every field is sized so a model lives
// in flash as a const table and is read straight through the XIP cache. Do not
// copy a mesh into RAM: that spends the one resource the device is short of and
// buys nothing, because a cache hit already costs what SRAM costs.

// Position in fixed point. `MeshData::scale` says how many of these units make
// one world unit.
struct MeshVertex {
    int16_t x, y, z;
};

// One triangle: three vertex indices, a flat colour, and a quantised normal.
// 12 bytes, no padding.
struct MeshFace {
    uint16_t i0, i1, i2;
    uint8_t r, g, b;
    int8_t nx, ny, nz;
};

struct MeshData {
    const MeshVertex* vertices;
    uint16_t vertex_count;
    const MeshFace* faces;
    uint16_t face_count;
    int16_t scale;
};

}  // namespace pse
