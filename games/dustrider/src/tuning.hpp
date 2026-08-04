#pragma once

// Every number that shapes a run, in one place. Units follow the sim:
// positions are 24.8 fixed point (256 = one meter), velocities are 16.16
// meters per tick, one tick is 10 ms.

#include <cstdint>

namespace dr {

// ---- the bike ----

// Full throttle thrust per tick, in velocity units. With the k_drag_shift
// drag below, terminal velocity on the road works out to exactly
// k_bike_accel << k_drag_shift.
constexpr int32_t k_bike_accel = 84;

// Rolling and air drag: v -= v >> k_drag_shift every tick.
constexpr int k_drag_shift = 8;

// Top speed on the road, the number every other speed hangs off:
// 21504 = 0.33 m per tick = ~33 m/s.
constexpr int32_t k_bike_vmax = k_bike_accel << k_drag_shift;

// Sand pays an extra v >> k_sand_drag_shift per tick, which puts off road
// terminal velocity at exactly half of road speed: a survivable mistake
// early, a lost race against the late game window.
constexpr int k_sand_drag_shift = 8;

// Brake: v -= (v >> k_brake_shift) + k_brake_base while B is held.
constexpr int k_brake_shift = 6;
constexpr int32_t k_brake_base = 20;

// ---- steering ----

// Steering is a rate that BUILDS while the pad is held, not a speed the
// bike snaps to. Full lock still has to beat the worst case lateral demand
// (the tightest bend taken at top speed) with room to spare, which the
// host tests check against k_curve_max.
//
// The ramp is what makes the pad usable. At a flat 64 the bike crossed the
// whole three metre road in an eighth of a second, so the shortest tap a
// human can make threw it into the sand. Building at k_steer_accel means a
// tenth of a second of pad is about half a metre, while a held input still
// reaches full lock in a sixth of a second for following a bend.
constexpr int32_t k_steer_rate = 48;
constexpr int32_t k_steer_accel = 3;

// The road is 3 m wide: this is the distance from its centerline to either
// painted edge, fp8.
constexpr int32_t k_road_half = 384;

// How far into the sand the bike can get before the dunes stop it, fp8.
// Far enough to reach every cactus the generator plants.
constexpr int32_t k_offroad_max = 920;

// ---- the screen ----

// The window chases the rider. It can never exceed 90% of the bike's top
// speed, so a rider at full throttle on the road always gains on it; the
// host tests hold this project rule against the tuned numbers.
constexpr int32_t k_screen_vmax = (k_bike_vmax * 9) / 10;

// Launch speed of the ramp, and how much target speed each meter of
// progress adds until the cap. It opens fast rather than crawling: the
// window speed IS the speed the world goes by, because the camera rides
// the window, so a slow opening reads as a slow game no matter what the
// bike is doing.
constexpr int32_t k_screen_v0 = 11500;
constexpr int32_t k_screen_ramp_per_m = 3;

// The screen changes speed by at most this much per tick, far below the
// bike's thrust, so the window never jumps away from a rider who can keep
// up in principle.
constexpr int32_t k_screen_accel = 6;

// Ticks after the first throttle press before the window starts to move.
constexpr int32_t k_start_grace = 100;

// Half width of the survival window in fp8 meters.
//
// A run ends only once the bike is COMPLETELY off screen: while a single
// pixel of it still shows, the rider is still alive. That makes this
// number a consequence of the camera, not a taste decision.
//
// The camera is locked to the road rather than to the bike, so the bike's
// distance from the lens changes as the rider strays off the centerline,
// and with it how far along x the bike can travel before clearing the
// frame. This is derived against the WORST case, the bike as far from the
// camera as it is allowed to get, because that is when it draws smallest
// and leaves latest. Every nearer position clears sooner, so the promise
// holds for all of them.
//
// The sim cannot ask the renderer anything, so this is verified the other
// way round: the preview harness parks the bike at exactly this offset, at
// that worst case depth, reads the span the real projection gives it, and
// fails if any of it is still on screen, or if a nudge back inside the
// window is not.
constexpr int32_t k_window_half = 1900;

// ---- track generation ----

// One chunk is 512 fp8 = 2 m of track.
constexpr int k_chunk_shift = 9;
constexpr int32_t k_chunk_len = 1 << k_chunk_shift;
constexpr int k_track_chunks = 64;   // ring buffer, 128 m of world

// Straight, featureless opening stretch of every run.
constexpr int k_calm_chunks = 15;

// How far the centerline may swing north or south per chunk, fp8. The
// generator eases curvature toward a target by at most k_curve_ease per
// chunk, which rounds the entry and exit of every bend.
constexpr int32_t k_curve_max = 120;
constexpr int32_t k_curve_ease = 28;

// The road wanders inside a 20 m band, so a long run does not drift the
// world off into large coordinates.
constexpr int32_t k_center_limit = 5120;

// Guardrail runs and the gaps between them, in chunks.
constexpr int k_rail_gap_min = 10;
constexpr int k_rail_gap_span = 16;          // gap = min + rnd % span
constexpr int k_rail_run_min = 5;
constexpr int k_rail_run_span = 8;

// No cactus inside a rail run nor within this many chunks of its ends: a
// cactus behind a rail is scenery the bike can never reach, and the game
// promises that everything off the road can kill you.
constexpr int k_rail_clear_chunks = 3;

// Cactus placement: chance per chunk in 1/256ths, ramping with distance,
// and a minimum gap in chunks so the shoulder never becomes a wall.
constexpr int32_t k_cactus_base_256 = 14;
constexpr int32_t k_cactus_ramp_per_m = 50;  // +1/256 every this many meters
constexpr int32_t k_cactus_max_256 = 46;
constexpr int k_cactus_min_gap = 4;

// Cactus collision box: half width along x, and how close in z the bike
// must come. Both fp8.
constexpr int32_t k_cactus_half = 96;
constexpr int32_t k_cactus_z_reach = 150;

// Cacti grow on the north shoulder only, between these distances from the
// centerline, fp8. The minimum is set so that a cactus reach can never
// overlap the road: k_cactus_off_min - k_cactus_z_reach > k_road_half. A
// bike with two wheels on the tarmac is safe by construction, which is the
// whole point of keeping them off the road.
constexpr int32_t k_cactus_off_min = 570;
constexpr int32_t k_cactus_off_span = 292;

// North is the far side of the road from the camera, so a rail and the
// cacti behind it never occlude the bike.
static_assert(k_cactus_off_min - k_cactus_z_reach > k_road_half,
              "a cactus must never reach onto the road");
static_assert(k_cactus_off_min + k_cactus_off_span < k_offroad_max,
              "every cactus must be reachable, or it is not a hazard");

}  // namespace dr
