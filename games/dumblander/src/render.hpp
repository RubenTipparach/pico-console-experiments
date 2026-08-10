#pragma once

#include "pse/pixel.hpp"

#include "sim.hpp"

namespace dl {

// Draw one frame of `world` into `target`.
//
// All of it, including the HUD and the end of run message. Nothing here calls
// the SDK, so the preview harness renders the same screen the device does and
// a layout mistake is visible without a PicoSystem in hand. CLAUDE.md's note
// about a HUD being unverified until the game is run applies to games that
// draw theirs with screen.text; this one does not.
void render_world(const World& world, const pse::RenderTarget& target);

}  // namespace dl
