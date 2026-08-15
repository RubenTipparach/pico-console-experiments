#pragma once

// The 2D half of the engine: shapes and sprites, drawn into a RenderTarget.
//
// This exists for the same reason pse::draw_text does. A game that draws its
// world with the SDK's `screen.line` and `screen.blit` cannot be rendered on a
// host, which means its screen cannot be photographed, cannot be regression
// tested, and does not appear in the preview harness at all. CLAUDE.md says as
// much: a HUD drawn with SDK calls is unverified until somebody runs the game.
// Everything here is plain C++ against a borrowed surface, so the same code
// draws the device's framebuffer and the test suite's buffer.
//
// Interface segregation (rule 7) is why this is not in raster.hpp: a 2D game
// links these few hundred bytes and never pulls in the triangle rasterizer,
// the projector or the depth buffer.
//
// plot_pixel and fill_rect live here rather than in text.hpp, where they
// started. They are shapes, text.cpp is a consumer of them like anything else,
// and text.hpp includes this header so nothing that used to include it needs
// to change.

#include <cstdint>

#include "pse/pixel.hpp"

namespace pse {

// One pixel, clipped. No depth test, no blending.
void plot_pixel(const RenderTarget& target, int x, int y, uint8_t r, uint8_t g,
                uint8_t b);

// Solid rectangle, clipped.
void fill_rect(const RenderTarget& target, int x, int y, int w, int h,
               uint8_t r, uint8_t g, uint8_t b);

// The same rectangle, MIXED with what is already there. `alpha` is the weight
// of the new colour: 0 leaves the target untouched, 255 is fill_rect, 128 is
// half and half.
//
// This is the one function in the engine that reads the framebuffer back, and
// the exception is deliberate and bounded. The rasterizer does not blend and
// must not start: it fills triangles, millions of pixels a second on a core
// with no cache, and a read-modify-write in that inner loop is not a cost, it
// is the cost. That argument does not carry over to an overlay. A panel is
// thousands of pixels ONCE a frame, drawn in immediate mode after the split
// workers have finished, and being able to see the game through a results
// board is worth a framebuffer read per pixel of the board.
//
// It costs the precision of the target format, and on a 16 bit screen that is
// visible: the result is quantised on the way back out, so a gradient blended
// under a panel bands slightly more than the gradient does. That is the price
// of the format, not of the blend.
//
// NOT SAFE DURING A COLLECTED FRAME. It reads pixels the split rasterizer may
// still be writing. Call it after end_collect, where every other overlay in
// this repo already lives.
void blend_rect(const RenderTarget& target, int x, int y, int w, int h,
                uint8_t r, uint8_t g, uint8_t b, uint8_t alpha);

// One pixel outline, clipped. `w` and `h` are the outside measurements, so a
// 10 wide rect covers x .. x+9.
void draw_rect(const RenderTarget& target, int x, int y, int w, int h,
               uint8_t r, uint8_t g, uint8_t b);

// Axis aligned runs. Both ends are inclusive and either order is accepted,
// because a caller working out a span rarely knows which end is smaller.
// These are the ones worth having separately: a horizontal run is a single
// clipped loop with no error term, and terrain, bars and panels are made of
// almost nothing else.
void h_line(const RenderTarget& target, int x0, int x1, int y, uint8_t r,
            uint8_t g, uint8_t b);
void v_line(const RenderTarget& target, int x, int y0, int y1, uint8_t r,
            uint8_t g, uint8_t b);

// Bresenham, both ends inclusive.
void draw_line(const RenderTarget& target, int x0, int y0, int x1, int y1,
               uint8_t r, uint8_t g, uint8_t b);

// Filled and outlined circles. `radius` is in pixels and a radius of 0 is a
// single pixel, which is what a caller shrinking something to nothing wants.
void fill_circle(const RenderTarget& target, int cx, int cy, int radius,
                 uint8_t r, uint8_t g, uint8_t b);
void draw_circle(const RenderTarget& target, int cx, int cy, int radius,
                 uint8_t r, uint8_t g, uint8_t b);

// A picture in flash.
//
// RGBA, row major, one byte a channel, and alpha is a mask rather than a
// blend: below k_alpha_threshold the pixel is not written at all. There is no
// blending because there is no framebuffer read in the inner loop, and on a
// core with no divide a read-modify-write per pixel is not what a 240x240
// screen has the budget for.
//
// Four bytes a pixel rather than three plus a colour key, because flash is the
// resource this device has 12 MB of and a colour key is a thing an artist has
// to remember not to paint with. tools/png2cpp.py --kind sprite writes these
// from a real PNG at build time; nothing generated is committed (rule 11).
struct Sprite {
    const uint8_t* pixels;   // w * h * 4
    int16_t w;
    int16_t h;
};

constexpr uint8_t k_alpha_threshold = 128;

// Blit a region of `sprite` with its top left at (x, y), clipped.
//
// The region is how a sheet is used: one PNG carries every frame, and the
// caller names the cell. `flip_x` mirrors the region as it is drawn, which
// costs nothing here and saves an artist drawing a thing twice.
void blit_sprite(const RenderTarget& target, const Sprite& sprite, int sx,
                 int sy, int sw, int sh, int x, int y, bool flip_x = false);

// The whole sprite, for a picture that is not a sheet.
void blit_sprite(const RenderTarget& target, const Sprite& sprite, int x,
                 int y, bool flip_x = false);

}  // namespace pse
