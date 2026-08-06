#pragma once

// Every number that shapes a flight, in one place.
//
// These are tom-lander's own constants, not invented ones. The desktop game
// runs at 60 Hz and stores velocity per tick; this runs at the 32blit's 100 Hz
// and its ship is 3.2x larger in world units, so each one is converted rather
// than retuned. The ratios are what the feel lives in and they are untouched.
//
//   tom-lander                            here
//   VTOL_GRAVITY      -0.005 /60Hz tick   k_gravity
//   VTOL_THRUST        0.0035 /tick       k_pod_thrust
//   VTOL_TORQUE_*      0.002 at arm 0.9   k_pod_torque, halved, see below
//   VTOL_DAMPING       0.95 /60Hz tick    k_drag_shift
//   MOON_FUEL_DRAIN   13 per second       k_fuel_burn
//   SHIP_HARD_LANDING 0.05 /tick          k_safe_descent
//
// The number that decides how it flies is thrust to weight: one pod is 0.70
// and all four are 2.80, so a single pod cannot hold the ship up. That is true
// in the original too, and the host tests hold this file to it.
//
// Units:
//   position and velocity  fp16, 65536 = one world unit. Velocity is per
//                          tick, so position += velocity with no scaling.
//   angles                 fp8 over 4096 units to the turn, so one fp8 unit
//                          is about a thousandth of a degree and a full turn
//                          is 1,048,576, comfortably inside int32.
//   fuel                   fp8, 100 << 8 is a full tank.
// One tick is 10 ms, the 32blit update cadence.

#include <cstdint>

namespace tl {

// ---- angles ----

constexpr int32_t k_turn = 4096;               // angle units in a full circle
constexpr int32_t k_quarter = k_turn / 4;
constexpr int32_t k_trig_one = 16384;          // fp14, what sin_fp returns

// ---- the flight model ----

// Gravity, fp16 velocity added per tick. 57 u/s^2 * (0.01 s)^2 * 65536.
constexpr int32_t k_gravity = 374;

// One pod at full, same units. Thrust to weight per pod is 262/374 = 0.70.
constexpr int32_t k_pod_thrust = 262;

// Linear and angular damping: v -= v >> k_drag_shift every tick. 31/32 is
// 0.96875, which is tom-lander's 0.95 per 60 Hz tick expressed at 100 Hz
// (0.95 ^ 0.6). Terminal fall works out at k_gravity * 31, about 17.7 u/s.
constexpr int k_drag_shift = 5;
constexpr int32_t k_drag_mul = 31;             // v_terminal = a * this

// Angular acceleration from one pod at full, fp8 angle units per tick.
//
// HALVED from the original. tom-lander's arm 0.9 times VTOL_TORQUE 0.002 is
// 6.5 rad/s^2, and against its damping that settles at 118 degrees of roll a
// second off ONE pod. That is lively with WASD, an analog pad and a free
// camera. On four digital buttons at 120 pixels it is a coin flip. 53 gives
// about 57 deg/s, which is still quick and is actually flyable.
constexpr int32_t k_pod_torque = 53;

// ---- how far off level, and the two gates on it ----
//
// Measured as 1 - cos(angle) in fp14, not as an angle. There is no angle in
// the state any anywhere now, only the hull's up vector, and that vector's y
// component IS cos(tilt): comparing on it costs a subtraction, where
// recovering the angle would cost an arc cosine every tick to answer a
// question that only ever needed an ordering. Monotonic from level to upside
// down, so it sorts exactly as the angle did.
//
//   0     level
//   988   20 degrees
//   16384 90 degrees, on its side
//   32768 fully inverted
constexpr int32_t k_tumble_tilt = 16384;

// ---- landing ----

// Vertical speed a touchdown survives, fp16 per tick. tom-lander's threshold
// is 0.05 per 60 Hz tick, which is 3.0 u/s there and 9.5 u/s at this scale.
constexpr int32_t k_safe_descent = 6225;

// And how far off level, in the fp14 measure above. tom-lander has NO tilt
// gate at all: damage is speed only until the hull passes 90 degrees. A lander
// that can land on its side is a strange lander, so the demake adds one, at
// 20 degrees.
constexpr int32_t k_safe_tilt = 988;

// Horizontal speed a touchdown survives, fp16 per tick. Sliding onto a deck
// sideways is a scrape, and the original charges for it separately.
constexpr int32_t k_safe_slide = 5200;

// ---- fuel ----

constexpr int32_t k_fuel_full = 100 << 8;
// Per tick with all four pods at full: 13 per second at 100 Hz, fp8.
constexpr int32_t k_fuel_burn = 33;

// ---- auto level, the down button ----

// A PD controller on the two tilt axes, run through the pods themselves. The
// original's auto_level is rotation only and fires nothing, so holding it and
// the all-thrusters button together is two bindings; the demake has four face
// buttons and no room for that, so down does both at once.
// The P term reads the world's up vector in the hull's OWN frame, so its
// error is an fp14 sine rather than an fp8 angle. Same controller, same
// response: 415 against sin(20 degrees) reproduces what 40 gave against 20
// degrees written as an angle, to the throttle count. Rescaled rather than
// retuned, so the levelling feel did not move when the attitude did.
constexpr int32_t k_level_kp = 415;    // per fp14 of sine, >> 12
constexpr int32_t k_level_kd = 700;    // per fp8 angle unit per tick, >> 12
constexpr int32_t k_level_base = 236;  // baseline throttle, 0..255

// ---- the world ----

// Terrain is a sum of three waves. Amplitudes in whole world units, and the
// wavelength shift: angle = coordinate >> shift, so a bigger shift is a
// longer, lazier hill.
constexpr int32_t k_hill_a1 = 5, k_hill_s1 = 10;
constexpr int32_t k_hill_a2 = 4, k_hill_s2 = 10;
constexpr int32_t k_hill_a3 = 3, k_hill_s3 = 11;

// The pads. Half width of the deck, how far the flat apron reaches, and how
// far the deck stands proud of it.
//
// The height is not decoration. The depth buffer is one byte, so a deck lying
// on the apron shares a depth step with it, they tie, and ties go to whoever
// drew first: the pad flickers. Standing it up clears the tie.
constexpr int32_t k_pad_half = 7 << 16;
constexpr int32_t k_pad_flat = 16 << 16;
constexpr int32_t k_pad_rise = (2 << 16) + (26214);   // 2.4 units

// Where the hull's origin sits when the footpads are down: the model's
// nozzles reach 3 model units below centre at the scale it is drawn.
constexpr int32_t k_rest_height = (1 << 16) + 56000;  // ~1.85 units

// Ceiling. Not a wall, a reminder: past this the ship is above the scene the
// camera is framed for and the terrain has stopped being readable.
constexpr int32_t k_max_altitude = 150 << 16;

}  // namespace tl
