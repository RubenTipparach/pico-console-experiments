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

// Texture coordinates for one face's three corners, 0..255 across the
// texture. A PARALLEL array rather than fields on MeshFace, so a mesh with no
// texture pays nothing: MeshFace stays 12 bytes and the pointer below is null.
// Folding six bytes into every face would have cost every model in the repo
// half again its face table to serve the two that are textured.
struct MeshUv {
    uint8_t u0, v0, u1, v1, u2, v2;
};

struct MeshData {
    const MeshVertex* vertices;
    uint16_t vertex_count;
    const MeshFace* faces;
    uint16_t face_count;
    int16_t scale;
    // One entry per face, or null when the model carried no `vt` data.
    const MeshUv* uvs = nullptr;
};

}  // namespace pse
