#pragma once

// A minimal self riding policy over the public sim queries. The title
// screen uses it as the attract mode, and the host tests use the very same
// policy to prove that generated roads stay followable: if this simple
// rider can hold the tarmac at speed, a human with eyes has a fair game.

#include "sim.hpp"

namespace dr {

inline Input bot_input(const World& world) {
    Input in{};

    // Hold station ahead of the window center. The margin is the strategy:
    // the sand and a missed apex both cost speed, so slack is banked before
    // it is needed, exactly like a human rider.
    const int32_t rel = world.x - world.screen_x;
    const int32_t overspeed = world.v - world.screen_v;
    in.throttle = rel < 600 && (overspeed < 2600 || rel < 0);
    in.brake = rel > 800 && overspeed > 0;

    // Steer at the centerline a little way ahead, so the bike turns into a
    // bend as it arrives rather than after it. The lookahead grows with
    // speed because that is how far the road moves before the input lands.
    int32_t lead = world.v >> 4;
    if (lead < 256) lead = 256;
    const int32_t target = track_center_z(world, world.x + lead);
    const int32_t error = target - world.z;
    constexpr int32_t k_deadzone = 32;
    if (error > k_deadzone) in.north = true;
    if (error < -k_deadzone) in.south = true;
    return in;
}

}  // namespace dr
