#pragma once

#include <cstdint>

#include "pse/config.hpp"
#include "pse/pixel.hpp"

namespace pse {

// A triangle already in screen space, with per vertex colour for Gouraud
// shading. Depth is in the projector's fixed point range.
struct ScreenTriangle {
    int16_t x0, y0;
    int16_t x1, y1;
    int16_t x2, y2;
    uint16_t z0, z1, z2;
    uint8_t r0, g0, b0;
    uint8_t r1, g1, b1;
    uint8_t r2, g2, b2;
};

// Immediate mode, z buffered triangle rasterizer. Knows nothing about any SDK,
// which is what makes it compile unchanged for device, desktop, and web.
//
// Immediate mode rather than a deferred triangle list: the picosystem SDK
// version of this renderer queued up to 1500 triangles so a second core could
// consume them, which cost 84 KB of RAM in list storage. Under the 32blit SDK
// core1 is not reliably ours (the backend takes it for display and audio when
// ENABLE_CORE1 is set), so there is no consumer to queue for. Drawing straight
// through the depth buffer gives the same image and hands the 84 KB back.
class Rasterizer {
public:
    Rasterizer() = default;

    Rasterizer(const Rasterizer&) = delete;
    Rasterizer& operator=(const Rasterizer&) = delete;

    // Point the rasterizer at this frame's surface and clear the depth buffer.
    void begin_frame(const RenderTarget& target);

    // Draw one triangle. Backfaces are culled, coordinates are clipped to the
    // target, and the depth test is 8 bit.
    void draw(const ScreenTriangle& tri);

    // Fill every pixel with a vertical gradient. Cheaper than clearing and then
    // drawing a sky, because it is the same single pass.
    void clear_gradient(uint8_t top_r, uint8_t top_g, uint8_t top_b,
                        uint8_t bottom_r, uint8_t bottom_g, uint8_t bottom_b);

    // Depth test and claim a pixel, for sprites and billboards drawn after the
    // geometry. Returns false when something nearer already owns the pixel.
    bool test_and_set_depth(int x, int y, uint8_t depth);

    // Write one pixel in the target's format, no depth test.
    void plot(int x, int y, uint8_t r, uint8_t g, uint8_t b);

    uint32_t triangles_drawn() const { return triangles_drawn_; }

    const RenderTarget& target() const { return target_; }

private:
    template <typename Format>
    void draw_typed(const ScreenTriangle& tri);

    template <typename Format>
    void clear_gradient_typed(uint8_t top_r, uint8_t top_g, uint8_t top_b,
                              uint8_t bottom_r, uint8_t bottom_g,
                              uint8_t bottom_b);

    uint8_t* pixel_at(int x, int y) const;

    RenderTarget target_{nullptr, 0, 0, 0, PixelFormat::rgb565};
    uint32_t triangles_drawn_ = 0;

    // 14,400 bytes at the default 120x120. This is the renderer's single
    // largest RAM cost and it is deliberately static.
    uint8_t depth_[k_render_width * k_render_height];
};

}  // namespace pse
