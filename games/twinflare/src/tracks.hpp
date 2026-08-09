#pragma once

#include <cstdint>

#include "tuning.hpp"

namespace twinflare {

// Feature flags. These must match tools/gen_twinflare_tracks.py, which is the
// only thing that writes them.
enum : uint8_t {
    kRamp   = 1,
    kGap    = 2,
    kBoost  = 4,
    kWall   = 8,
    kShort  = 16,
    kTunnel = 32,
};

// One node of the road's centreline. Eight bytes, in flash, never copied into
// RAM.
//
// fp4 (16 = one world unit) for position. A circuit reaches about 600 units
// from its centre, which is 9,600 in fp4 and comfortably inside an int16, and
// a sixteenth of a unit is about six centimetres of road: finer than either
// the collision or the renderer can see. fp16 here would have cost four more
// bytes a node for nothing.
struct TrackNode {
    int16_t x, z;      // fp4
    int16_t y;         // fp4
    uint8_t half_width;  // fp4
    uint8_t flags;
};

// How a place behaves, which is the reason four tracks are four games rather
// than four palettes. Every one of these is a thousandths multiplier the sim
// applies everywhere, so the moon is not a grey desert: it is somewhere a jump
// goes a long way, the air brake barely bites and the heat has nowhere to go.
struct World {
    int16_t gravity;
    int16_t grip;
    int16_t cooling;
    int16_t air;
};

struct Palette {
    uint8_t sky_top[3], sky_bottom[3];
    uint8_t ground[2][3];
    uint8_t road[2][3];
    uint8_t edge[3];
    uint8_t wall[3];
    uint8_t rock[2][3];
    // Two bands of sea and one of foam. Two bands rather than one because a
    // flat plate of a single colour reads as a floor however you shade it, and
    // the cheapest thing that reads as water is a banding that MOVES: the
    // renderer walks the phase with the clock, so the sea slides under the pod
    // for no triangles at all. Dry tracks carry a sea colour they never draw,
    // which costs nine bytes of flash apiece and means a track that grows a
    // water level later cannot ship a black one.
    uint8_t water[2][3];
    // The shallows, which is where the road went. A submerged stretch of
    // TIDEBREAK is drawn at sea level, not down where its tarmac is, and the
    // first version simply drew sea there: a third of the lap with no visible
    // racing line at all, which is not a hazard, it is the track being taken
    // away. The lane is a lighter band with foam at its edges, so the causeway
    // reads as continuing under the surface.
    uint8_t shallow[2][3];
    uint8_t foam[3];
};

// The sea level, in fp4 like a node's own y, and it is a SURFACE rather than a
// hazard volume. The hover field pushes off water exactly as it pushes off
// rock, so a pod skims the waves and cannot be dropped through them, which is
// what the whole force field premise says should happen and what a track
// described as "under and over the water" needs in order to be drivable at
// all. A dry planet says so with the sentinel and pays nothing for it.
constexpr int16_t k_no_water = INT16_MIN;

struct Track {
    const char* name;
    const TrackNode* nodes;
    uint16_t node_count;
    uint8_t laps;
    int16_t water;             // fp4, or k_no_water
    World world;
    Palette palette;
};

// World units between nodes, and it is the one number that decides what a
// track costs in flash.
constexpr int32_t k_node_spacing = fp(8);

constexpr int k_track_count = 4;
const Track& track(int index);

// fp4 out of flash, fp16 into the sim.
inline int32_t node_x(const TrackNode& n) { return static_cast<int32_t>(n.x) << 12; }
inline int32_t node_z(const TrackNode& n) { return static_cast<int32_t>(n.z) << 12; }
inline int32_t node_y(const TrackNode& n) { return static_cast<int32_t>(n.y) << 12; }
inline int32_t node_half_width(const TrackNode& n) {
    return static_cast<int32_t>(n.half_width) << 12;
}

inline bool has_water(const Track& t) { return t.water != k_no_water; }
inline int32_t water_level(const Track& t) {
    return static_cast<int32_t>(t.water) << 12;
}

}  // namespace twinflare
