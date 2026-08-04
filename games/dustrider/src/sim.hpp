#pragma once

// Dust Rider's simulation. Pure integer C++, no SDK, no floats, no
// allocation, which is what lets the host test suite prove the tuning
// claims: the window never outruns a flat out bike, the generator never
// deals an unwinnable hand, and a run is a pure function of its seed.
//
// Units: world positions are 24.8 fixed point (256 = one meter). Velocities
// are 16.16 meters per tick. One tick is 10 ms, the 32blit update cadence.
// The bike rides +x; z picks the lane, the road at z=0 and the sand
// shoulder toward the camera.

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

// One 2 m slice of the world. h is the ground height at the chunk's START;
// the ground interpolates linearly to the next chunk's h, so crests are
// exactly the chunk boundaries.
struct Chunk {
    int16_t h;             // fp8 meters
    uint8_t flags;         // bit0 rail, bit1 cactus, bit2 cactus on sand lane
    uint8_t cactus_off;    // cactus x within the chunk, in fp8/4 units
};

constexpr uint8_t k_flag_rail = 1;
constexpr uint8_t k_flag_cactus = 2;
constexpr uint8_t k_flag_cactus_sand = 4;

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
    bool to_road;          // pressed: aim for the road lane
    bool to_sand;          // pressed: aim for the sand shoulder
};

// One tick's worth of things the presentation cares about. Reset every
// tick.
struct Events {
    bool died;
    bool takeoff;
    bool landed;
};

struct World {
    uint32_t rng;
    uint32_t tick;

    // The bike. x is fp8 plus a fractional accumulator so integration
    // never loses the low bits; y is full 16.16 because heights stay
    // small. v is forward speed, vy vertical, both 16.16 m/tick.
    int32_t x;             // fp8
    uint8_t x_frac;
    int32_t y16;           // fp16
    int32_t v;
    int32_t vy;
    int32_t z;             // fp8, lane position
    int32_t lane_z;        // fp8, lane target center
    bool grounded;
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

    // Generator state.
    int32_t gen_h;         // height at chunk gen_next's start, fp8
    int32_t gen_slope;     // current slope, fp8
    int32_t gen_slope_target;
    int32_t gen_feat_left; // chunks left in the current slope feature
    int32_t gen_rail_left; // chunks of rail still to lay
    int32_t gen_rail_gap;  // chunks until the next rail run
    int32_t gen_rail_after;// no-cactus chunks left after a rail run ends
    int32_t gen_cactus_gap;

    bool gen_flat;         // test hook: generate bare flat road forever

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

// Ground height (fp8) at an fp8 x. Valid for x anywhere inside the ring;
// outside it, the nearest valid chunk's height is returned.
int32_t track_height(const World& world, int32_t x);

// Slope (fp8 rise per fp8 run) of the chunk containing x.
int32_t track_slope(const World& world, int32_t x);

// Whether a guardrail runs along the road edge at x.
bool track_rail_at(const World& world, int32_t x);

// The chunk containing x, for the renderer. Clamped into the ring.
const Chunk& track_chunk_at(const World& world, int32_t x);

// First cactus with center in (from_x, from_x + max_ahead], or false.
// out_sand says which lane it blocks.
bool track_next_cactus(const World& world, int32_t from_x, int32_t max_ahead,
                       int32_t& out_x, bool& out_sand);

// Test hooks: overwrite the hazard content of the chunk containing x.
// Generation stays untouched either side, so tests can stage an exact
// collision without fishing for a seed.
void world_test_place_cactus(World& world, int32_t x, bool sand_lane);
void world_test_set_rail(World& world, int32_t x, bool rail);
void world_test_clear_hazards(World& world);

// Test hook: from now on generate featureless flat road, so physics claims
// can be measured without terrain noise.
void world_test_flat(World& world, bool flat);

// Test hook: generate exactly one more chunk and return it, so the
// generator's fairness rules can be audited chunk by chunk.
Chunk world_test_generate_chunk(World& world);

}  // namespace dr
