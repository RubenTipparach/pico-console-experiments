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
};
FrameStats last_frame_stats();

}  // namespace drr
