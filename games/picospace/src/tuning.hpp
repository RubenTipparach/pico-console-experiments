#pragma once

// Every number Pico Space Program flies on, in one file.
//
// UNITS, because a space sim with sloppy units is a space sim that lies:
//
//   length    metres. Positions are fp16 metres in an int64, because the
//             flight spans seven decades: a rocket 20 m long, a planet 60 km
//             across, a moon 300 km out. int32 fp16 tops out at 32 km, which
//             does not even reach orbit, and int32 whole metres cannot resolve
//             a rocket sitting on its pad. int64 costs two library calls a
//             tick and settles the whole question.
//   speed     fp16 metres per second, int32. 32 km/s of headroom over an
//             orbital speed of about 800 m/s.
//   mass      fp8 kilograms, int32. One tick burns about 0.1 kg, which is 28
//             counts, so a burn integrates without grinding to a halt.
//   angle     the repo's angle units, 4096 to the turn, carried as fp16 of
//             one so a moon creeping round at half an angle unit per second
//             still moves every tick.
//   time      one tick is k_tick_ms of MISSION time. Time warp lengthens the
//             tick rather than running more of them, because 200 ticks a
//             frame is not something a 133 MHz M0+ is going to do.
//
// The bodies do not rotate. A launch site therefore stands still in inertial
// space and an orbit is a plain conic about a fixed centre, which is what
// makes the readouts and the tests as simple as they are. Nothing in the
// flight model wants a rotating frame, and adding one would buy a launch
// azimuth nobody is asking a 120 pixel screen for.

#include <cstdint>

