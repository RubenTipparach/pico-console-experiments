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
// is nearer, so it is drawn (D - R cos 30) / (D - R) times larger. Pulling the
// camera back flattens the drum into a wall of stickers; pushing it in bends
// the reel until only one face reads.
// 32 px a facet, which is a 16x16 symbol texture at exactly 2x and three
// readable rows spanning 87 of the window's 112. Five reels of 32 leave 80 px
// for the frame, which is 10 between reels and 20 at each edge.
constexpr float k_cam_dist = 57.99f;

// Horizontal, because the engine scales the vertical by the target's WIDTH
// when a viewport band is set, which is what keeps the pixels square.
constexpr float k_fov_degrees = 92.0f;

constexpr float k_drum_gap = 12.37f;

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
void render_learn(const jr::World& world, const pse::RenderTarget& screen);
void render_end(const jr::World& world, const pse::RenderTarget& screen);

// One cell of the joker sheet, which is what a joker looks like everywhere it
// appears: the HUD row, its shop card, the instructions, the end screen.
constexpr int k_joker_icon = 20;

/* Where a slot sits on the panel, and how big its box is.
 *
 * One row, five jokers and then two consumables past a divider. They are the
 * same size and the same shape because they are the same question, "what am I
 * holding", and the divider is what says the two halves are answered
 * differently: a joker fires on its own and a consumable is spent.
 *
 * Exposed for the same reason the text measurements are: a row that ran off
 * the panel, or an icon too big for its box, should fail a check rather than
 * be noticed on a device.
 */
void joker_slot(int index, int& x, int& y);
void item_slot(int index, int& x, int& y);
constexpr int k_slot_w = 26;
constexpr int k_slot_h = 26;

// Where a shop card's text starts, past the icon column. Exposed so the string
// check measures from where the words are actually drawn.
constexpr int k_shop_text_x = 34;

// What a shop card says it is. Exposed so the checks read what is drawn rather
// than their own copy of it: a card that stopped naming itself would otherwise
// still be a card, at the right price, in the right place.
const char* shop_title(const jr::ShopItem& item);
const char* shop_body(const jr::ShopItem& item);

// Where shop card `index` draws its icon. Exposed so a check can look at the
// pixels rather than trust that something was blitted there: a card that
// stopped drawing its picture still lays out, still prices, still sells.
void shop_card_icon(int index, int& x, int& y);

// Where the three visible rows land vertically, in window rows, and how tall
// each is. Projected rather than measured off a screenshot, so the payline
// drawing follows the geometry: the outer rows are further away AND turned
// away, so they are both smaller and nearer the middle than an even split of
// the window would put them.
void row_band(int row, int& centre_y, int& height);

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
