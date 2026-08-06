#pragma once

#include <cstdint>

#include "pse/config.hpp"
#include "pse/pixel.hpp"
#include "pse/texture.hpp"

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

    // Texturing, optional. 0 means untextured and Gouraud shaded exactly as
    // this always was, which is what every existing caller leaves it at.
    // Otherwise it is a 1 based index into the table the Rasterizer was given.
    //
    // An INDEX and not a pointer, deliberately. A pointer is 8 bytes and forces
    // this struct to 8 byte alignment, which took a queued triangle from 28
    // bytes to 48 and the 640 triangle queue from 17.9 KB to 30.7 KB. Every
    // member here is one or two bytes wide, so an index keeps the alignment at
    // 2 and the queue at 21.8 KB, and it costs one indirection per textured
    // triangle rather than per pixel.
    //
    // The texel MULTIPLIES the interpolated vertex colour rather than replacing
    // it, so a textured face is still lit by the same lambert every other face
    // is lit by. A texture that ignored the shading would make a building the
    // one object in the scene with no light on it.
    //
    // u and v are 0..255 across the texture. A byte is enough for the sizes
    // that fit a 120 pixel screen and it keeps the queue small.
    uint8_t tex = 0;
    uint8_t u0 = 0, v0 = 0;
    uint8_t u1 = 0, v1 = 0;
    uint8_t u2 = 0, v2 = 0;
};

// A frame's worth of collected triangles, for split rasterization across two
// cores. Fixed capacity: on a 264 KB device an unbounded queue is a crash with
// extra steps. Overflow drops the triangle and counts it, so a scene that gets
// too heavy degrades visibly rather than corrupting memory.
//
// A frame can hold either one scene or two. mark_split() ends the first
// scene: triangles queued before it belong to the top band of the screen,
// triangles after it to the bottom band, and run_split renders each group
// only into its own band. Without a mark the whole queue renders into both
// bands, which is the classic single scene split.
struct FrameQueue {
    static constexpr int k_capacity = PSE_MAX_QUEUE;
    static constexpr uint16_t k_no_split = 0xFFFF;

    ScreenTriangle tris[k_capacity];
    uint16_t count = 0;
    uint16_t dropped = 0;
    uint16_t split = k_no_split;

    void reset() { count = 0; dropped = 0; split = k_no_split; }

    void mark_split() { split = count; }

    bool push(const ScreenTriangle& tri) {
        if (count >= k_capacity) {
            dropped++;
            return false;
        }
        tris[count++] = tri;
        return true;
    }
};

// Immediate mode, z buffered triangle rasterizer. Knows nothing about any SDK,
// which is what makes it compile unchanged for device, desktop, and web.
//
// Two ways to drive it:
//
//   Immediate: begin_frame() then draw(). Each triangle rasterizes on the
//   calling core as it arrives.
//
//   Collect: begin_frame_collect() then draw(). Triangles are culled and
//   queued instead of rasterized, and pse::run_split() later rasterizes the
//   queue with each core owning a disjoint band of rows. Same draw() calls,
//   same image, so game code does not change between the two.
class Rasterizer {
public:
    Rasterizer() = default;

    Rasterizer(const Rasterizer&) = delete;
    Rasterizer& operator=(const Rasterizer&) = delete;

    // Point the rasterizer at this frame's surface and clear the depth buffer.
    void begin_frame(const RenderTarget& target);

    // The textures a ScreenTriangle's `tex` index resolves against. Held by
    // the Rasterizer rather than reached for globally, so the game owns its
    // art and the engine only renders it. Read only during a frame, which is
    // why both split workers can share it with no lock.
    //
    // `table` must outlive the frame. Passing nullptr, or leaving this unset,
    // makes every triangle untextured, which is what a game with no textures
    // gets for free.
    void set_textures(const Texture* table, uint8_t count) {
        textures_ = table;
        texture_count_ = count;
    }

    // Point at the surface and route draw() into the queue. The depth buffer is
    // NOT cleared here: the split workers each clear their own rows, which
    // parallelizes the clear as well.
    void begin_frame_collect(const RenderTarget& target, FrameQueue& queue);

    // Stop queueing. Draw() rasterizes immediately again, which is how
    // billboards and UI are drawn on top after the split workers finish.
    void end_collect();

    // Draw one triangle: immediately, or into the queue in collect mode.
    // Backfaces are culled and offscreen triangles rejected in both modes.
    void draw(const ScreenTriangle& tri);

    // Rasterize one triangle clipped to rows [row_begin, row_end). Used by the
    // split workers. Touches only those rows of the framebuffer and depth
    // buffer, which is the whole thread safety argument: two cores calling
    // this with disjoint row ranges never write the same memory. Does not
    // update the triangle counter, because two cores would race on it.
    void draw_rows(const ScreenTriangle& tri, int row_begin, int row_end);

    // Clear depth for rows [row_begin, row_end) only.
    void clear_depth_rows(int row_begin, int row_end);

    // Fill rows [row_begin, row_end) with the frame's vertical gradient. The
    // gradient is computed against the full target height, so two workers
    // filling adjacent bands produce one continuous gradient.
    void clear_gradient_rows(uint8_t top_r, uint8_t top_g, uint8_t top_b,
                             uint8_t bottom_r, uint8_t bottom_g,
                             uint8_t bottom_b, int row_begin, int row_end);

    // Same fill, but the gradient lerps across [span_begin, span_end) instead
    // of the full height. A split screen scene uses this so its band carries
    // a complete gradient of its own.
    void clear_gradient_span(uint8_t top_r, uint8_t top_g, uint8_t top_b,
                             uint8_t bottom_r, uint8_t bottom_g,
                             uint8_t bottom_b, int row_begin, int row_end,
                             int span_begin, int span_end);

    // Fill every pixel with a vertical gradient (immediate mode convenience).
    void clear_gradient(uint8_t top_r, uint8_t top_g, uint8_t top_b,
                        uint8_t bottom_r, uint8_t bottom_g, uint8_t bottom_b);

    // Depth test and claim a pixel, for sprites and billboards drawn after the
    // geometry. Returns false when something nearer already owns the pixel.
    bool test_and_set_depth(int x, int y, uint8_t depth);

    // Write one pixel in the target's format, no depth test.
    void plot(int x, int y, uint8_t r, uint8_t g, uint8_t b);

    uint32_t triangles_drawn() const { return triangles_drawn_; }

    const RenderTarget& target() const { return target_; }

    const Texture* texture(uint8_t index) const {
        if (index == 0 || textures_ == nullptr || index > texture_count_) {
            return nullptr;
        }
        return &textures_[index - 1];
    }

private:
    template <typename Format>
    void draw_typed(const ScreenTriangle& tri, int row_begin, int row_end);

    template <typename Format>
    void clear_gradient_typed(uint8_t top_r, uint8_t top_g, uint8_t top_b,
                              uint8_t bottom_r, uint8_t bottom_g,
                              uint8_t bottom_b, int row_begin, int row_end,
                              int span_begin, int span_end);

    // True when the triangle is a backface or entirely outside the target.
    bool rejected(const ScreenTriangle& tri) const;

    uint8_t* pixel_at(int x, int y) const;

    RenderTarget target_{nullptr, 0, 0, 0, PixelFormat::rgb565};
    FrameQueue* queue_ = nullptr;
    uint32_t triangles_drawn_ = 0;

    // 14,400 bytes at the default 120x120. This is the renderer's single
    // largest RAM cost and it is deliberately static.
    uint8_t depth_[k_render_width * k_render_height];
    const Texture* textures_ = nullptr;
    uint8_t texture_count_ = 0;
};

}  // namespace pse
