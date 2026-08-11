#pragma once

// Drawing Joker Reels. Engine only: no SDK anywhere in here, which is what
// lets the host preview harness render real frames without a device.
//
// That includes the text. The obvious place for a HUD is game.cpp with
// screen.text, which is what every other game here does and what this one did
// first, and it is the wrong place: game.cpp is the one file no host build
// compiles, so every number and label was invisible to the preview and
// unverified until somebody ran it on hardware. pse::draw_text exists for
// exactly this and its header says so. game.cpp is input and one call.

#include "pse/pixel.hpp"

#include "sim.hpp"

namespace jrr {

// ---------------------------------------------------------------------------
// The 3D window
// ---------------------------------------------------------------------------
//
// The machine is drawn into the TOP BAND of a 240x240 hires screen and the
// panel underneath is ordinary 2D. That split is the whole reason this game is
// affordable.
//
// pse::Rasterizer indexes its depth buffer as y * target.width + x, and
// RenderTarget carries row_stride separately from width precisely so a
// target's rows need not be packed. So a target pointing at row 0 of the
// screen, with this height and the screen's own stride, gives a 3D window in
// the top band and a depth buffer covering only that band: 240 x 112 is 26,880
// bytes rather than the 57,600 a full hires depth buffer would cost. The
// game's DEFINES set PSE_RENDER_WIDTH and PSE_RENDER_HEIGHT to match, and
// nothing in the engine had to change for it.
constexpr int k_window_h = 112;
constexpr int k_screen_w = 240;
constexpr int k_screen_h = 240;

// ---------------------------------------------------------------------------
// The drum, and the one rule its proportions have to obey
// ---------------------------------------------------------------------------
//
// A facet of a twelve sided drum is 2*pi*R/12 tall, so the drum has to be that
// wide too or a square symbol is drawn into a rectangle and comes out
// squashed. The width is DERIVED from the radius rather than chosen beside it,
// so there is no second number anybody can nudge to break it.
//
// That makes the drum about four times taller than it is wide, which is what a
// slot reel is, and it is why the machine needs a window: the drum does not
// fit the band, and it is not supposed to. You see three of its twelve faces.
constexpr float k_pi = 3.14159265f;
constexpr float k_drum_radius = 24.0f;
constexpr float k_facet_size = 2.0f * k_pi * k_drum_radius / jr::k_facets;

// How far the camera stands off the drum axis.
//
// Not a taste decision: it is the one number that sets how big a facet lands
// on screen and how hard the perspective bends it. At distance D the front
// facet is focal * facet_size / (D - R) pixels, and the facet 30 degrees round
// is nearer, so it is drawn (D - R cos 30) / (D - R) times larger. D = 2.07 R
// gives a 56 pixel front facet and about 27 percent of that curve, which is
// what a reel looks like. Pulling the camera back flattens the drum into a
// wall of stickers.
constexpr float k_cam_dist = 49.8f;

// Horizontal, because the engine scales the vertical by the target's WIDTH
// when a viewport band is set, which is what keeps the pixels square.
constexpr float k_fov_degrees = 92.0f;

constexpr float k_drum_gap = 14.8f;

// The reel window, in screen pixels. Cut from where the drums actually
// project rather than typed against a screenshot: see render.cpp.
constexpr int k_bezel_top = 8;
constexpr int k_bezel_bottom = k_window_h - 12;

// The whole frame, whichever screen is showing. This is all game.cpp calls.
void render_frame(const jr::World& world, const pse::RenderTarget& screen);

// The parts, for the preview harness and for anything that wants one of them.
// `screen` is always the whole 240x240 surface.
void render_machine(const jr::World& world, const pse::RenderTarget& screen);
void render_panel(const jr::World& world, const pse::RenderTarget& screen);
void render_shop(const jr::World& world, const pse::RenderTarget& screen);
void render_end(const jr::World& world, const pse::RenderTarget& screen);

// What a joker's name is shortened to for its HUD slot. Exposed so the string
// check measures what is actually drawn rather than its own copy of the rule.
const char* joker_slot_name(uint8_t joker);

// Where drum `d`'s front face lands horizontally, so the HUD and the tests can
// ask rather than assume.
void drum_window(int drum, int& left, int& right);

// Worst frame measurements, for the preview harness and a debug overlay.
struct Stats {
    uint32_t triangles;
    uint32_t queued;
    uint32_t dropped;
};
const Stats& stats();

}  // namespace jrr
