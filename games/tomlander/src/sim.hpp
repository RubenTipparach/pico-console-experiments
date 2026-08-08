#pragma once

// Tom Lander's simulation. Pure integer C++, no SDK, no floats, no
// allocation, which is what lets the host tests prove the tuning claims: that
// one pod cannot hold the ship up and four can, that a pod lifts its own
// corner, that the leveller converges rather than diverges, and that a flight
// is a pure function of its inputs.
//
// Axes: +x right, +y up, +z away from the camera at rest. The hull's own up
// vector is what thrust acts along, which is the whole game: you point the
// ship by lifting one corner of it, and then you go wherever it is pointing.
//
// Units are in tuning.hpp. One tick is 10 ms.

#include <cstdint>

#include "pse/quat.hpp"

#include "tuning.hpp"

namespace tl {

// Which pod. The order matters: it is the order the buttons are bound in and
// the order the arm offsets below are written in, and the two have to agree.
enum Pod : uint8_t { kPodRight = 0, kPodLeft, kPodFront, kPodBack, kPodCount };

// Where each pod sits, in signs along x and z. The arm length is the same for
// all four, so the sign is the whole geometry.
struct PodGeometry { int8_t ox, oz; };
constexpr PodGeometry k_pods[kPodCount] = {
    {+1, 0},   // right
    {-1, 0},   // left
    {0, +1},   // front
    {0, -1},   // back
};

enum class Flight : uint8_t {
    Flying,
    Landed,      // down safe on the target deck: the mission is over
    Crashed,
};

enum class Fault : uint8_t {
    None,
    TooFast,     // came down faster than a hull survives at all
    TooSteep,
    Scraped,
    Ditched,
    Struck,      // flew into the side of a building hard enough to matter
    Broke,       // survivable landings, but one too many of them
    Dry,         // out of fuel with the mission still open
};

// How a touchdown at this descent rate goes. The sim judges by it and the HUD
// colours by it, from the one function, so the number a player is watching
// cannot promise something the landing does not honour. See k_soft_descent for
// where the two lines sit and why they sit on exact readout values.
enum class Touchdown : uint8_t {
    Clean,       // green: set down, nothing bent
    Hard,        // amber: it holds, and it costs a point of hull
    Fatal,       // red: a wreck
};

inline Touchdown descent_band(int32_t fall) {
    if (fall > k_safe_descent) return Touchdown::Fatal;
    if (fall > k_soft_descent) return Touchdown::Hard;
    return Touchdown::Clean;
}

struct Pad {
    int32_t x, z;     // fp16 centre
    int32_t y;        // fp16 apron height, filled in by world_init
    // Half width of the square that counts as a touchdown. Per pad rather
    // than one constant, because the salvage is a rocket section half the size
    // of a built deck: a shared half would have let the ship land on the water
    // beside it and score it.
    int32_t half;
};

constexpr int k_pad_count = 3;

// A fuel crate: a green cube hanging in the air that refills half a tank when
// the ship flies into it.
//
// Three states rather than one `active` flag, because a crate can be absent
// for two different reasons and the mission needs to tell them apart. The
// delivery's teaching crate is placed from the start and does not appear until
// the player has picked the cargo up, and "not out yet" has to survive being
// distinguished from "already taken" or the pickup would keep re-arming it.
enum class Crate : uint8_t {
    Waiting,     // placed, not shown, not collectable
    Out,         // on the map
    Taken,       // gone
};

struct FuelCrate {
    int32_t x, z;      // fp16 centre
    int32_t y;         // fp16, filled in by world_init from what is underneath
    Crate state;
};

// Four is the most any mission places. The delivery uses three (one on each
// leg and the teaching one beside deck B) and the salvage two.
constexpr int k_crate_max = 4;

// Which flight this is. Mission one is the hop the game opens on; mission two
// is the delivery, and it is the same world with a crate on it.
enum class Mission : uint8_t { Hop = 1, Delivery = 2, Salvage = 3 };
constexpr uint8_t k_mission_count = 3;

// Where the crate is, in mission two. A pad index while it is sitting on one,
// kCargoHeld once the ship has it, kCargoDone after it is delivered. Mission
// one leaves it kCargoNone and nothing in the flight model reads it.
constexpr uint8_t kCargoNone = 0xFD;
constexpr uint8_t kCargoHeld = 0xFE;
constexpr uint8_t kCargoDone = 0xFF;

// A building standing in the valley.
//
// This is PLACEMENT, not geometry: the meshes are block.obj and tower.obj and
// this says where they stand and how big. Rule 11 forbids writing polygons in
// C++; instancing a real model from a table is what it expects instead.
//
// It lives in the sim rather than in the renderer because a building is solid.
// ground_at reads this table, so the roof is a floor you can put down on and
// the wall is something you can hit, and the picture and the collision cannot
// disagree about where a building is because there is only one table.
// Both meshes are one unit half width and stand on y = 0, so `half` is the
// whole of the size: a building is drawn at a uniform scale of `half`, and its
// height follows from which mesh it is. Uniform on purpose. draw_mesh turns a
// baked normal by the same basis it turns a vertex by, which is exact for a
// rotation and for a uniform scale and WRONG for a squashed one, because a
// squashed normal wants the inverse transpose. Stretching a building here
// would have lit its walls by how tall it was.
struct Building {
    int16_t x, z;      // whole world units, centre
    int16_t half;      // half width, square footprint, and the draw scale
    bool tower;        // false is block.obj, true is tower.obj
    uint8_t tint;      // 0..255 multiplied over the mesh colours
};

// How tall each mesh is per unit of half width, matching the constants in
// tools/gen_tomlander_props.py. The preview harness measures the models
// against these rather than trusting the two files to stay in step.
constexpr int32_t k_block_aspect_num = 8, k_block_aspect_den = 5;   // 1.6
constexpr int32_t k_tower_aspect_num = 5, k_tower_aspect_den = 1;   // 5.0

inline int32_t building_height(const Building& b) {
    const int32_t half = b.half * 65536;
    return b.tower ? (half * k_tower_aspect_num) / k_tower_aspect_den
                   : (half * k_block_aspect_num) / k_block_aspect_den;
}

// Scattered along the two legs, and deliberately NOT within a pad's flat
// apron: a building inside the apron would be an obstacle exactly where the
// player has to be slow and low, which is where an obstacle stops being
// scenery and starts being a trap.
constexpr int k_building_count = 11;
extern const Building k_buildings[k_building_count];

// One tick's worth of input. Every field is a held state, not an edge: the
// pods burn while a button is down and stop when it is not.
struct Input {
    bool pod[kPodCount];
    bool level;        // down: level the hull and fire all four
};

struct World {
    uint32_t tick;

