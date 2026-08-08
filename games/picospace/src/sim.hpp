#pragma once

// Pico Space Program's flight model. Pure integer C++: no SDK, no floats, no
// allocation, which is what lets the host tests prove the claims the game
// makes rather than leaving them as comments. That a circular orbit stays
// circular for a thousand revolutions. That a Hohmann burn computed from the
// readouts actually raises apoapsis to the moon. That staging drops the
// booster's mass and its tank together. That the touchdown the HUD colours
// green is the touchdown the sim accepts.
//
// Axes: an inertial plane centred on Picopiter, +x right, +y up. The whole
// flight is two dimensional and that is a design decision rather than a
// shortcut: an orbital plane IS a plane, an inclination change is dead weight
// on a console with four directions, and the 3D view renders the plane from
// the side so the ship is still a model in a scene.
//
// Gravity is n body against bodies on rails. Picopiter and both moons pull the
// ship every tick; the moons themselves travel on exact circles and are not
// perturbed by anything. There is no sphere of influence in the physics and
// therefore no patch to cross, no seam where a trajectory jumps, and no
// question about which body a ship between two of them is falling toward: it
// is falling toward both. `ref_body` picks a body for the READOUTS only.
//
// Units are in tuning.hpp. One tick is k_tick_ms of mission time, and time
// warp makes the tick longer rather than running more of them.

#include <cstdint>

#include "tuning.hpp"

namespace ps {

enum class Flight : uint8_t {
    Flying,
    Complete,     // the mission's goal was met: the flight is over and won
    // Down safe, somewhere the mission did not ask for. Its own state and not
    // a crash, because nothing was flown badly: a ship that comes home in one
    // piece having failed to reach orbit has landed, and calling that a wreck
    // would be the game lying about what just happened.
    Landed,
    Crashed,
    Stranded,     // out of fuel on a trajectory that never comes down
};

enum class Fault : uint8_t {
    None,
    Impact,       // hit the ground faster than the gear takes
    Toppled,      // hit it leaning further than the gear takes
    Dry,
    Lost,         // left the system
};

// How a touchdown at this descent rate goes. The sim judges by it and the HUD
// colours by it, from this one function, so the number the player is watching
// cannot promise something the landing does not honour.
enum class Touchdown : uint8_t { Clean, Hard, Fatal };

inline Touchdown touchdown_band(int32_t speed) {
    if (speed > k_safe_touch) return Touchdown::Fatal;
    if (speed > k_soft_touch) return Touchdown::Hard;
    return Touchdown::Clean;
}

// Where the nose is pointed when the autopilot has it. Prograde and retrograde
// are relative to the reference body, which is the frame the player is reading
// speeds in, so the marker on screen and the direction the ship turns to are
// the same thing.
enum class Hold : uint8_t { Off, Prograde, Retrograde, kCount };

// One tick's worth of input.
//
// The four analogue-ish ones are held states: the throttle moves while up is
// down. The three at the bottom are EDGES, and the caller is responsible for
// only setting them on the first tick of a frame. A frame can step the sim
// several times, and a stage command that fired on each of them would drop
// both stages at once.
struct Input {
    bool up, down;        // throttle
    bool left, right;     // turn the nose
    bool stage;           // edge: drop the burnt stage, light the next
    bool warp;            // edge: step the warp ladder
    bool hold;            // edge: cycle the attitude hold
};

struct World {
    uint32_t tick;
    uint32_t mission_ms;      // sim time, which warp advances faster than a clock

    // Position and velocity about Picopiter's centre. See tuning.hpp for why
    // position is an int64 and velocity is not.
    int64_t x, y;             // fp16 metres
    int32_t vx, vy;           // fp16 metres per second

    int32_t angle;            // fp16 angle units: where the nose points
    int32_t spin;             // fp16 angle units per second

    int32_t fuel_kg;          // fp8, the burning stage's tank only
    uint8_t stage;            // index into k_stages
    uint8_t throttle;         // 0..255
    uint8_t warp_step;        // index into k_warp_steps
    Hold hold;

    // Where each moon is, fp16 angle units. Index kPicopiter is unused and
    // kept anyway so a body id indexes this directly: an array that skips an
    // entry is an off by one waiting to be written.
    int32_t moon_phase[kBodyCount];

    uint8_t ref_body;         // which body the readouts report an orbit about
    uint8_t landed_on;        // a body id, or kBodyCount while airborne
    bool grounded;

    // The spent booster, which keeps flying after it is let go. One slot,
    // because there is one staging event, and it is scenery with a trajectory
    // rather than a second ship: nothing collides with it and nothing reads it
    // but the renderer.
    bool debris;
    int64_t dx, dy;
    int32_t dvx, dvy;
    int32_t debris_angle, debris_spin;

    Mission mission;
    Flight state;
    Fault fault;
    uint32_t ticks_in_state;
    uint32_t dry_ms;          // mission time since the last tank ran out
    int32_t fuel_used;        // fp8, for the debrief
    int32_t touch_speed;      // fp16, the speed the flight ended at
    uint8_t peak_stage;       // the highest stage lit, for the debrief
};

void world_init(World& world, Mission mission = Mission::Orbit);
void world_tick(World& world, const Input& input);

// ---- the readouts ----
//
// Everything below is derived. Nothing here changes the world, so the HUD, the
// map, the renderer and the tests all read the same numbers by calling the
// same functions, and there is no second copy of the orbital mechanics.

// The conic the ship is on about `ref`, as its two extremes in metres from
// that body's centre.
//
// Computed from the state vector every time it is asked for rather than being
// integrated alongside the ship, which is the only way it can be trusted: an
// element set carried in the world would drift away from the trajectory the
// moment the engine lit, and the number under the player's thumb during a burn
// is exactly the number that has to be right.
//
// `closed` is false on an escape trajectory, where apoapsis does not exist and
// the HUD says so rather than printing a number it made up.
struct Elements {
    int32_t apoapsis_m;       // from the body's CENTRE, not its surface
    int32_t periapsis_m;
    bool closed;
    uint8_t ref;

