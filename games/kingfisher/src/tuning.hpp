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

// What working costs the fish, per tick: its own effort at full tilt costs
// k_drain_effort, and being cranked on costs k_drain_reel on top. A wiggle
// against the fish's direction costs it k_wiggle_drain outright. Regen is
// per tick while it is left alone.
constexpr int k_drain_effort = 5;
constexpr int k_drain_reel = 2;
constexpr int k_wiggle_drain = 14;
constexpr int k_stamina_regen = 1;

// ---- the line: forces, not phases ----
//
// Everything in the fight is a force on the line, measured in stress units.
// Two sources add up:
//
//   fish stress = strength * k_stress_per_strength * effort / 255
//   tow stress  = k_tow_stress_base + size_cm * num / den, while cranking
//
// The rule that makes the fight readable: a fish can never break the line on
// its own. The strongest fish pulling as hard as it can sits under the rod's
// limit, and the static_assert below keeps it there no matter how the dials
// move. Only the player adds enough to snap it, which means letting go is
// always safe and every break is something the player did.
//
// Effort is the fish's own doing: it wanders up and down, and the fish
// chooses a direction to put it in (away, holding, or straight at the boat).
// That is where the fight's texture comes from now, rather than from a phase
// deciding everything at once.
constexpr int k_stress_per_strength = 55;   // at full effort, per strength
constexpr int k_tow_stress_base = 40;       // cranking at all costs this
constexpr int k_tow_stress_num = 8;         // plus mass: size_cm * num / den
constexpr int k_tow_stress_den = 5;

// ---- the rod: what the line will take ----
//
// One rod for now, with room for the ones that follow. A rod is its line's
// breaking stress: raise it and bigger fish become landable, which is what a
// better rod is for.
//
//   species      str  size  fish stress  tow stress  total  can break
//   MINNOW         1    12       55           59        114     no
//   BLUEGILL       2    25      110           80        190     no
//   PERCH          3    30      165           88        253     no
//   GHOST KOI      4    60      220          136        356     no
//   BASS           5    55      275          128        403     no
//   CARP           4    70      220          152        372     no
//   GOLD CARP      5    75      275          160        435     no
//   MOONFISH       6    60      330          136        466     no
//   PIKE           7    90      385          184        569     no
//   CATFISH        8   110      440          216        656     yes
//   STURGEON       9   160      495          296        791     yes
//   THE OLD ONE   10   200      550          360        910     yes
//
//   rod: STARTER, limit 600. Strongest fish alone: 550.
//
// Fish stress is what a fish that size can manage flat out; tow stress is
// what cranking on that much mass adds. Total over the rod's limit can
// break, and only while cranking, which is why the last column changes with
// the rod rather than with the fish. The numbers come from the constants
// above, at each species' maximum size; tools cannot regenerate this table,
// so change a constant and change these too.
constexpr uint16_t k_rod_starter_max = 600;

static_assert(10 * k_stress_per_strength < k_rod_starter_max,
              "the strongest fish must not be able to break the line alone: "
              "letting go has to be safe, or the meter is a lie");

// ---- the reel: how fast line comes in ----
//
// An empty hook winds home at 4 m/s. With a fish on it the rate falls twice
// over: once for the fish's mass, which it cannot help, and again for the
// effort it is putting in, which it can. A beaten minnow comes in at about
// 1.9 m/s and a beaten legend at 0.8, and anything fighting flat out crawls
// at 0.1 whatever it weighs. A hooked fish is never as quick as no fish.
//
// Both are fp<<8 per tick, so a tenth of a metre a second survives integer
// maths (fp8 units, one tick is 10 ms: 1 m/s = 655). The empty hook's rate
// is k_retrieve_max_fp256, in the retrieve section below, because it is the
// same reel doing the same job with nothing fighting it.
constexpr int k_fight_reel_max_fp256 = 1311;  // 2 m/s, a spent minnow
constexpr int k_fight_reel_min_fp256 = 66;    // 0.1 m/s, a fish fighting

// Mass costs speed even when the fish has given up: dead weight still has to
// be dragged. This is what makes a big fish a long fight rather than just a
// dangerous one. THE OLD ONE at 200 cm gives up 800 of the 1311, so a beaten
// legend still only comes in at about 0.8 m/s.
constexpr int k_reel_mass_drag = 4;           // fp<<8 per tick, per cm

// What the fish takes when it swims away, at full effort and full strength.
// A strong fish outruns the reel; a small one cannot.
constexpr int k_fish_pull_max_fp256 = 1638;   // 2.5 m/s

// ---- effort and direction: what the fish decides ----
//
// Effort walks toward a target that depends on how rested the fish is and
// what it is currently doing. Direction is rerolled every so often, weighted
// by the phase: a running fish mostly pulls away, a resting one mostly holds
// or drifts back toward the boat.
// Pulling on a fish makes it fight: cranking raises what it is working
// toward, which is the feedback loop the whole fight turns on. Hold the reel
// down and the fish answers, the line loads, and the meter climbs. Ease off
// and it settles, which is when the line is safe and the fish is spending
// itself for nothing.
constexpr int k_effort_reel_bump = 90;        // added to the target, of 255
constexpr int k_effort_step = 9;              // per tick, toward the target
constexpr int k_effort_run_min = 150;
constexpr int k_effort_run_vary = 105;
constexpr int k_effort_tire_min = 10;
constexpr int k_effort_tire_vary = 60;
constexpr int k_dir_ticks_base = 40;
constexpr int k_dir_ticks_vary = 100;
constexpr int k_dir_away_run = 205;           // out of 255, during a run
constexpr int k_dir_away_tire = 50;           // out of 255, while resting
constexpr int k_dir_toward_tire = 40;         // of the remainder, comes back
constexpr int k_toward_div = 2;               // a fish coming back is slower

// The catch: line shorter than this lands the fish. The line snaps free at
// max = initial * (1 + 1/k_line_slack_div) + k_line_slack_fp.
constexpr int32_t k_catch_len = 340;
constexpr int k_line_slack_div = 1;
constexpr int32_t k_line_slack_fp = 3 * 256;

// ---- the meter: what the player sees ----
//
// The bar is line stress against the rod's limit, so a full bar means the
// line is at its breaking stress. It slews rather than jumping, both because
// a twitching bar is unreadable and because the climb is the warning. The
// red zone starts before the limit: it is a warning, not the break.
//
// The break itself needs the bar pinned at full for the whole danger window,
// which is the player's chance to ease off or wiggle out of it.
constexpr uint16_t k_tension_danger = 780;    // red zone, of 1023
constexpr uint16_t k_tension_full = 1023;     // the rod's limit, on the bar
constexpr uint16_t k_danger_ticks = 110;
constexpr int k_tension_slew = 26;            // per tick, toward the truth
constexpr int k_wiggle_effort_drop = 40;      // effort a wiggle costs

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
// four meters per second, so a full 50 m cast winds back in a dozen seconds.
// B recalls the lure instantly instead, from the air or the water.
//
// This is the fast case on purpose: an empty hook is the only thing the reel
// ever gets to wind in freely. The moment a fish is on it the rate drops to
// k_fight_reel_max_fp256 at best, and to a twentieth of this at worst.
constexpr int k_retrieve_ramp_ticks = 100;
constexpr int k_retrieve_max_fp256 = 2621;  // fp<<8 per tick, ~4 m/s

static_assert(k_fight_reel_max_fp256 < k_retrieve_max_fp256,
              "a fish on the line must never come in faster than a bare hook");

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
