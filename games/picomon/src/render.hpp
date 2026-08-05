#pragma once

#include <cstdint>

#include "pse/pixel.hpp"

#include "sim.hpp"

// Drawing. Reads the sim and writes pixels, and knows nothing about any SDK,
// which is what lets the same file compile into the game, into the preview
// harness, and into the host tests.

namespace pmr {

void render_scene(const pm::World& world, const pse::RenderTarget& target,
                  uint32_t time_ms);

}  // namespace pmr
