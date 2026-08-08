#pragma once

#include <cstdint>

#include "tuning.hpp"

namespace twinflare {

// Feature flags. These must match tools/gen_twinflare_tracks.py, which is the
// only thing that writes them.
enum : uint8_t {
    kRamp  = 1,
    kGap   = 2,
    kBoost = 4,
    kWall  = 8,
    kShort = 16,
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
};

struct Track {
    const char* name;
    const TrackNode* nodes;
    uint16_t node_count;
    uint8_t laps;
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

}  // namespace twinflare
