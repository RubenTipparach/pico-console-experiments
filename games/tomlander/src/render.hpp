#pragma once

#include <cstdint>

#include "pse/pixel.hpp"

#include "sim.hpp"

namespace tlr {

// Draw one frame. Pure presentation: never ticks the sim, and all of it runs
// on the host preview harness with no SDK anywhere.
//
// `yaw` is the camera's heading in radians. It is the game's, not the sim's:
// left and right turn the view and never the hull, so the flight model has no
// opinion about it and the tests do not have to carry it.
void render_scene(const tl::World& world, const pse::RenderTarget& target,
                  float yaw, uint32_t time_ms);

// What the last frame cost. The queue has a fixed capacity and silently drops
// the overflow, which on screen is a hole in the ground rather than a crash,
// so the preview harness watches this and fails loudly instead of leaving it
// to be found on hardware.
struct FrameStats {
    uint16_t queued;
    uint16_t dropped;
    // Where the target arrow ended up, or -1 when it was not drawn because
    // the deck is in frame. Reported because the arrow's direction comes from
    // the world bearing rather than from the projection, so nothing else can
    // check the two agree.
    int16_t arrow_x, arrow_y;
    // The target deck's projected centre, and whether it landed on screen.
    int16_t pad_x, pad_y;
    bool pad_visible;
    // Which way each pod's plume walked across the screen, in 1/64ths of a
    // unit vector, or 0,0 for a pod that was not lit. Reported because the
    // flames are plotted straight into the framebuffer rather than going
    // through the triangle queue, so nothing downstream can see where they
    // pointed, and they pointed the wrong way once already: a plume is aimed
    // along the hull's own down axis, and it used to fall vertically out of a
    // tilted nozzle.
    int8_t flame_ax[tl::kPodCount], flame_ay[tl::kPodCount];
};
FrameStats last_frame_stats();

}  // namespace tlr
