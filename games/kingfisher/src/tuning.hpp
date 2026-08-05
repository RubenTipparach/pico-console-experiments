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
// Rough conversion: constant tugging drains about 7 per tick, so seconds of
// fight in a fish is roughly stamina_max / 700. The minnow (12 cm, strength
// 1) works out to 2020, about three seconds of nonstop tugging; THE OLD ONE
// at 200 cm carries 5700, about eight.
//
// This pool is deliberately twice what a fish needs to make one good stand.
// A fish that could only be worn down once would be a countdown; one that
// can come back at the line twice over is a fight, and the second wind
// below is what it comes back with.
constexpr int k_stamina_base = 1700;
constexpr int k_stamina_per_cm = 10;
constexpr int k_stamina_per_strength = 200;

// What working costs the fish, per tick: its own effort at full tilt costs
// k_drain_effort, and being cranked on costs k_drain_reel on top. A wiggle
// against the fish's direction costs it k_wiggle_drain outright. Regen is
// per tick while it is left alone.
//
// Regen is high on purpose. A fish left alone gets its wind back faster than
// idle effort burns it, so waiting is never the answer: stop working the rod
// and the fish is fresh again in seconds. That is what keeps a hand on the
// reel the whole fight instead of parked between runs.
//
// Which is why k_drain_reel has to beat k_stamina_regen on its own. If it did
// not, a fish would win back everything the player took the moment the reel
// stopped, exhaustion would never arrive, and the second wind below would be
// dead code. Working the reel is the only thing that wears a fish down: that
// is the whole economy, and the wiggle is the one relief valve on it.
constexpr int k_drain_effort = 5;
constexpr int k_drain_reel = 11;
constexpr int k_wiggle_drain = 60;
constexpr int k_stamina_regen = 7;

static_assert(k_drain_reel > k_stamina_regen,
              "cranking must cost a fish more than resting gives back, or no "
              "fish can ever be run out of stamina");

// ---- the second wind: what a spent fish does next ----
//
// Running a fish out of stamina is the payoff. Its effort collapses, the
// line goes quiet, and the reel finally bites: this is the window the whole
// fight is played for, and it is when line actually comes in.
//
// It does not last. A spent fish gets a second wind, refilling over
// k_spent_recharge_ticks whatever the player does, and the window closes as
// the bar climbs. The rate is a share of the fish's own pool rather than a
// flat number, so every fish takes the same two and a half seconds to come
// back and a legend does not get to lie limp longer than a perch.
//
// Each wind comes back smaller than the last: the cap falls by a quarter
// every time. That is what turns the cycle into progress instead of a
// treadmill. A fish is dangerous on its first stand, workable on its third,
// and beaten by its sixth, and the fight ends because the fish runs out of
// comebacks rather than because a timer said so.
constexpr int k_spent_recharge_ticks = 250;   // 2.5 s, floor to fill
constexpr int k_wind_cap_num = 3;             // each wind refills to 3/4
constexpr int k_wind_cap_den = 4;             // of the previous ceiling

// The decay stops here. A fish whose ceiling fell to nothing would flicker
// between spent and refilled every few ticks, which reads as a broken meter
// rather than a tired fish. At a quarter of its pool a fish still surges
// enough to be worth watching, but never enough to take the line back, so
// the end of a long fight is a beaten fish being walked in rather than a
// stutter.
constexpr int k_wind_cap_floor_num = 1;
constexpr int k_wind_cap_floor_den = 4;

static_assert(k_wind_cap_num < k_wind_cap_den,
              "every second wind must be weaker than the one before it, or "
              "the fish never tires and the fight cannot be won");

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
// Running a fish out of stamina has to pay, and it only pays if the reel
// actually bites while the fish is spent. A beaten fish that still crawled in
// would make the exhaustion window a formality; at these rates the window is
// where the line genuinely comes home, which is what the player is working
// the whole cycle for.
constexpr int k_fight_reel_max_fp256 = 1450;  // 2.2 m/s, a spent minnow
constexpr int k_fight_reel_min_fp256 = 66;    // 0.1 m/s, a fish fighting

// Mass costs speed even when the fish has given up: dead weight still has to
// be dragged. This is what makes a big fish a long fight rather than just a
// dangerous one. THE OLD ONE at 200 cm gives up 600 of the 1450, so a beaten
// legend still comes in at about 1.3 m/s: well short of a spent minnow, but
// no longer the crawl that made an exhaustion window a formality.
constexpr int k_reel_mass_drag = 3;           // fp<<8 per tick, per cm

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

// The rod has to load again between wiggles. Without this the wiggle is not
// a relief valve, it is an off switch: alternating left and right every few
// ticks sheds effort faster than k_effort_step can rebuild it, so the fish
// never gets to pull, the meter never climbs, and the reel can simply be
// held down from hook to net. The cooldown is what makes a wiggle a decision
// with a cost, taken when the meter is climbing and not before.
constexpr int k_wiggle_cooldown = 45;         // ticks before the next one bites

static_assert(k_wiggle_effort_drop < k_effort_step * k_wiggle_cooldown,
              "effort must out-climb a perfectly timed wiggle chain, or the "
              "fish can be held at zero effort for the whole fight");

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