    int32_t x, y, z;              // fp16
    int32_t vx, vy, vz;           // fp16 per tick

    // Attitude, as a unit quaternion rather than a pair of angles.
    //
    // The pods rotate the hull about the hull's OWN axes, and Euler angles
    // cannot carry that: each angle turns about a frame the others have
    // already moved, so feeding body rates into them puts the rotation on the
    // wrong axis by an amount that grows with the other angle. Measured on
    // this exact sim before the change: firing the front pod at 30 degrees of
    // roll turned the hull about an axis 30 degrees away from the one it
    // should have, and at 90 degrees it pitched the hull where the physics
    // says it should have yawed it. A quaternion has no privileged axis, so
    // the same body rate is correct at every attitude and no attitude loses a
    // degree of freedom.
    pse::Quat q;

    // Angular velocity about the hull's own x, y and z, fp8 angle units per
    // tick. Body frame, which is the frame the pods actually push in, so this
    // is the natural place for it and no conversion happens before the torque
    // is applied.
    //
    // wy exists and is always driven to zero by the pods, because every pod
    // thrusts along the hull's +y and a force through the centre line has no
    // moment about it. It is kept because a rate is a vector and dropping a
    // component of one to save four bytes is how a sim starts lying.
    int32_t wx, wy, wz;
    int32_t fuel;                 // fp8

    uint8_t throttle[kPodCount];  // 0..255, what each pod did this tick
    Pad pads[k_pad_count];
    uint8_t target;               // index into pads
    uint8_t landed_on;            // index, or 0xFF when airborne
    bool grounded;

    Mission mission;
    uint8_t cargo;                // a pad index, or kCargo* above
    // Where the load goes once it is aboard. Named rather than inferred: the
    // delivery carries its crate ONWARD to a third deck and the salvage brings
    // it BACK to the one it launched from, so "the next pad along" is right for
    // one mission and wrong for the other.
    uint8_t deliver_to;
    // Waterline, fp16, or k_no_sea when this mission has no ocean.
    int32_t sea;

    FuelCrate crates[k_crate_max];
    uint8_t crate_count;          // how many of the array this mission placed
    // Counts up as crates are collected, and never resets inside a flight. A
    // counter rather than a one tick flag: the SDK layer edge detects it for
    // the chime the same way it edge detects the cargo, so nothing has to
    // remember to clear it and a missed frame cannot swallow the sound.
    uint8_t crates_taken;

    // Hull damage, one point per hard landing, k_damage_max ends the flight.
    // Not a health bar: nothing in the air can hurt the ship, so this only
    // ever moves at a touchdown and it only ever counts up.
    uint8_t damage;

