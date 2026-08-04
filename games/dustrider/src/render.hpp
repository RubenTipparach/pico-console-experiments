#pragma once

#include <cstdint>

#include "pse/pixel.hpp"

#include "sim.hpp"

namespace drr {

// Draw one frame of the world into the target. Pure presentation: never
// ticks the sim, and all of it runs on the host preview harness.
void render_scene(const dr::World& world, const pse::RenderTarget& target,
                  uint32_t time_ms);

}  // namespace drr
