#pragma once

#include <cstdint>

// Every constant the flight model is made of, and no code beyond the fixed
// point helpers. Same shape as games/stardancer/src/tuning.hpp, and for the
// same reason: a number that is tuned is a number somebody has to find.
//
// UNITS. The sim runs at a fixed 100 Hz tick and never sees wall clock, so a
// slow frame costs frames and never physics. Distances are fp16 world units
// (k_one = 65536). Velocities are fp16 world units PER TICK. Accelerations
// are fp16 per tick per tick. Angles are brads: 65536 to a full turn, which
// makes wrapping free (it is what an int32 already does) and a sine a table
// lookup.
//
// The pod is about 3.4 world units long, and every number below only means
// anything next to that.

namespace twinflare {

// ---- fixed point -----------------------------------------------------------
constexpr int k_fp = 16;
constexpr int32_t k_one = 1 << k_fp;          // 65536
constexpr int32_t k_turn = 1 << 16;           // brads in a full revolution
constexpr int32_t k_half_turn = k_turn / 2;
constexpr int k_trig_fp = 14;
constexpr int32_t k_trig_one = 1 << k_trig_fp;

constexpr int32_t k_tick_hz = 100;
constexpr uint32_t k_tick_ms = 1000 / k_tick_hz;

// A per second rate expressed per tick, and a per second squared one likewise.
// Written as constexpr functions rather than done by hand at each site,
// because a constant that has silently been divided by the tick rate once too
// often is the hardest kind of tuning bug to see.
constexpr int32_t per_s(int32_t units_fp) { return units_fp / k_tick_hz; }
constexpr int32_t per_s2(int32_t units_fp) {
    return units_fp / (k_tick_hz * k_tick_hz);
}
constexpr int32_t fp(int32_t whole, int32_t thousandths = 0) {
    return whole * k_one + thousandths * k_one / 1000;
}

// ---- the hover field -------------------------------------------------------
// The brief: a pod cannot collide with the ground because a force always keeps
// it above. That force is a SPRING, not a constraint, and the difference is
// the whole feel: a constraint glues the pod to the road, a spring lets it
// breathe over a crest and slam into a dip.
//
// It only ever pushes UP. That one line is also why the pod can fly, because
// above the rest height there is no field at all, only gravity. A ramp is not
// a special case; it is road that stopped being underneath.
constexpr int32_t k_hover_height = fp(2, 200);
constexpr int32_t k_hover_spring = 210;        // per unit of penetration, per tick^2
constexpr int32_t k_hover_damp = 155;          // tenths, on the vertical rate
constexpr int32_t k_hover_reach = fp(3, 400);  // above this clearance, no field
// And a hard floor under all of it. A spring alone lets a heavy landing push
// through the surface for a few ticks while it catches up, which is precisely
// the thing the field is supposed to make impossible: the pod sank into its
// own road. Half the rest height, so the field visibly compresses under a slam
// and then stops dead rather than passing through.
constexpr int32_t k_hover_floor = fp(1, 100);

// ---- the shape of the world beside the road --------------------------------
// The cross section, and these four numbers are the ONLY description of it.
// They were not, and that is the bug they exist to close: the sim dropped its
// shoulder over twelve units of width while the renderer drew the same
// shoulder over three and a bit, and a walled stretch was DRAWN four units
// above the height the hover field actually held the pod at. Measured over a
// lap of the desert, the pod was drawn inside the scenery at 17 of 198 sampled
// positions off the racing line, by as much as four units. Off the road, the
// game showed the player one world and drove them around a different one.
//
// sim.cpp's ground_offset() turns these into a height, and both the hover
// field and the renderer's quads read that one function.
constexpr int32_t k_shoulder_run = fp(12);    // how far out the shoulder falls
constexpr int32_t k_shoulder_drop = fp(3);    // and how far down it has got
// A canyon wall, and the height is a legibility decision twice over. The sim
// stops a pod dead at the road edge on a walled stretch; four units of drawn
// kerb did not say so, and a pod pushed back by something it could see over
// reads as the game cheating.
//
// Eleven and not the eighteen it was first drawn at. Eighteen of rock either
// side of a twenty unit canyon fills the whole frame above the road: the
// screen goes brown, there is no sky to judge the corner against, and nothing
// passes the edge of vision to say how fast you are going. Eleven still towers
// over a pod that hovers two units up, and leaves the skyline in shot.
constexpr int32_t k_wall_height = fp(11);
// Headroom in a tunnel. The pod hovers 2.2 up and is about a unit tall, so
// this is room to move and not room to fly: a tunnel takes the sky away, which
// is the whole point of putting one on a track where the answer to everything
// else is to pitch up and glide.
constexpr int32_t k_tunnel_height = fp(7);
// How deep a chasm is drawn. Below the crash floor on purpose, so what the
// player sees under them is somewhere they have already died by reaching.
constexpr int32_t k_chasm_depth = fp(34);

// THE RAILING, and the distance is measured rather than chosen. A pod that
// wandered far enough off the road ran out of drawn world: the plain is
// clamped short of the fold, and past it there was nothing to see and nothing
// to aim at. The circuit that clamps soonest is HOARFROST, whose tightest
// corner leaves 18.8 units of plain past the road edge, so a railing at
// eighteen is drawn standing on ground on every corner of every track.
//
// Eighteen units of runoff is generous next to a twelve unit shoulder: this
// bounds a lost pod, it does not narrow the racing.
constexpr int32_t k_verge = fp(18);
// And how high it stands. Over the hover height, or it is scenery rather than
// a barrier, and low enough to see the world over.
constexpr int32_t k_rail_height = fp(4);

// ---- gravity and lift ------------------------------------------------------
constexpr int32_t k_gravity = per_s2(fp(26));
// Glide, which the brief asks for by name: nose up and stay airborne longer.
// Lift is speed times angle of attack, which is the real relation and also the
// one that punishes a nose held so high the pod stops.
constexpr int32_t k_lift = 620;               // thousandths, per unit of speed*angle
constexpr int32_t k_lift_ceiling = 620;       // thousandths OF LOCAL GRAVITY
constexpr int32_t k_induced = 550;            // thousandths of speed bled per radian

// ---- thrust ----------------------------------------------------------------
// Per ENGINE, because there are two and the whole damage model hangs off that.
// One engine is not half a pod: it is half the thrust plus a permanent yaw
// torque toward the dead side.
constexpr int32_t k_thrust = per_s2(fp(21));
// Q8 brads per tick squared, per fp16 unit of thrust imbalance. At a full
// imbalance (one engine gone) this is about a fifth of a brad a tick squared,
// which over the three hundred ticks of a straight is a forty degree drift:
// survivable on the straight, and the next corner is the question.
constexpr int32_t k_asym_yaw = 350;
constexpr int32_t k_asym_drag = 220;          // thousandths of extra drag, one engine
constexpr int32_t k_drag = 4950;              // hundred-thousandths, quadratic
constexpr int32_t k_drag_brake = 7200;        // thousandths multiplier, air brake out
constexpr int32_t k_roll_drag = per_s2(fp(1, 900));
// Rough ground, in thousandths of the normal drag. Leaving the road costs
// time; it does not cost the run.
constexpr int32_t k_offroad_drag = 2600;
// Water, likewise. Well short of the rough ground: a third of TIDEBREAK's lap
// is run over the sea, so this is a stretch of the circuit rather than a
// punishment, and it has to be a cost you race against rather than one you
// avoid. Off the road AND over water takes whichever is worse.
constexpr int32_t k_water_drag = 1550;

// ---- steering --------------------------------------------------------------
// A podracer does not turn like a car. It turns like a thing dragged behind
// two engines: the heading swings first and the velocity catches up, and the
// gap between those two is the drift.
// EVERY rate here is per TICK, in thousandths. That is worth stating because
// the first pass wrote the per SECOND figures in the same units and applied
// them once a tick, so the whole of the steering, the pitch, the swing and the
// grip ran ten times too fast: the pod snapped to a commanded yaw rate inside
// two ticks and the drift the entire model is about could not happen.
constexpr int32_t k_yaw_accel = 74;           // toward the commanded rate
constexpr int32_t k_yaw_speed_fall = 460;     // of it lost at top speed
constexpr int32_t k_yaw_brake_gain = 1750;    // the hairpin tool
constexpr int32_t k_yaw_air = 780;            // of authority off the ground
constexpr int32_t k_grip = 39;                // lateral bleed
constexpr int32_t k_grip_air = 3;
constexpr int32_t k_pitch_accel = 44;
constexpr int32_t k_pitch_max = 6470;         // brads (0.62 rad)
constexpr int32_t k_pitch_damp = 32;
constexpr int32_t k_pitch_level = 26;         // while the field holds it

// Yaw rate is carried in Q8 brads per tick rather than whole brads, and that
// is not tidiness. The asymmetry torque from one weakened engine is about a
// fifth of a brad per tick squared, so in whole brads it truncated to zero and
// the entire damage-steers-you mechanic did nothing at all until an engine was
// completely gone.
constexpr int k_rate_fp = 8;
// 1.62 rad/s. A radian is 65536/(2*pi) = 10430 brads, so this is 16,897 brads
// a second. It was written as 4231, a quarter of that, and the pod understeered
// its way off every corner on the roster.
constexpr int32_t k_yaw_max = 16897 * (1 << k_rate_fp) / k_tick_hz;

// ---- the cockpit on its cables ---------------------------------------------
// The signature of the whole vehicle, and a simulation cost rather than a lean
// applied in the renderer: the engines answer the stick at once, the cockpit
// lags behind them, and then the cockpit's momentum feeds BACK into the
// engines, so a pod swung wide drags its own engines off line. A single rigid
// body with a visual tilt does not reproduce it.
constexpr int32_t k_swing_spring = 150;
constexpr int32_t k_swing_damp = 44;
constexpr int32_t k_swing_feed = 300;         // thousandths back into yaw
constexpr int32_t k_swing_max = 6300;         // brads (0.60 rad)

// ---- boost, heat, damage ---------------------------------------------------
// The boost is armed by SPEED, not by a charge. In Episode I Racer the Thrust
// Meter is a readout of current speed, and at the top of it you arm the boost
// by briefly releasing the accelerator and pressing it again. The resource you
// spend is heat. So double tapping the throttle is not a workaround for the
// pause button being in the way, it is the mechanic.
constexpr int32_t k_double_tap_ticks = 30;    // 300 ms
constexpr int32_t k_boost_ticks = 145;        // 1.45 s
constexpr int32_t k_boost_thrust = 1620;      // thousandths
constexpr int32_t k_boost_top = 1300;         // thousandths; the drag ceiling moves too
constexpr int32_t k_boost_gate = 620;         // thousandths of the pod's own top speed
constexpr int32_t k_pad_boost_ticks = 35;

constexpr int32_t k_heat_one = 1024;
constexpr int32_t k_heat_rise = 6;            // per tick of boost
constexpr int32_t k_heat_fall = 3;            // per tick otherwise
constexpr int32_t k_heat_warn = 737;          // 0.72
constexpr int32_t k_heat_lock = 430;          // 0.42, where a redline releases

constexpr int32_t k_engine_max = 1000;
constexpr int32_t k_heat_burn = 340;          // health per second per engine, over warn
constexpr int32_t k_repair = 260;             // health per second, both engines
constexpr int32_t k_repair_thrust = 340;      // thousandths of throttle while repairing
// Repair is a HANDLING penalty, not a pause button: in the original it pulls
// the pod toward the engine being worked on while it bleeds speed. Repairing
// both evenly, as the brief asks, would pull nowhere, so the pull goes toward
// whichever engine is worse. Same feel, same question: this straight or the
// next one.
constexpr int32_t k_repair_pull = 133;        // Q8 brads/tick^2 at full imbalance
constexpr int32_t k_scrape = 460;             // health per second grinding a wall
// Health per (world unit per second) of impact over the floor, in
// thousandths. Engine health runs 0..1000 here where the mockup ran 0..100,
// so every damage rate is ten times its mockup value; a rate copied across
// unscaled is an engine that never breaks.
constexpr int32_t k_slam = 3400;
constexpr int32_t k_slam_floor = per_s(fp(26));

// ---- the run ---------------------------------------------------------------
constexpr int32_t k_crash_floor = fp(-26);    // below the road, and the run ends
constexpr int32_t k_respawn_ticks = 160;
constexpr int32_t k_respawn_speed = per_s(fp(12));

// ---- racers ----------------------------------------------------------------
// A stat is 0..5 and 3 is neutral, so a middling pod flies the numbers above
// exactly as written and every other pod reads off it. `spread` is in
// thousandths per point.
constexpr int32_t stat_scale(int32_t stat, int32_t spread_thousandths) {
    return 1000 + (stat - 3) * spread_thousandths;
}
constexpr int32_t k_spread_top = 170;
constexpr int32_t k_spread_acc = 220;
constexpr int32_t k_spread_grip = 220;
constexpr int32_t k_spread_cool = 260;
constexpr int32_t k_spread_fix = 300;
constexpr int32_t k_spread_hull = 240;

// The reference top speed a stat of 3 settles at, which is the number the drag
// constant above is solved for rather than a number tuned by feel: two healthy
// engines give 42 units per second squared, so a terminal velocity of 90 needs
// (42 - roll drag) / 90^2. Ninety units a second with 8 unit track nodes is a
// node every 90 milliseconds, and that is the rate the road has to arrive at
// for this to read as a podracer.
constexpr int32_t k_top_speed_ref = per_s(fp(90));

// What the HUD shows. The sim works in world units a second, which is the only
// honest number, and 90 of them is a top speed nobody reads as fast. The
// display scales it, as every racing game has always done, and the factor is
// written down here rather than hidden in the HUD so the two cannot disagree.
constexpr int32_t k_speed_display = 5;

}  // namespace twinflare
