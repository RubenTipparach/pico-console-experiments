#pragma once

#include <cstdint>

#include "pse/pixel.hpp"

#include "sim.hpp"

namespace drr {

// Draw one frame of the world into the target. Pure presentation: never
// ticks the sim, and all of it runs on the host preview harness.
void render_scene(const dr::World& world, const pse::RenderTarget& target,
                  uint32_t time_ms);

// What the last frame cost. The queue has a fixed capacity and silently
// drops the overflow, which on screen is a hole in the desert rather than
// a crash, so the preview harness watches this and fails loudly instead of
// leaving it to be noticed on hardware.
struct FrameStats {
    uint16_t queued;
    uint16_t dropped;
    // The bike's horizontal screen span this frame, in pixels, from its
    // real transformed bounding box. The sim decides death from a world
    // space window and cannot ask the renderer anything, so this is how
    // the promise that a run only ends once the bike is COMPLETELY off
    // screen gets checked against the projection that actually draws it.
    int16_t bike_x0;
    int16_t bike_x1;
    // Vertical span too. The bike may leave the frame sideways, that is
    // the whole game, but it must NEVER leave it up or down: the rider
    // moves up and down the road by design, so a framing that loses them
    // off the top or bottom is a bug, not a death.
    int16_t bike_y0;
    int16_t bike_y1;
};
FrameStats last_frame_stats();

}  // namespace drr