    // The conic itself, for whatever wants to DRAW the trajectory rather than
    // read its ends. r(t) = p / (1 + e cos(t - peri_angle)), which is one
    // formula for a circle, an ellipse and a hyperbola alike: the map view
    // samples it and gets the right picture on an escape trajectory without a
    // second code path, and without the renderer keeping its own copy of the
    // orbital mechanics to disagree with this one.
    int32_t semi_latus_m;     // p
    int32_t ecc_fp16;         // e, so 65536 is parabolic
    int32_t peri_angle;       // angle units, the direction periapsis lies in
};
Elements elements(const World& world);

// Which body the readouts belong to: the moon whose reference radius the ship
// is inside, or Picopiter. Recomputed every tick into World::ref_body.
uint8_t reference_body(const World& world);

// Where a body is, in metres, and how fast it is going, in fp16 m/s. Both are
// exact functions of the mission clock because the moons are on rails.
void body_position(const World& world, uint8_t body, int32_t& x, int32_t& y);
void body_velocity(const World& world, uint8_t body, int32_t& vx, int32_t& vy);

// Terrain height above the body's mean radius, in metres, at an angle measured
// the same way the ship's position is. Deterministic and cheap: three
// harmonics off the same sine table the rest of the sim uses, so the ground
// the renderer draws and the ground the ship lands on are the same ground.
int32_t terrain_m(uint8_t body, int32_t along);

// The same ground, off the plane the ship flies in.
//
// The flight is planar, so the collision only ever needs the profile above.
// The VIEW is not planar: it looks at a sphere, and a sphere whose height only
// varies one way is a corrugated roof. `across` is the angle out of the
// orbital plane, and the extra relief it adds is defined to vanish as it goes
// to zero, so terrain_at(b, a, 0) == terrain_m(b, a) exactly. That identity is
// the contract: everything off the plane is scenery, and the strip the gear
// actually touches is the sim's own ground and not a second guess at it.
int32_t terrain_at(uint8_t body, int32_t along, int32_t across);

// The ship's angular position about a body, in angle units.
int32_t bearing_from(const World& world, uint8_t body);

// Height of the ship's ORIGIN above the terrain under it, in metres. Never
// negative. This is the geometric one, and it is what the renderer scales the
// world by; on the pad it reads the height the rocket stands at rather than
// zero.
int32_t altitude_m(const World& world);

// How far the gear is off the ground, which is what the HUD shows: zero when
// the ship is sitting on something, whichever stages are attached.
int32_t clearance_m(const World& world);

// How high the ship's origin sits when it is resting: the legs, or the
// booster's engine bell and the pad under it while the first stage is still
// there. The collision, the spawn and the readouts all ask this one function.
int32_t stand_m(const World& world);

// Speed relative to the reference body, fp16 m/s.
int32_t speed_fp16(const World& world);

// Where the ship is going, as a fp14 unit vector relative to the reference
// body, and the angle units that direction stands at. Both come off the same
// velocity, so the marker on screen and the direction the autopilot flies to
// cannot disagree.
void prograde(const World& world, int32_t& ux, int32_t& uy);
int32_t prograde_angle(const World& world);

// Total mass right now, fp8 kilograms: every stage still attached plus what is
// in the burning tank.
int32_t mass_fp8(const World& world);

// Thrust actually being produced this tick, in newtons. Zero with an empty
// tank, which is what makes the fuel bar and the plume agree.
int32_t thrust_n(const World& world);

// Is the transfer window to this mission's target moon open? True when the
// moon leads the ship by the angle it takes a Hohmann transfer to arrive at,
// within k_window_tol. False for the orbit mission, which has no target, and
// while the ship is not on a closed orbit about Picopiter.
bool burn_window(const World& world);

// The mission's target, or kPicopiter when it has none.
inline uint8_t target_body(const World& world) {
    return k_mission_target[static_cast<uint8_t>(world.mission)];
}

inline bool flying(const World& world) { return world.state == Flight::Flying; }

// Is the tank of the last stage empty? The condition the stranding rule is
// built on, and the one the HUD dims the fuel bar for.
bool out_of_fuel(const World& world);

// Can warp be engaged at all right now? Off the throttle, off the ground, and
// clear of the air. Exposed so the HUD can dim the indicator rather than
// leaving a player pressing a button that quietly does nothing.
bool warp_allowed(const World& world);

inline uint8_t warp_factor(const World& world) {
    return k_warp_steps[world.warp_step];
}

// ---- the fixed point trigonometry the sim runs on ----
//
// Exposed because the renderer and the map need exactly these values, and
// because a test that cannot see them cannot check that thrust really does
// follow the nose. `angle` is in angle units, 4096 to the turn, and both
// return fp14.
int32_t sin_fp(int32_t angle);
int32_t cos_fp(int32_t angle);

// Integer square root of a non negative 64 bit value. Used for every radius
// and every speed in here, so it is worth having exactly one of.
int64_t isqrt64(int64_t value);

}  // namespace ps