namespace ps {

// ---- fixed point ----

constexpr int32_t k_fp16 = 65536;
constexpr int32_t k_fp8 = 256;
constexpr int32_t k_fp14 = 16384;      // what sin_fp and cos_fp return

// Angle units. 4096 to the turn is the repo's convention, and the sin table
// below is a quarter of it.
constexpr int32_t k_turn = 4096;
constexpr int32_t k_quarter = k_turn / 4;

// ---- time ----

constexpr uint32_t k_tick_ms = 10;

// The warp ladder. Every step is time the tick covers rather than extra
// ticks: at x200 one tick is two seconds of mission, and the integrator has to
// survive that. It does, because the step is semi implicit (symplectic), so a
// long step walks around the orbit slightly out of phase rather than spiralling
// out of it, which explicit Euler would do within one revolution.
//
// Two seconds at a low orbit is under a degree of arc a step. That is the real
// ceiling on this ladder and the reason it stops at 200.
constexpr uint8_t k_warp_steps[] = {1, 10, 50, 200};
constexpr uint8_t k_warp_count = 4;

// Warp needs the ship clear of the air and off the throttle. Both are what
// KSP does and for the same reason: a two second step through an atmosphere
// integrates drag once for a whole second of it, and a two second step through
// a burn misses the burn.
constexpr int32_t k_warp_floor_m = 1500;     // above this over an airless body

// ---- the bodies ----

enum BodyId : uint8_t { kPicopiter = 0, kPip, kPom, kBodyCount };

struct Body {
    const char* name;
    int32_t radius_m;
    int64_t mu;             // gravitational parameter, m^3/s^2, = g0 * R^2
    int32_t atmo_m;         // top of the air, 0 when airless
    // The radius inside which the readouts and the map switch to reporting an
    // orbit about THIS body. Gravity does not use it: every body pulls all the
    // time (see world_tick), so there is no patch to cross and no discontinuity
    // to explain. This is a frame of reference, not a physical boundary.
    int32_t ref_m;
    int32_t orbit_m;        // circular orbit about Picopiter, 0 for Picopiter
    int32_t rate_fp16;      // orbital angular rate, fp16 angle units per second
    int32_t start_fp16;     // where it sits at mission time zero
    int16_t relief_m;       // peak to trough of the terrain
    uint8_t r, g, b;        // surface colour, shared by the world and the map
};

extern const Body k_bodies[kBodyCount];

// Home. 90 km across and pulling 8 m/s^2, so a circular orbit just above the
// air runs at about 767 m/s and takes 15 minutes.
//
// The radius is set by what it costs to leave, not by what it looks like. At
// 60 km and 6 m/s^2 an orbit cost 537 m/s, which any sensible rocket built
// from these engines reaches in one stage, and the whole staging mechanic had
// nothing to do. 90 km and 8 m/s^2 puts orbit at about 1100 m/s once the
// gravity and drag losses of the climb are counted, which is more than the
// booster carries and less than the two stages carry together.
constexpr int32_t k_home_radius_m = 90000;
constexpr int32_t k_home_atmo_m = 12000;

// The air. Density is exponential with a scale height of a fifth of the
// atmosphere, quantised to 0..256, so the top of the air is not a wall.
constexpr int32_t k_scale_height_m = k_home_atmo_m / 5;

// Drag: a = rho256 * v^2 / (mass_kg * k_drag_div).
//
// One divisor rather than a drag coefficient and a frontal area, because the
// rocket is one shape and the two would only ever appear multiplied together.
// It is set so the full 2050 kg stack at sea level at 300 m/s sheds 3 m/s^2,
// which is half a g of the two it climbs at: fly the turn too low and too fast
// and the air takes the ascent apart, fly it high and the air is a rumour.
constexpr int32_t k_drag_div = 3700;

// ---- the rocket ----

struct Stage {
    int32_t dry_kg;
    int32_t fuel_kg;
    int32_t thrust_n;
    int32_t exhaust_ms;     // effective exhaust velocity, thrust / mass flow
};

constexpr int k_stage_count = 2;
extern const Stage k_stages[k_stage_count];

// 2050 kg on the pad under 33 kN is a thrust to weight of 2.01, which is the
// classic launch number: enough to climb without wasting most of the burn
// holding the rocket up. See k_stages in sim.cpp for where the rest of the
// rocket's numbers come from and what each of them is protecting.
constexpr int32_t k_launch_mass_kg = 2050;

// How hard the ship turns. Direct rate control with a short ramp: the d pad
// asks for a rate and releasing it asks for zero, which is what a player can
// actually fly with four digital directions. A real vehicle carries its
// angular momentum, and flying that on a d pad is a fight with the ship rather
// than with the orbit.
constexpr int32_t k_turn_rate = 50;             // degrees per second, peak
constexpr int32_t k_turn_ramp_ms = 250;         // to reach it, and to stop

// ---- landing ----

// How a touchdown at this speed goes, in fp16 m/s. Both edges sit on exact
// printed values so the colour on the HUD and the verdict in the sim can never
// disagree: at 5 it is green and it sets down, at 13 it is red and it does not.
constexpr int32_t k_soft_touch = 5 * k_fp16;
constexpr int32_t k_safe_touch = 12 * k_fp16;

// How far off the local vertical the ship may be at contact, as 1 - cos of the
// angle in fp14, same trick the lander uses. This is 25 degrees.
constexpr int32_t k_safe_tilt = 1673;

// How far the ship's origin stands above the terrain when it is resting on
// something. The origin is the joint between the two stages, so what is under
// it depends on which stages are still attached: the legs once the booster is
// gone, and the booster's own engine bell before that. One number for both put
// the whole first stage five metres underground on the pad.
//
// k_stack_m includes the pad deck the rocket is standing on, which is why it
// is not exactly the 11.0 m the bell hangs at: 11 to the bell, and the rest is
// concrete. See tools/gen_picospace_models.py, and tests/preview.cpp measures
// the models against both of these rather than trusting the two files to stay
// in step.
constexpr int32_t k_gear_m = 6;
constexpr int32_t k_stack_m = 12;

// ---- when a flight is over ----

// A stable orbit: both ends of the conic clear of the air by this much. The
// margin exists so a periapsis grazing the top of the atmosphere does not
// count, because that orbit decays and the card would be a lie.
constexpr int32_t k_orbit_margin_m = 1200;

// An empty tank is not the end of a flight on its own: a ship coasting to its
// landing with a dry tank has already done the work. It becomes the end when
// the trajectory will never come down, and then only after a grace period.
constexpr uint32_t k_dry_grace_ms = 20000;

// Far enough out that nothing is coming back. Beyond Pom's orbit by three
// quarters, which no mission needs and no fuel load reaches by accident.
constexpr int32_t k_lost_m = 1400000;

// ---- missions ----

// Three flights, in the order they teach each other. ORBIT is the ascent and
// the circularisation, and nothing else. PIP adds the transfer and the
// landing. POM is the same skills at twice the range with the same tank.
enum class Mission : uint8_t { Orbit = 1, Pip = 2, Pom = 3 };
constexpr uint8_t k_mission_count = 3;

// Which body a mission is trying to reach, or kPicopiter for the orbit run.
constexpr uint8_t k_mission_target[k_mission_count + 1] = {
    kPicopiter, kPicopiter, kPip, kPom,
};

// The transfer window cue. The map lights BURN when the target moon leads the
// ship by the angle a Hohmann transfer arrives at, within this tolerance.
//
// Wide enough that a player reading a 120 pixel map and pressing up can hit
// it, and no wider, because the tolerance is not symmetric in practice: the
// lead angle closes from above as the ship laps the moon, so the light comes
// on at the FAR edge every single time and a player who burns the moment it
// lights burns the whole tolerance early. At twelve degrees that put the ship
// 84 km wide of a moon whose capture radius is 70, which is to say it worked
// by luck. Seven degrees is 42 km at Pip's orbit, comfortably inside, and it
// is still four seconds of warp at x50 to react in.
constexpr int32_t k_window_tol = 7 * k_turn / 360;

}  // namespace ps
