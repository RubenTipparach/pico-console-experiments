#pragma once

#include "pse/raster.hpp"

namespace pse {

// The frame's background gradient, passed to the workers so the clear runs in
// parallel with everything else.
struct SkyGradient {
    uint8_t top_r, top_g, top_b;
    uint8_t bottom_r, bottom_g, bottom_b;
};

// Rasterize rows [row_begin, row_end) of a collected frame using triangles
// [tri_begin, tri_end): clear that band's depth, fill its slice of the
// gradient, then draw each triangle clipped to the band. The gradient lerps
// across [gradient_begin, gradient_end), which lets a scene band carry its
// own full gradient while a single scene frame keeps one gradient across
// bands rendered by different cores. Pure code, same on every platform, which
// is what makes the split testable on a host: two bands must reproduce one
// full-frame pass byte for byte.
void render_band(Rasterizer& rasterizer, const FrameQueue& queue,
                 const SkyGradient& sky, int row_begin, int row_end,
                 int tri_begin, int tri_end,
                 int gradient_begin, int gradient_end);

// Whole-queue convenience over render_band.
void render_rows(Rasterizer& rasterizer, const FrameQueue& queue,
                 const SkyGradient& sky, int row_begin, int row_end);

// How run_split divides a frame between the two bands. Shared by the host and
// pico implementations so every platform splits identically: a marked queue
// renders as two scenes, each with its own triangles and a gradient spanning
// just its band; an unmarked queue is one scene drawn into both bands under
// one continuous gradient.
struct SplitPlan {
    int mid, height;
    int top_tri_begin, top_tri_end;
    int bottom_tri_begin, bottom_tri_end;
    int top_grad_end, bottom_grad_begin;
};

inline SplitPlan plan_split(const Rasterizer& rasterizer,
                            const FrameQueue& queue) {
    SplitPlan plan;
    plan.height = rasterizer.target().height;
    plan.mid = plan.height / 2;
    const bool two_scenes = queue.split != FrameQueue::k_no_split;
    plan.top_tri_begin = 0;
    plan.top_tri_end = two_scenes ? queue.split : queue.count;
    plan.bottom_tri_begin = two_scenes ? queue.split : 0;
    plan.bottom_tri_end = queue.count;
    plan.top_grad_end = two_scenes ? plan.mid : plan.height;
    plan.bottom_grad_begin = two_scenes ? plan.mid : 0;
    return plan;
}

// Execute the whole collected frame.
//
// On the PicoSystem this splits the screen between the cores: core 0 takes the
// top band, core 1 the bottom. The bands are disjoint rows of the framebuffer
// and depth buffer, so there is no locking anywhere. Everywhere else (web,
// desktop, host tests) both bands run sequentially on the calling thread and
// produce the identical image.
//
// The two gradient version renders a two scene frame: the queue's split mark
// separates the top band's triangles from the bottom band's, and each band
// clears with its own gradient. With no mark, both bands share the queue.
// The single gradient version is the classic one scene split.
//
// Flash safety contract on device: core 1 idles in a loop that lives entirely
// in RAM. Anything that writes flash (write_save) must happen OUTSIDE
// run_split, which is when core 1 is guaranteed to be parked in that RAM loop
// and immune to XIP being disabled mid write. Games uphold this by saving
// between frames, never during rendering.
void run_split(Rasterizer& rasterizer, const FrameQueue& queue,
               const SkyGradient& sky_top, const SkyGradient& sky_bottom);

void run_split(Rasterizer& rasterizer, const FrameQueue& queue,
               const SkyGradient& sky);

}  // namespace pse
