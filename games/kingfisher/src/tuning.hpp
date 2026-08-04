#pragma once

// Every dial in the Kingfisher fight, in one place, so tuning the feel means
// editing this file and nothing else. The host tests in tests/test_sim.cpp
// re-prove the consequences after every change: patient technique lands all
// twelve species, greedy reeling loses strong fish, the minnow forgives
// everything. If a tweak here breaks one of those, the tests say so before
// a player does.
//
// Units. One tick is 10 ms, so 100 ticks = 1 second. Line lengths are fp8
// world units (256 = one unit). Tension runs 0..1023.
//
// Species stats (strength, size range, rarity, depth band) live in the
// k_species table in sim.cpp; this file holds the physics those stats feed.

#include <cstdint>

namespace kf {

// ---- stamina: how long a fish can fight ----
//
// stamina_max = base + size_cm * per_cm + strength * per_strength.
// Rough conversion: constant tugging drains about 2 per tick, so seconds of
// fight in a fish is roughly stamina_max / 200. The minnow (5 cm, strength
// 1) works out to 975, about five seconds of nonstop tugging; THE OLD ONE
// at 200 cm carries 2850, and resting lets any fish claw stamina back.
constexpr int k_stamina_base = 850;
constexpr int k_stamina_per_cm = 5;
constexpr int k_stamina_per_strength = 100;

// Drain while the player reels (per tick), and while counter wiggling (per
// wiggle). Regen is per resting tick (no reel, no wiggle).
constexpr int k_drain_reel_run = 2;
constexpr int k_drain_reel_tire = 2;
constexpr int k_wiggle_drain = 14;
constexpr int k_stamina_regen = 1;

// ---- the reel: who wins line, and how fast ----
//
// The crank has a fixed power; the fish resists with pull, which is
// strength scaled by remaining stamina (0..strength). What the player
// gains per tick is power minus resistance, so a fresh strong fish barely
// comes in and a spent one comes in at full crank. Against a running fish
// the resistance is doubled: reeling into a strong run still loses line,
// but less than resting does, so the crank acts as a brake on a long run
// at the price of tension. Easing off saves the line and pays in distance.
constexpr int k_reel_power = 14;       // unopposed crank, fp line per tick
constexpr int k_reel_run_power = 12;   // crank power during a run
constexpr int k_resist_run_mul = 2;    // run resistance = pull * this
constexpr int k_resist_tire_mul = 1;   // tire resistance = pull * this

// A running fish that is not being reeled takes pull * this per tick.
constexpr int k_run_take_mul = 1;

// The catch: line shorter than this lands the fish. The line snaps free at
// max = initial * (1 + 1/k_line_slack_div) + k_line_slack_fp.
constexpr int32_t k_catch_len = 340;
constexpr int k_line_slack_div = 1;
constexpr int32_t k_line_slack_fp = 3 * 256;

// ---- tension: the risk meter ----
//
// Builds while reeling against pull, decays when easing off. The line only
// breaks after tension has camped at or above the danger threshold for the
// full window, and the counter drains twice as fast below it, so the red
// zone is a place to escape from, not an instant loss.
constexpr uint16_t k_tension_start = 250;
constexpr uint16_t k_tension_danger = 760;
constexpr uint16_t k_danger_ticks = 110;

// Per tick deltas. Run reeling adds base + (pull * num) / den; run drifting
// adds pull / drift_div. Tire reeling adds creep while the fish still has
// half its pull, sheds otherwise; easing off in tire sheds fastest.
constexpr int k_tension_run_reel_base = 1;
constexpr int k_tension_run_reel_num = 3;
constexpr int k_tension_run_reel_den = 4;
constexpr int k_tension_run_drift_div = 4;
constexpr int k_tension_tire_creep = 1;
constexpr int k_tension_tire_reel_shed = -2;
constexpr int k_tension_tire_rest_shed = -5;
constexpr int k_wiggle_relief = 40;    // tension shed by one rod wiggle

// ---- the pond and the cast ----
//
// The lake runs k_lake_far meters out from the boat and a full power cast
// just about reaches it. Depth bands split it into shallow, mid, and deep
// water; deeper bands hold the rarer fish, so the power meter is the
// difficulty dial. Flight lasts about a second at full loft.
constexpr int32_t k_lake_far_fp = 50 * 256;         // 50 m out
constexpr int32_t k_shallow_max_fp = 15 * 256;
constexpr int32_t k_mid_max_fp = 32 * 256;
constexpr int32_t k_lake_near_fp = 3 * 256;         // fish keep off the boat
constexpr int32_t k_lake_half_width_fp = 12 * 256;
constexpr int k_cast_vy = -100;      // launch loft, fp per tick (y is down)
constexpr int k_cast_gravity = 2;    // fp per tick squared
constexpr int k_cast_vz_base = 26;   // fp per tick at zero power (~10 m)
constexpr int k_cast_vz_per255 = 102;  // added by full power (~50 m)

// ---- the retrieve: dragging the lure home ----
//
// Holding A tows the lure toward the boat, ramping over about a second to
// two meters per second, so a full 50 m cast winds back in under half a
// minute. B recalls the lure instantly instead, from the air or the water.
constexpr int k_retrieve_ramp_ticks = 100;
constexpr int k_retrieve_max_fp256 = 1311;  // fp<<8 per tick, ~2 m/s

// The reel ratchet: one low bump for every this much line wound in, during
// the tow and the fight both. Cadence tracks reel speed for free: a spent
// fish coming in at full crank clicks fast, a stalled crank is silent.
constexpr int k_reel_click_fp = 128;        // half a meter per click

// ---- phases: the fight's rhythm ----
//
// Runs last base + rnd(vary + strength * per_strength) ticks; tires last
// base + rnd(vary). A fish below stamina_max / rest_div cannot start a run.
constexpr int k_run_ticks_base = 50;
constexpr int k_run_ticks_vary = 50;
constexpr int k_run_ticks_per_strength = 8;
constexpr int k_tire_ticks_base = 70;
constexpr int k_tire_ticks_vary = 70;
constexpr int k_run_rest_div = 8;

}  // namespace kf
