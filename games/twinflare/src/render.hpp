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
    // Sparks off the engine grinding a wall, counted separately from spray so
    // that one cannot stand in for the other. They are both unlit billboard
    // quads thrown off the pod and they were briefly the same counter, which
    // would have let a scrape satisfy a test asking whether water was drawn.
    uint16_t sparks;
    // And smoke off an engine that is nearly out, counted separately again for
    // the same reason: a plume and a shower are different cues about different
    // things, and one standing in for the other in a test would let a pod that
    // never smokes pass a check about smoking because it happened to be
    // grinding a wall at the time.
    uint16_t smoke;
    // Vertical faces: canyon walls, tunnel sides and roofs, and the two lips of
    // a chasm. Counted because a hole in the road with no walls around it and a
    // hole in the road with walls look the same from every angle except the one
    // the player flies at it from.
    uint16_t cliffs;
    // Segments of perimeter rim drawn: the wall that closes the world at the
    // outer edge of the plain. Counted because an unclosed world and a closed
    // one look identical from the racing line, which is where every screenshot
    // is taken from, and differ completely the moment a player looks sideways.
    uint16_t rim;
    // Top and bottom rows the pod select screen's turning pod occupies. It
    // shares a 120 pixel screen with a name, a pilot and six labelled bars, and
    // rule 9 says measure a layout rather than placing it by eye. A pod is not
    // text but it is the same problem: the one that fits at one angle of the
    // turn is the one that reaches into the stat bars at another.
    int16_t showcase_top, showcase_bottom;
    // Which nodes drew a rock last frame. The rocks used to be picked at fixed
    // offsets AHEAD OF THE POD, so every time it crossed a node boundary all
    // of them jumped to different nodes and were rebuilt from a different
    // hash: they did not move, they teleported, five times a second. A rock
    // belongs to a node, and this is how a test says so.
    uint8_t props;
    uint16_t prop_node[4];
};
const RenderStats& render_stats();

// The height of the ground the renderer DRAWS at a world point, or false where
// it draws nothing. Exists for the host tests, because the drawn ground and
// the driven ground disagreeing is invisible in a still frame and was the
// worst thing about the desert: measured over a lap, the pod was drawn inside
// the scenery at one sampled position in twelve off the racing line, by up to
// four units, and beside a jump the plain was drawn straight across a hole the
// sim would drop you through.
bool drawn_ground(const Track& t, uint16_t hint, int32_t x, int32_t z, float& y);

// One node's ground on one side, in WORLD space, as the boundary points the
// renderer builds its strips between: the road edge, the top of any rock
// standing on it, the foot of the shoulder, the railing's foot and top, and the
// outer edge of the plain.
//
// Exists so tools/gen_twinflare_viewer.py can export exactly what the game draws
// rather than a second opinion about it. Every geometry decision, the mitred
// normal, the fold clamp, ground_offset, the waterline, stays in one place.
struct GroundSlice {
    float base[3], lip[3], shoulder[3], verge[3], rail[3], plain[3];
    // The foot and the top of the perimeter rim post: the far skyline, a
    // hundred and forty units out, which is the only thing beyond the plain
    // that is anchored to the track rather than to the camera. Exported rather
    // than recomputed for the same reason as everything else here: the viewer
    // must not hold a second opinion about it.
    float rim_foot[3], rim_top[3];
    bool railed;
};
void ground_slice(const Track& t, int node, float side, GroundSlice& out);

// The greatest height above the outer plain from which the world still has no
// hole in it, in world units.
//
// Beyond the drawn plain the ground is not drawn at all: what covers the view
// out there is the camera pinned ridges, whose feet sit a fixed distance below
// the camera. So a ray leaving the pod downward hits the plain if it lands
// inside the plain's reach, and hits a ridge if it is shallower than the angle
// that ridge's foot subtends. Between those two angles is a wedge that hits
// nothing, and it OPENS AS THE POD CLIMBS: at pod height it is a sliver that
// reads as haze, and from a jump it was most of the lower half of the frame.
//
// This is where it closes, and it exists because the obvious test cannot be
// written. Counting sky pixels needs sky and ground to be different colours,
// and on three of these four circuits they are not: HOARFROST is white ice
// under white cloud and its ground sits SIX units from the sky gradient.
float sky_closes_below(const Track& t);

}  // namespace twinflare
