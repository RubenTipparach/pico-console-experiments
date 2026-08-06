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
// The instantaneous share is deliberately small. It used to be most of the
// bar on its own, which is what made the meter read as on or off: the
// strongest fish put 550 of the rod's 600 into the line the moment it pulled,
// so the needle was already at the stop before anything accumulated and the
// only question was whether the player was touching the reel. At 26 a legend
// pulling flat out sits under half way, which leaves the top half of the bar
// for the climb that the pull itself earns.
constexpr int k_stress_per_strength = 26;   // at full effort, per strength
constexpr int k_tow_stress_base = 40;       // cranking at all costs this

// ---- strain: what a long pull costs ----
//
// The two forces above are instantaneous, so on their own the meter only
// reports what is happening this tick: hold the reel and it steps up once and
// sits there, ease off and it drops straight back. That reads as on or off,
// and it means the length of a pull costs nothing.
//
// Strain is the memory. Pulling against a fish that still has something left
// loads the rod a little more every tick, and the loading is what eventually
// takes the line over its limit rather than the pull itself. A long haul on a
// strong fish therefore ends in a break even though no single tick of it was
// dangerous, which is the feedback loop the fight is played on.
//
// Two things are deliberate. It builds only against a fish with stamina, so
// the exhaustion window stays safe to spend and the reward for wearing a fish
// down is real. And it bleeds off faster than it builds, so easing up is
// always the answer and the player is never handed an unavoidable break.
//
// Two things build it, and both are the fish rather than the tow: how hard it
// is pulling, and how much of it there is. Mass lives here rather than in the
// instantaneous reading on purpose. A heavy fish leaning on the rod for a
// while is what breaks a line; a heavy fish for one tick is not, and putting
// its weight in the instant reading made the biggest species snap the moment
// the reel went down, with the meter jumping rather than climbing.
// Carried in 64ths of a stress unit. A minnow loads the rod at a fraction of
// a point per tick and a whole number would round that to nothing, which is
// the difference between "a minnow can just about break a line if you are
// stupid about it" and "a minnow never can".
constexpr int k_strain_fp = 64;
constexpr int k_strain_gain_effort = 30;     // per point of the fish's stress
constexpr int k_strain_gain_mass = 50;      // per cm of fish
constexpr int k_strain_relief = 3 * 64;     // per tick, easing off
constexpr int k_strain_wiggle_shed = 70 * 64;   // a wiggle dumps this much
constexpr int k_strain_max = 900 * 64;

static_assert(k_strain_relief >
                  (10 * k_stress_per_strength / 4) * k_strain_gain_effort / 64,
              "letting go has to shed strain faster than the strongest fish "
              "builds it at a quarter effort, or the meter ratchets");

// ---- the rod: what the line will take ----
//
// One rod for now, with room for the ones that follow. A rod is its line's
// breaking stress: raise it and bigger fish become landable, which is what a
// better rod is for.
//
// No fish reaches the limit on what it is doing right now. Every break is a
// pull that went on too long, which is the whole point of the strain above:
// the instant column is where the meter sits when the reel goes down, and the
// per tick column is how fast it climbs from there.
//
//   species      str  size  instant  strain/tick  to limit  snaps if held
//   MINNOW         1    12      66      0.33         never       0%
//   BLUEGILL       2    25      92      0.68         never       0%
//   PERCH          3    30     118      0.94          8.6s       0%
//   GHOST KOI      4    60     144      1.49          3.1s      12%
//   BASS           5    55     170      1.62          2.7s      51%
//   CARP           4    70     144      1.62          2.8s      85%
//   GOLD CARP      5    75     170      1.87          2.3s     100%
//   MOONFISH       6    60     196      1.87          2.2s     100%
//   PIKE           7    90     222      2.43          1.6s     100%
//   CATFISH        8   110     248      2.86          1.2s     100%
//   STURGEON       9   160     274      3.67          0.9s     100%
//   THE OLD ONE   10   200     300      4.34          0.7s     100%
//
//   rod: STARTER, limit 600. Hardest instant pull in the lake: 300.
//
// "to limit" is the strain alone at full effort, and the fish is not at full
// effort for all of it, so the measured snap times run longer: a legend takes
// about two seconds of solid cranking, a catfish three and a half.
//
// The last column is measured, not derived: a bot that holds the reel from
// hook to net, forty trials a species, six hundred for the small ones. The
// three smallest never snap because they are landed before the rod loads,
// which is what makes them the ones you can be careless with. The gradient
// through the middle is the point, and it is fragile: raising the gains to
// give a minnow a chance collapses it, because GHOST KOI jumps to 80% long
// before a minnow's fight lasts long enough to matter.
//
// The numbers come from the constants above at each species' maximum size;
// tools cannot regenerate this table, so change a constant and change these
// too.
constexpr uint16_t k_rod_starter_max = 600;

