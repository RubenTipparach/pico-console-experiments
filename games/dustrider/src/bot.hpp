#pragma once

// A minimal self riding policy over the public sim queries. The title
// screen uses it as the attract mode, and the host tests use the very same
// policy to prove that generated runs stay survivable: if this simple rider
// lives, a human with eyes has a fair game.

#include "sim.hpp"

namespace dr {

inline Input bot_input(const World& world) {
    Input in{};

    // Hold station ahead of the window center with a banked speed margin,
    // but never so much overspeed that a crest could throw the bike out of
    // the window's right edge while it is airborne and helpless.
    const int32_t rel = world.x - world.screen_x;
    const int32_t overspeed = world.v - world.screen_v;
    in.throttle = rel < 600 && (overspeed < 2600 || rel < 0);
    in.brake = rel > 800 && overspeed > 0;

    // Scan each lane's nearest cactus inside a generous horizon.
    constexpr int32_t k_horizon = 24 << 8;
    int32_t d_road = k_horizon, d_sand = k_horizon;
    int32_t scan = world.x;
    for (int guard = 0; guard < 16 && scan < world.x + k_horizon; guard++) {
        int32_t cactus_x;
        bool cactus_sand;
        if (!track_next_cactus(world, scan, world.x + k_horizon - scan,
                               cactus_x, cactus_sand)) {
            break;
        }
        const int32_t d = cactus_x - world.x;
        if (cactus_sand) {
            if (d < d_sand) d_sand = d;
        } else {
            if (d < d_road) d_road = d;
        }
        scan = cactus_x + 1;
    }

    // Switch lanes only when the current lane demands it within the
    // distance a change takes, and only into a clearly better lane; drift
    // home to the faster road whenever it is comfortably clear. A rail
    // across the path pins the current lane, which the generator keeps
    // survivable.
    const bool on_sand = world.z > k_road_edge_z;
    const int32_t commit = (world.v * 45) >> 8;   // ~45 ticks of travel
    const int32_t d_here = on_sand ? d_sand : d_road;
    const int32_t d_other = on_sand ? d_road : d_sand;

    bool want_switch = false;
    if (d_here < commit && d_other > d_here + 600) want_switch = true;
    if (!want_switch && on_sand && d_road > commit * 2) want_switch = true;

    if (want_switch) {
        const bool pinned =
            track_rail_at(world, world.x) ||
            track_rail_at(world, world.x + (d_here < commit ? d_here
                                                            : commit));
        if (!pinned) {
            if (on_sand) in.to_road = true;
            else in.to_sand = true;
        }
    }
    return in;
}

}  // namespace dr
