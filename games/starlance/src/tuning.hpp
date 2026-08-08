#pragma once

// Every number Starlance's flight and combat model runs on, in one file.
//
// Units, and why they are what they are:
//
//   Distance  fp16 world units. One unit is roughly a fighter's beam. The
//             whole battle is deliberately small: the depth buffer is one
//             byte and a perspective depth curve spends nearly all of it near
//             the camera, so a fight spread over kilometres would put every
//             ship inside a single depth step. Everything here fits a box of
//             +/- k_arena_half, and the renderer brackets its depth range to
//             what the frame actually contains.
//
//   Time      one tick is 10 ms. The sim is driven by ticks and never by
//             wall clock, so a slow frame costs frames and never physics.
//
//   Angle     Q14 radians per tick, which is what pse::quat_integrate wants.
//             16384 is one radian.
//
// Nothing here is a float. The RP2040 has no FPU and this file feeds the per
// tick path.

#include <cstdint>

namespace sl {

// ---- fixed point ----

constexpr int32_t k_fp = 16;                 // fp16: 65536 is 1.0
constexpr int32_t k_one = 1 << k_fp;

inline constexpr int32_t units(int32_t whole) { return whole * k_one; }

// A hundredth of a unit, for writing sub unit constants readably.
inline constexpr int32_t centi(int32_t hundredths) {
    return (hundredths * k_one) / 100;
}

// ---- the arena ----

// Half width of the box the battle happens in. A ship that wanders out is
// turned back rather than clamped, so nothing ever pins to a wall.
constexpr int32_t k_arena_half = units(170);

// Beyond this the renderer does not draw a ship at all. It stays on the HUD:
// a contact you can see on the reticle ring and not out of the window is
// exactly what a long range sensor reads like, and drawing a two pixel
// smudge at 400 units would cost a mesh transform to say less.
constexpr int32_t k_draw_range = units(320);

// ---- the player ----

// ---- the throttle ----
//
// Speed is commanded, not fixed. The numbers come from the pico-8 space combat
// prototype this game is descended from, converted from its 60 frames a second
// to this sim's 100 ticks: it stepped the throttle by 0.03 of full per frame
// (about half a second lever to lever) and eased the speed toward it at 0.08
// per frame (a fifth of a second to settle).
//
// Full ahead is slightly faster than an enemy fighter, so a contact that runs
// can be caught, and the bottom of the range is a dead stop, so a turning
// fight is something you can choose to have.
constexpr int32_t k_throttle_one = 1024;          // the lever, fully forward
constexpr int32_t k_player_speed_max = centi(30); // fp16 per tick: 30 a second
constexpr int32_t k_throttle_step = 18;           // lever travel per tick
constexpr int32_t k_speed_gain = 12;              // toward commanded, over 256

// How hard the stick turns the ship, and how fast that rate builds and
// bleeds. Rates rather than angles: the ship has mass, and a craft that snaps
// to a new heading in one tick reads as a cursor.
//
// The units are Q14 RADIANS PER TICK, which is what pse::quat_integrate takes,
// and the two conversions in that sentence are both easy to skip. 16384 is a
// whole radian, and a tick is a hundredth of a second, so a rate in degrees
// per second is `deg * 16384 * 3.14159 / 180 / 100`, or very nearly `deg * 2.9`.
// Written here because the first pass of this file used numbers around 30 and
// shipped a fighter that took thirty seconds to turn round: at 10 degrees a
// second nothing can dogfight, and it reads on screen as broken AI rather than
// as a units mistake.
// int64 inside: degrees times 16384 times pi in ten thousandths overshoots an
// int32 at anything past about 40 degrees a second, and a constant expression
// that overflows is a compile error rather than a wrong number, which is the
// one mercy in it.
inline constexpr int32_t turn_rate(int32_t degrees_per_second) {
    return static_cast<int32_t>(
        (static_cast<int64_t>(degrees_per_second) * 16384 * 31416) /
        (180 * 100 * 10000));
}

constexpr int32_t k_pitch_rate = turn_rate(72);
constexpr int32_t k_yaw_rate = turn_rate(60);
constexpr int32_t k_roll_rate = turn_rate(125);

// Per tick approach toward the commanded rate, as a numerator over 256.
// About a tenth of a second to reach full deflection.
constexpr int32_t k_rate_gain = 34;

// What each enemy class can pull. A fighter out-turns you slightly and is
// slower to bring its guns to bear because it has to point its whole hull;
// a capital ship barely turns at all, which is why its turrets exist.
constexpr int32_t k_turn_fighter = turn_rate(66);
constexpr int32_t k_turn_bomber = turn_rate(34);
constexpr int32_t k_turn_gunship = turn_rate(14);
constexpr int32_t k_turn_frigate = turn_rate(7);

constexpr int32_t k_player_hull_max = 100;
constexpr int32_t k_player_shield_max = 100;

// Shields come back, hull does not. That single asymmetry is the whole reason
// to break off and let them charge rather than merging into the next furball.
constexpr uint16_t k_shield_regen_delay = 240;    // ticks after a hit
constexpr int32_t k_shield_regen = 1;             // points per 4 ticks
constexpr uint8_t k_shield_regen_period = 4;

// ---- guns ----

constexpr int32_t k_gun_speed = units(2);         // fp16 per tick: 200 a second
constexpr uint16_t k_gun_life = 55;               // ticks, so about 110 units
constexpr uint8_t k_gun_period = 9;               // ticks between shots
constexpr int16_t k_gun_damage = 6;

// The guns sit either side of the nose and their fire converges at this
// range, which is where the reticle is drawn. Without convergence a pair of
// parallel bolts straddles anything nearer than infinity, and a player aiming
// dead centre misses everything small.
constexpr int32_t k_gun_convergence = units(45);
constexpr int32_t k_gun_offset = centi(45);       // half the spacing

// ---- missiles ----

constexpr uint8_t k_missiles_max = 8;
constexpr int32_t k_missile_speed = centi(85);
constexpr uint16_t k_missile_life = 420;
constexpr uint16_t k_missile_period = 90;         // ticks between launches
constexpr int16_t k_missile_damage = 45;
// How hard a missile turns onto its target. Enough to run down a fighter that
// is not looking, not enough to catch one that breaks hard across it.
constexpr int32_t k_missile_turn = turn_rate(105);
constexpr int32_t k_missile_arm = units(4);       // it will not detonate inside this

// ---- enemy weapons ----

constexpr int32_t k_bolt_speed = centi(130);
constexpr uint16_t k_bolt_life = 130;
constexpr int16_t k_bolt_damage = 7;
// A turret's shell: slower, and it hurts.
constexpr int32_t k_shell_speed = centi(95);
constexpr uint16_t k_shell_life = 200;
constexpr int16_t k_shell_damage = 14;

// ---- hit volumes ----
//
// Spheres, one per ship class, in the table in sim.cpp. A capital ship is a
// long box and a sphere around it is generous at the waist, which is the
// forgiving direction: shots that should have hit do, and the alternative is
// an oriented box test per bolt per ship on a chip with no divider.

constexpr int32_t k_player_radius = centi(90);

// ---- targeting ----

// Only contacts inside this cone off the nose count as "in view" for the
// first pass of the target cycle. Everything else follows behind them, so Y
// always reaches what is in front of you first and still gets you round to a
// contact on your six eventually.
//
// Q14 cosine of the half angle: 11585 is 45 degrees.
constexpr int32_t k_view_cos = 11585;

// ---- the mission ----

// Ticks the frigate needs to charge its subspace jump. Kill its navigation
// before this runs out or it leaves, and leaving is a loss.
constexpr uint32_t k_jump_charge = 9000;          // 90 seconds

// Ticks between one wave dying and the next arriving, long enough to notice
// that the sky went quiet.
constexpr uint16_t k_wave_gap = 220;

// A ship with no crew stops fighting and comes apart slowly. One point of
// hull every this many ticks.
constexpr uint16_t k_derelict_period = 45;

}  // namespace sl
