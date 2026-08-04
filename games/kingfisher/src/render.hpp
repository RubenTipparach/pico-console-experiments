#pragma once

// The 3D scene, drawn entirely through the pse engine. No SDK types cross
// this interface, which is what lets the preview harness and any future host
// tests render real frames without the 32blit SDK. Text (the catch card, the
// records screen) is the game glue's job, because text is where the SDK
// dependency lives.

#include <cstdint>

#include "pse/pixel.hpp"
#include "sim.hpp"

namespace kfr {

void render_scene(const kf::World& world, const pse::RenderTarget& target,
                  uint32_t time_ms);

}  // namespace kfr