static_assert(10 * k_stress_per_strength + k_tow_stress_base <
                  k_rod_starter_max,
              "nothing may break the line on one tick's pull: a break has to "
              "be a pull held too long, or the strain above is decoration");

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

// ---- working the lure up and down the water column ----
//
// Casting distance picks the band. Depth picks between the species holding in
// it, because a fish only takes an interest in a lure near its own level: a
// bottom feeder will not come up for one hanging under the surface.
//
// The lure settles at k_lure_settle_fp while left alone, which is the sink it
// always had, and moves at k_lure_depth_step_fp while the player is holding a
// direction, so a full column takes under two seconds to sweep rather than
// five.
// The bottom of the pond, and the deepest anything is ever allowed to be.
// The underwater viewport frames exactly this column, so a hook or a fish
// below it would be a thing the player cannot see, and both are clamped here
// rather than trusted to stay in range. render.cpp sizes its camera from this
// same number so the picture and the rule cannot drift apart.
constexpr int32_t k_pond_floor_fp = (240 * 256) / 100;

constexpr int32_t k_lure_settle_fp = 2;
constexpr int32_t k_lure_depth_step_fp = 6;
constexpr int32_t k_lure_min_depth_fp = 256 / 8;   // just under the surface

// How far off a fish's level the lure may hang and still be worth rising to.
// Generous next to a band's slice, so depth is a choice to make and not a
// pixel to hit.
constexpr int32_t k_lure_depth_reach_fp = (35 * 256) / 100;

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

// ---- weight ----
//
// A fish's weight from its length, which is what a quota is counted in.
// Length cubed against a condition factor, the way a real fish's weight is
// estimated: grams = size_cm^3 / den. At 100 that is the usual factor for a
// well fed fish, and it is what makes size mean something beyond a number on
// a card. A 12 cm minnow is 17 g and a 160 cm sturgeon is 41 kg, so one good
// fish is worth an afternoon of small ones.
constexpr int k_weight_den = 100;

// ---- the tournament ----
//
// Ten days, a quota each day, and one missed quota ends the run. The quota
// climbs so the early days are a warm up and the last few need the deep
// water, which is where the rare fish are and where the line is at most risk.
//
// One day is one full day/night cycle of the pond, so the clock a player
// reads is the sun, not a number. That is three minutes a day at
// k_day_length, and a competent angler lands somewhere between four and ten
// fish in that time depending on how far out they are working.
//
// The numbers below are measured rather than guessed: tools in the test
// suite play a full run and report what a day's fishing actually yields.
constexpr uint32_t k_tour_target_base = 250;    // grams, day one
constexpr uint32_t k_tour_target_step = 200;    // added per day after that

// The score: every gram over quota, times the days survived. Overshooting on
// day one is worth ten times overshooting on day ten, which is what makes an
// early big fish worth chasing rather than banking the minimum and moving on.
// Divided down so a score reads as a number rather than a phone bill.
constexpr uint32_t k_tour_score_div = 100;

// How long the day result card holds before the next day starts.
constexpr int k_tour_card_ticks = 260;

}  // namespace kf