    // Ticks since the tank emptied, zero while there is anything in it. The
    // mission is not called until this passes k_dry_grace AND the hull has
    // stopped moving, so running dry in the air is a glide rather than an
    // ending. Refilling on a leg deck resets it.
    uint16_t dry_ticks;

    Flight state;
    Fault fault;
    uint32_t ticks_in_state;
    uint32_t fuel_used;           // fp8, for the debrief
};

void world_init(World& world, Mission mission = Mission::Hop);
void world_tick(World& world, const Input& input);

// Is the ship carrying the crate right now? The one question the flight model
// asks about cargo, so it is the one the renderer and the HUD ask too.
inline bool carrying(const World& world) { return world.cargo == kCargoHeld; }

// Is there still a mission to finish? False once the flight is over, however
// it ended. Running the tank dry is only a fail while this is true, which is
// the whole of the "goals not complete" half of the rule.
inline bool mission_open(const World& world) {
    return world.state == Flight::Flying;
}

// Has the hull stopped moving? Speed rather than the grounded flag, so a hull
// something else is still pushing does not read as a hull at rest. Chebyshev,
// because this is a threshold and not a measurement and a square is one
// comparison cheaper than a circle.
inline bool at_rest(const World& world) {
    const int32_t ax = world.vx < 0 ? -world.vx : world.vx;
    const int32_t ay = world.vy < 0 ? -world.vy : world.vy;
    const int32_t az = world.vz < 0 ? -world.vz : world.vz;
    const int32_t fastest = ax > ay ? (ax > az ? ax : az) : (ay > az ? ay : az);
    return fastest <= k_at_rest_speed;
}

// ---- the fixed point trigonometry the sim runs on ----
//
// Exposed because the renderer needs the same values, and because a test that
// cannot see them cannot check that the thrust really does follow the hull.
// angle is in angle units, 4096 to the turn. Returns fp14.
int32_t sin_fp(int32_t angle);
int32_t cos_fp(int32_t angle);

// ---- the world ----

// Ground height under a point, fp16. This is the terrain plus any pad apron,
// but NOT the pad deck: see ground_at for what the hull actually rests on.
int32_t terrain_height(const World& world, int32_t x, int32_t z);

// Index of the pad whose deck covers this point, or -1.
int pad_at(const World& world, int32_t x, int32_t z);

// Is this deck floating on the sea rather than built on the ground? A floating
// deck gets NO apron: the apron exists so a pad looks built into its hill, and
// flattening the sea floor around a wreck would raise a plateau out of the
// ocean the mission is supposed to be flown across.
inline bool pad_floats(const World& world, int index) {
    return world.sea > k_no_sea &&
           world.pads[index].y <= world.sea + k_float_rise;
}

// Is this point over open water? True only where the terrain floor is below
// the waterline AND no pad or salvage deck covers it, because a deck floating
// on the sea is somewhere you land rather than somewhere you drown.
bool over_water(const World& world, int32_t x, int32_t z);

// What the hull rests on here: the deck if it is over a pad, the ground
// otherwise, plus the resting height. One function so the collision, the
// spawn and the altitude readout can never disagree about where the floor is.
int32_t ground_at(const World& world, int32_t x, int32_t z);

// Height above that floor, fp16, never negative.
inline int32_t altitude(const World& world) {
    const int32_t g = ground_at(world, world.x, world.z);
    return world.y > g ? world.y - g : 0;
}

// Descent rate, fp16 per tick. Positive is falling.
inline int32_t descent(const World& world) { return -world.vy; }

// Distance to the target pad in whole world units.
int32_t range_to_target(const World& world);

// The hull's own up vector, fp14. What thrust acts along, and the whole of
// the flight model: tilt the hull and you go sideways.
void hull_up(const World& world, int32_t& ux, int32_t& uy, int32_t& uz);

// Where the WORLD's up sits in the hull's own frame, fp14. The levelling
// error, and a better one than a pair of angles: it stays meaningful at every
// attitude, and its x and z components name the two pods that will fix it.
void up_in_hull(const World& world, int32_t& bx, int32_t& by, int32_t& bz);

// How far off level, fp14, as 1 - cos of the angle. See k_safe_tilt for why
// it is not an angle. Zero when level, 16384 at a quarter turn, 32768 upside
// down. Nothing ends a flight for being large: a hull can be inverted and fly
// out of it, and the leveller can right it.
inline int32_t tilt(const World& world) {
    int32_t ux, uy, uz;
    hull_up(world, ux, uy, uz);
    const int32_t off = pse::k_quat_one - uy;
    return off < 0 ? 0 : off;
}

}  // namespace tl
