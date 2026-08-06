#pragma once

// Texture mapping, for the case where detail is cheaper as pixels than as
// polygons.
//
// The engine went without one for a long time, and the reasoning was that a
// game with no texture unit can put detail in per face colour instead. That is
// true and it is also expensive: a banded tower facade cost 42 triangles to
// say what one quad and a picture say, and 32 of those were slivers covering
// twenty pixels each. A sliver is the worst thing a scanline rasterizer can be
// given. It pays the full per triangle bill (a bounding box, three edge
// setups, three divides for the reciprocal depths) and three software float
// vertex transforms, and then fills almost nothing.
//
// What a textured pixel costs here is only multiplies. draw_typed already
// computes the barycentrics and the perspective corrected depth for every
// pixel it fills, so perspective correct u and v ride along on work that has
// already been done and add no divide to the inner loop. That is the whole
// reason this is worth having on a chip with no divide instruction.
//
// Textures are const, live in flash, and are read through the XIP cache. Never
// copy one into RAM: a cache hit already costs what SRAM costs, and RAM is the
// resource this device is short of.

#include <cstddef>
#include <cstdint>

namespace pse {

// A power of two texture, three bytes per texel, row major.
//
// Power of two is not a convenience, it is the point: sampling is a shift and
// a mask, so wrapping is free and there is no divide or modulo anywhere near
// the inner loop. 32x32 is 3 KB of flash, which is nothing against 12 MB, and
// is more detail than a 120x120 screen can show on a building anyway.
struct Texture {
    const uint8_t* texels;   // width * height * 3
    uint8_t w_shift;         // width  is 1 << w_shift
    uint8_t h_shift;         // height is 1 << h_shift
};

// Fetch, wrapping. u and v are 0..255 across the texture, which is the range
// ScreenTriangle carries them in: a byte is enough for a 32 or 64 wide texture
// and it keeps the queue small.
inline void texture_fetch(const Texture& tex, int u, int v,
                          uint8_t& r, uint8_t& g, uint8_t& b) {
    const int w = 1 << tex.w_shift;
    const int h = 1 << tex.h_shift;
    // Shift the 0..255 coordinate down to the texture's own size, then mask.
    // The mask is what makes a coordinate that ran past the edge wrap instead
    // of reading someone else's flash.
    const int tx = ((u * w) >> 8) & (w - 1);
    const int ty = ((v * h) >> 8) & (h - 1);
    const uint8_t* p = tex.texels + (static_cast<size_t>(ty) * w + tx) * 3;
    r = p[0];
    g = p[1];
    b = p[2];
}

}  // namespace pse
