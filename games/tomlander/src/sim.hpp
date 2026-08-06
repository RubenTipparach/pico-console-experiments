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
    Tumbled,
};

enum class Fault : uint8_t { None, TooFast, TooSteep, Scraped };

struct Pad {
    int32_t x, z;     // fp16 centre
    int32_t y;        // fp16 apron height, filled in by world_init
};

constexpr int k_pad_count = 2;

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

    Flight state;
    Fault fault;
    uint32_t ticks_in_state;
    uint32_t fuel_used;           // fp8, for the debrief
};

void world_init(World& world);
void world_tick(World& world, const Input& input);

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
// it is not an angle. Zero when level, k_tumble_tilt at a quarter turn.
inline int32_t tilt(const World& world) {
    int32_t ux, uy, uz;
    hull_up(world, ux, uy, uz);
    const int32_t off = pse::k_quat_one - uy;
    return off < 0 ? 0 : off;
}

}  // namespace tl
