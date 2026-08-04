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
constexpr int32_t k_bike_accel = 72;

// Rolling and air drag: v -= v >> k_drag_shift every grounded tick.
constexpr int k_drag_shift = 8;

// Top speed on the road, the number every other speed hangs off:
// 18432 = 0.28 m per tick = ~28 m/s.
constexpr int32_t k_bike_vmax = k_bike_accel << k_drag_shift;

// Sand pays an extra v >> k_sand_drag_shift per tick, which puts off road
// terminal velocity at exactly half of road speed: a survivable detour
// early on, a guaranteed lost race against the late game window.
constexpr int k_sand_drag_shift = 8;

// Brake: v -= (v >> k_brake_shift) + k_brake_base while B is held.
constexpr int k_brake_shift = 6;
constexpr int32_t k_brake_base = 20;

// Airborne drag is a lighter v >> k_air_drag_shift; no rolling contact.
constexpr int k_air_drag_shift = 10;

// Vertical gravity, velocity units per tick. Real gravity would be 64
// (9.8 m/s^2 at 100 ticks a second) and it makes every crest a five second
// glide with the throttle dead; the game needs hops, not hang gliding.
// 2.5x gravity keeps jumps readable at a third of the airtime.
constexpr int32_t k_gravity = 160;

// Pull along a slope stays at the real 9.8 m/s^2 so hills read honestly
// against the bike's thrust.
constexpr int32_t k_slope_pull = 64;

// ---- the screen ----

// The window chases the rider. It can never exceed 90% of the bike's top
// speed, so a rider at full throttle on the road always gains on it; the
// host tests hold this project rule against the tuned numbers.
constexpr int32_t k_screen_vmax = (k_bike_vmax * 9) / 10;

// Launch speed of the ramp, and how much target speed each meter of
// progress adds until the cap: the window hits its 90% ceiling a bit past
// the 3 km mark, so a run's difficulty climbs for several minutes.
constexpr int32_t k_screen_v0 = 6800;
constexpr int32_t k_screen_ramp_per_m = 3;

// The screen changes speed by at most this much per tick, far below the
// bike's thrust, so the window never jumps away from a rider who can keep
// up in principle.
constexpr int32_t k_screen_accel = 6;

// Ticks after the first throttle press before the window starts to move.
constexpr int32_t k_start_grace = 100;

// Half width of the survival window in fp8 meters. The camera shows about
// 5.2 m each side of center; dying at 4.5 m keeps the wreck on screen.
constexpr int32_t k_window_half = 1152;

// ---- lanes and hazards ----

// Lane centers in fp8 world z: the road, and the sand shoulder on the far
// side of the painted edge at k_road_edge_z. The road is 3 m wide so the
// two lanes read apart on a 120 pixel screen.
constexpr int32_t k_lane_road_z = 0;
constexpr int32_t k_lane_sand_z = 700;
constexpr int32_t k_road_edge_z = 384;

// Lane change speed, fp8 z per tick (a change takes ~0.35 s).
constexpr int32_t k_lane_rate = 20;

// A guardrail is jumpable: crossing the road edge only kills below this
// height over the ground, fp8.
constexpr int32_t k_rail_top = 230;

// Cactus collision box: half width along x, height above ground, and how
// close in z the bike must be to the cactus lane center. All fp8.
constexpr int32_t k_cactus_half = 96;
constexpr int32_t k_cactus_top = 400;
constexpr int32_t k_cactus_z_reach = 160;

// ---- track generation ----

// One chunk is 512 fp8 = 2 m of track.
constexpr int k_chunk_shift = 9;
constexpr int32_t k_chunk_len = 1 << k_chunk_shift;
constexpr int k_track_chunks = 64;   // ring buffer, 128 m of world

// Flat, featureless opening stretch of every run.
constexpr int k_calm_chunks = 15;

// Slope is fp8 rise over fp8 run. The generator walks slope toward a
// target, easing by at most k_slope_ease per chunk, which rounds the
// crests just enough to read as dunes while still launching the bike.
constexpr int32_t k_slope_max = 110;         // ~0.43, ~23 degrees
constexpr int32_t k_slope_ease = 28;
constexpr int32_t k_height_limit = 5120;     // +-20 m keeps int16 chunk heights safe

// Guardrail runs and the gaps between them, in chunks.
constexpr int k_rail_gap_min = 10;
constexpr int k_rail_gap_span = 16;          // gap = min + rnd % span
constexpr int k_rail_run_min = 5;
constexpr int k_rail_run_span = 8;

// No cactus inside a rail run nor within this many chunks of its ends, so
// the rail is purely a lane lock, never a trap.
constexpr int k_rail_clear_chunks = 3;

// Cactus placement: chance per chunk in 1/256ths, ramping with distance,
// and a minimum gap so two obstacles never demand an impossible double
// lane change. 6 chunks = 12 m, twice the distance a change costs at top
// speed.
constexpr int32_t k_cactus_base_256 = 12;
constexpr int32_t k_cactus_ramp_per_m = 50;  // +1/256 every this many meters
constexpr int32_t k_cactus_max_256 = 40;
constexpr int k_cactus_min_gap = 6;

}  // namespace dr
