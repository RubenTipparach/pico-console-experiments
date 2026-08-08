#pragma once

#include <cstdint>

#include "pse/pixel.hpp"

#include "sim.hpp"

namespace psr {

// Which of the two views is on screen.
//
// They are two ways of looking at one flight rather than two modes: the sim
// runs identically under both, and nothing about the ship changes when the
// player presses the button. Out of the window is where a launch and a landing
// are flown, because both are about a rocket and the ground. The map is where
// a transfer is flown, because a transfer is about two conics and neither of
// them fits in a window.
enum class View : uint8_t { Flight, Map };

// Draw one frame. Pure presentation: never ticks the sim, and all of it runs
// on the host preview harness with no SDK anywhere.
void render_scene(const ps::World& world, const pse::RenderTarget& target,
                  View view, uint32_t time_ms);

// What the last frame cost, and where it put things.
//
// The queue has a fixed capacity and silently drops the overflow, which on
// screen is a hole in the ground rather than a crash, so the preview harness
// watches this and fails loudly instead of leaving it to be found on hardware.
//
// The positions are reported because almost everything in the flight view is
// placed by a scale that changes with altitude, and a scale is exactly the
// kind of thing that can be wrong by a factor and still produce a picture.
// Nothing downstream of the renderer can see where the horizon ended up.
struct FrameStats {
    uint16_t queued;
    uint16_t dropped;

    // Screen row of the surface directly under the ship, or -1 when the
    // ground is not in frame. The whole flight view hangs off this: the ship
    // is drawn at a fixed size and the world is scaled so that this row sits
    // at a constant place, whatever the altitude.
    int16_t horizon_y;

    // The ship, and the prograde marker on its ring.
    int16_t ship_x, ship_y;
    int16_t prograde_x, prograde_y;

    // Metres per view unit the world was drawn at, times 65536. Reported
    // rather than recomputed, because the pad, the ground and the moons all
    // have to agree about it.
    int32_t world_scale_fp16;

    bool pad_drawn;
    bool legs_drawn;
    bool booster_drawn;
};
FrameStats last_frame_stats();

}  // namespace psr
