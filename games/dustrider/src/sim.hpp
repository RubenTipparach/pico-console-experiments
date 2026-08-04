#pragma once

// Dust Rider's simulation. Pure integer C++, no SDK, no floats, no
// allocation, which is what lets the host test suite prove the tuning
// claims: the window never outruns a flat out bike, the road is always
// followable at top speed, no cactus ever grows on the tarmac, and a run
// is a pure function of its seed.
//
// Units: world positions are 24.8 fixed point (256 = one meter). Velocities
// are 16.16 meters per tick. One tick is 10 ms, the 32blit update cadence.
//
// Axes: the bike always rides +x. z is north, away from the camera, and it
// is the axis the road curves along. Ground is flat: there is no y in the
// sim at all, and the apparent rise and fall of the road on screen is the
// perspective of a ribbon snaking north and south.

#include <cstdint>

#include "tuning.hpp"

namespace dr {

enum class Death : uint8_t {
    None,
    Cactus,
    Rail,
    Behind,     // fell out of the window's left edge
    Ahead,      // outran the window's right edge
};

// One 2 m slice of the world. c is the road centerline's z at the chunk's
// START; the centerline interpolates linearly to the next chunk's c, so a
// bend's corners are exactly the chunk boundaries.
struct Chunk {
    int16_t c;             // fp8 z of the centerline
    uint8_t flags;
    uint8_t cactus_x;      // x within the chunk, in fp8/4 units
    uint8_t cactus_z;      // z beyond k_cactus_off_min, in fp8/4 units
};

constexpr uint8_t k_flag_rail = 1;
constexpr uint8_t k_flag_cactus = 2;

// What write_save persists. Bump the magic when the layout changes so a
// stale save is ignored instead of misread.
struct SaveData {
    uint32_t magic;        // 'D','S','T','1'
    uint32_t best_m;
    uint8_t reserved[4];
};
constexpr uint32_t k_save_magic = 0x31545344u;

// One tick's worth of input, already edge detected by the caller.
struct Input {
    bool throttle;
    bool brake;
    bool north;            // held: steer away from the camera
    bool south;            // held: steer toward the camera
};

// One tick's worth of things the presentation cares about. Reset every
// tick.
struct Events {
    bool died;
};

struct World {
    uint32_t rng;
    uint32_t tick;

    // The bike. x is fp8 plus a fractional accumulator so integration never
    // loses the low bits; v is forward speed in 16.16 m/tick; z is an
    // absolute world position, NOT a lane index, which is what makes
    // following the curve the player's job.
    int32_t x;             // fp8
    uint8_t x_frac;
    int32_t v;
    int32_t z;             // fp8
    int32_t steer_v;       // fp8 z per tick, builds while the pad is held
    bool throttling;       // last tick's throttle, for the renderer's wheelie

    // The window. screen_x is the window CENTER, fp8.
    int32_t screen_x;
    uint8_t screen_frac;
    int32_t screen_v;
    bool started;          // first throttle seen
    uint32_t start_tick;

    // Track ring buffer. Chunks [gen_next - k_track_chunks, gen_next) are
    // valid; slot = index & (k_track_chunks - 1).
    Chunk chunks[k_track_chunks];
    int32_t gen_next;      // absolute index of the next chunk to generate

    // Generator state. The road is built as plateaus joined by decisive
    // transitions rather than one continuous wobble, so a straight reads
    // as a straight and a bend reads as a bend.
    int32_t gen_c;         // centerline z at chunk gen_next's start, fp8
    int32_t gen_curve;     // current z step per chunk, fp8
    int32_t gen_curve_target;
    int32_t gen_feat_left; // chunks left in the current straight or bend
    bool gen_bending;      // the current feature is a transition
    int32_t gen_rail_left; // chunks of rail still to lay
    int32_t gen_rail_gap;  // chunks until the next rail run
    int32_t gen_rail_after;// no-cactus chunks left after a rail run ends
    int32_t gen_cactus_gap;
    bool gen_straight;     // test hook: generate bare straight road forever

    bool alive;
    Death death;
    uint32_t best_m;
    bool save_pending;

    Events ev;
};

void world_init(World& world, uint32_t seed);
void world_tick(World& world, const Input& input);

bool world_load(World& world, const SaveData& data);
void world_make_save(const World& world, SaveData& out);

// Distance ridden, whole meters.
inline int32_t distance_m(const World& world) { return world.x >> 8; }

// The road centerline's z (fp8) at an fp8 x. Outside the generated ring the
// nearest valid chunk's value is returned.
int32_t track_center_z(const World& world, int32_t x);

// How far the bike sits from the centerline, fp8. Positive is north.
inline int32_t road_offset(const World& world) {
    return world.z - track_center_z(world, world.x);
}

// True when the bike has both wheels off the tarmac.
inline bool off_road(const World& world) {
    const int32_t off = road_offset(world);
    return off > k_road_half || off < -k_road_half;
}

// Whether a guardrail runs along the north edge at x.
bool track_rail_at(const World& world, int32_t x);

// The chunk containing x. Clamped into the ring.
const Chunk& track_chunk_at(const World& world, int32_t x);

// First cactus with center in (from_x, from_x + max_ahead], or false.
// out_z is the cactus centre's absolute world z.
bool track_next_cactus(const World& world, int32_t from_x, int32_t max_ahead,
                       int32_t& out_x, int32_t& out_z);

// Test hooks: overwrite the hazard content of the chunk containing x.
// Generation stays untouched either side, so tests can stage an exact
// collision without fishing for a seed.
void world_test_place_cactus(World& world, int32_t x, int32_t z_offset);
void world_test_set_rail(World& world, int32_t x, bool rail);
void world_test_clear_hazards(World& world);

// Test hook: from now on generate featureless straight road, so physics
// claims can be measured without the road moving underneath them.
void world_test_straight(World& world, bool straight);

// Test hook: generate exactly one more chunk and return it, so the
// generator's fairness rules can be audited chunk by chunk.
Chunk world_test_generate_chunk(World& world);

}  // namespace dr
