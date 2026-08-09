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

// What the last frame actually did, for the tests and for budgeting.
//
// `max_coordinate` is the largest camera relative coordinate handed to the
// projector, and it is the one number that says whether the floating origin is
// working. Renderer3D projects in 1024 scale fixed point and its error grows
// with the magnitude going in, so a game that feeds it absolute world
// coordinates shimmers at the far end of a 2,400 unit lap and is rock steady
// at the start line. Feeding it camera relative coordinates bounds this by the
// far plane wherever on the track the race is, and a test can say so.
struct RenderStats {
    float max_coordinate;
    uint16_t clipped;      // polygons cut by the near plane rather than dropped
    uint16_t dropped_far;  // polygons wholly beyond the far plane
    uint16_t triangles;
    // How far the worst cable anchor sits outside the hull it attaches to.
    // Zero is welded on. The cables used to be strung between two points that
    // were on neither the engines nor the cockpit, so they met in the middle
    // and connected to nothing.
    float cable_gap;
    // Sea and spray drawn last frame. Counted rather than eyeballed because
    // both are invisible in exactly the case that matters: a shoreline that
    // never gets drawn and a track with no water look identical in a still
    // frame of open sea, and a spray count of zero over water is the whole
    // feature silently absent.
    uint16_t sea;
    uint16_t spray;
};
const RenderStats& render_stats();

}  // namespace twinflare
