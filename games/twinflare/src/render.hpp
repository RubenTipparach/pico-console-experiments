#pragma once

#include <cstdint>

#include "pse/pixel.hpp"
#include "sim.hpp"

// Everything the player sees, including the HUD and every menu.
//
// That last part is deliberate and it is copied from Star Dancer rather than
// from the other games here. The usual arrangement draws the HUD in game.cpp
// with the SDK's screen.text, which cannot be compiled on a host at all, so
// CLAUDE.md has to warn that any HUD change is unverified until somebody runs
// the game on a desktop. Drawing through a RenderTarget instead means the
// preview harness renders every screen in this file, and the thumbnail comes
// out of the same code the device runs.

namespace twinflare {

enum class Screen : uint8_t {
    Title,
    PodSelect,
    TrackSelect,
    Race,
    Paused,
    Results,
};

struct Chrome {
    Screen screen = Screen::Title;
    uint8_t pod = 0;
    uint8_t track = 0;
    uint8_t menu_item = 0;
    uint32_t time_ms = 0;
    bool boost_on_a = false;   // the button scheme, see game.cpp
};

void render_frame(const Race& race, const Chrome& chrome,
                  const pse::RenderTarget& target);

}  // namespace twinflare
