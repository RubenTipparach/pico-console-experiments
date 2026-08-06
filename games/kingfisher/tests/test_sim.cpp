// Kingfisher sim tests. These are the proof behind the tuning claims: every
// species is landable with patient reeling, strong fish snap the line when
// muscled, the state machine cannot wedge, and the whole sim allocates
// nothing, ever.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>

#include "sim.hpp"

namespace {

int g_failures = 0;
int g_checks = 0;
bool g_alloc_guard = false;

void check(bool condition, const char* what) {
    g_checks++;
    if (condition) return;
    g_failures++;
    std::printf("FAIL %s\n", what);
}

#define CHECK(expr) check((expr), #expr)

uint32_t g_rng = 0xBADC0DE1u;
uint32_t next_random() {
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 17;
    g_rng ^= g_rng << 5;
    return g_rng;
}

void check_invariants(const kf::World& world) {
    if (world.tension >= 1024) { check(false, "tension in range"); }
    if (world.day_tick >= kf::k_day_length) { check(false, "day_tick wraps"); }
    if (world.hooked_fish < -1 || world.hooked_fish >= kf::k_max_fish) {
        check(false, "hooked_fish index valid");
    }
    if (world.mode == kf::Mode::Fight) {
        if (world.hooked_fish < 0 ||
            world.fish[world.hooked_fish].state != kf::FishState::Hooked) {
            check(false, "fight always has a hooked fish");
        }
    }
    for (const auto& fish : world.fish) {
        if (static_cast<uint8_t>(fish.state) > 6) {
            check(false, "fish state valid");
        }
        if (fish.species >= kf::k_species_count) {
            check(false, "fish species valid");
        }
    }
    if (world.lure_x < -kf::k_lake_half_width_fp - 2 * kf::k_one ||
        world.lure_x > kf::k_lake_half_width_fp + 2 * kf::k_one ||
        world.lure_z < -2 * kf::k_one ||
        world.lure_z > kf::k_lake_far_fp + 2 * kf::k_one) {
        check(false, "lure inside the lake");
    }
}

// 20 minutes of random button mashing must not break a single invariant.
void test_monkey() {
    kf::World world;
    kf::world_init(world, 1234);

    g_alloc_guard = true;
    int active_late = 0;
    for (int i = 0; i < 120000; i++) {
        kf::Input input{};
        const uint32_t r = next_random();
        input.a = (r & 3) == 0;
        input.a_pressed = (r & 15) == 4;
        input.a_released = (r & 15) == 8;
        input.left = (r & 48) == 16;
        input.right = (r & 48) == 32;
        input.left_pressed = (r & 192) == 64;
        input.right_pressed = (r & 192) == 128;
        input.b_pressed = (r & 1536) == 512;
        kf::world_tick(world, input);
        check_invariants(world);
        if (i > 100000) {
            for (const auto& fish : world.fish) {
                if (fish.state != kf::FishState::Gone) active_late++;
            }
        }
        if (g_failures > 10) break;   // one report per class is enough
    }
    g_alloc_guard = false;
    // Two full day cycles in, fish must still exist: slot leaks drain the
    // pond permanently and this is what catches the next one.
    CHECK(active_late > 0);
}

// Identical seeds and inputs must replay identically. Rendering differing per
// platform is fine; the pond differing is not.
void test_determinism() {
    kf::World a, b;
    kf::world_init(a, 777);
    kf::world_init(b, 777);
    for (int i = 0; i < 20000; i++) {
        kf::Input input{};
        input.a = (i % 7) == 0;
        input.a_pressed = (i % 700) == 3;
        input.a_released = (i % 700) == 40;
        input.left = (i % 13) == 0;
        kf::world_tick(a, input);
        kf::world_tick(b, input);
    }
    CHECK(std::memcmp(&a, &b, sizeof(kf::World)) == 0);
}

// A patient angler with full technique must land every species at maximum
// size without ever losing one: work the reel whenever the meter is out of
// the red and come off it the moment it climbs, wiggling throughout to shed
// what builds anyway.
//
// The policy reads the tension meter alone, which is the instrument the
// player actually has on screen. An earlier version gated on the fight phase
// and on stamina hitting exactly zero; the second wind turned that zero into
// a single tick rather than a window, so the bot stopped reeling and the
// legend took all the line. Reading the meter is both what a player does and
// the technique the fight is built to teach.
void test_patient_bot_lands_everything() {
    for (int species = 0; species < kf::k_species_count; species++) {
        kf::World world;
        kf::world_init(world, 42 + species);
        kf::world_test_hook(world, species, kf::k_species[species].size_max);

        kf::Input hook{};
        hook.a_pressed = true;
        kf::world_tick(world, hook);
        CHECK(world.mode == kf::Mode::Fight);

        bool snapped = false;
        bool caught = false;
        for (int t = 0; t < 30000 && world.mode == kf::Mode::Fight; t++) {
            kf::Input input{};
            input.a = world.tension < kf::k_tension_danger - 80;
            if (t % 3 == 0) {
                if ((t / 3) % 2 == 0) input.left_pressed = true;
                else input.right_pressed = true;
            }
            kf::world_tick(world, input);
            if (world.ev.snap) snapped = true;
            if (world.ev.caught) caught = true;
        }
        if (!caught || snapped) {
            std::printf("  patient bot failed on %s\n",
                        kf::k_species[species].name);
        }
        CHECK(caught);
        CHECK(!snapped);
    }
}

// Holding the reel non stop must snap the line on strong fish most of the
// time, and must still land weak fish. This is the risk axis of the fight.
void test_greedy_bot_pays_for_it() {
    int strong_snaps = 0;
    const int trials = 20;
    for (int trial = 0; trial < trials; trial++) {
        kf::World world;
        kf::world_init(world, 1000 + trial);
        const int old_one = kf::k_species_count - 1;
        kf::world_test_hook(world, old_one, 200);
        kf::Input hook{};
        hook.a_pressed = true;
        kf::world_tick(world, hook);

        for (int t = 0; t < 30000 && world.mode == kf::Mode::Fight; t++) {
            kf::Input input{};
            input.a = true;
            kf::world_tick(world, input);
            if (world.ev.snap) { strong_snaps++; break; }
        }
    }
    std::printf("  greedy vs the legend: %d/%d snapped\n", strong_snaps, trials);
    CHECK(strong_snaps >= trials / 2);

    // The minnow forgives everything.
    kf::World world;
    kf::world_init(world, 5);
    kf::world_test_hook(world, 0, kf::k_species[0].size_max);
    kf::Input hook{};
    hook.a_pressed = true;
    kf::world_tick(world, hook);
    bool caught = false, snapped = false;
    for (int t = 0; t < 30000 && world.mode == kf::Mode::Fight; t++) {
        kf::Input input{};
        input.a = true;
        kf::world_tick(world, input);
        if (world.ev.caught) caught = true;
        if (world.ev.snap) snapped = true;
    }
    CHECK(caught);
    CHECK(!snapped);
}


// The line must never break on a spike. It breaks only after the tension has
// camped in the red zone for the full danger window, which is the player's
// chance to ease off or wiggle out of trouble.
void test_break_needs_sustained_danger() {
    kf::World world;
    kf::world_init(world, 61);
    kf::world_test_hook(world, kf::k_species_count - 1, 200);
    kf::Input hook{};
    hook.a_pressed = true;
    kf::world_tick(world, hook);

    int ticks_in_red_before_snap = 0;
    bool snapped = false;
    for (int t = 0; t < 30000 && world.mode == kf::Mode::Fight; t++) {
        kf::Input input{};
        input.a = true;   // muscle it: guaranteed to reach the red zone
        kf::world_tick(world, input);
        if (world.mode == kf::Mode::Fight &&
            world.tension >= kf::k_tension_danger) {
            ticks_in_red_before_snap++;
        }
        if (world.ev.snap) { snapped = true; break; }
    }
    CHECK(snapped);
    // The fish held on for the whole danger window, not one tick less.
    CHECK(ticks_in_red_before_snap >= kf::k_danger_ticks - 1);
}

// Tugging drains the fish; resting lets it breathe. Both directions of the
// stamina economy, verified directly.
void test_stamina_drains_and_regens() {
    kf::World world;
    kf::world_init(world, 62);
    kf::world_test_hook(world, 8, 60);   // a bass sized fish
    kf::Input hook{};
    hook.a_pressed = true;
    kf::world_tick(world, hook);
    const uint16_t fresh = world.stamina;
    CHECK(fresh == world.stamina_max);

    kf::Input reel{};
    reel.a = true;
    for (int t = 0; t < 120 && world.mode == kf::Mode::Fight; t++) {
        kf::world_tick(world, reel);
    }
    const uint16_t worked = world.stamina;
    CHECK(worked < fresh);

    kf::Input rest{};
    for (int t = 0; t < 120 && world.mode == kf::Mode::Fight; t++) {
        kf::world_tick(world, rest);
    }
    CHECK(world.mode != kf::Mode::Fight || world.stamina > worked);
}

// One alternating wiggle pair must shed real tension, and countering the run
// direction must cost the fish stamina without touching the reel.
void test_wiggle_relieves_and_tires() {
    kf::World world;
    kf::world_init(world, 63);
    kf::world_test_hook(world, kf::k_species_count - 1, 200);
    kf::Input hook{};
    hook.a_pressed = true;
    kf::world_tick(world, hook);

    // Load the line first.
    kf::Input reel{};
    reel.a = true;
    for (int t = 0; t < 60 && world.mode == kf::Mode::Fight; t++) {
        kf::world_tick(world, reel);
    }
    CHECK(world.mode == kf::Mode::Fight);
    const uint16_t loaded = world.tension;
    CHECK(loaded > 300);

    kf::Input wiggle{};
    wiggle.left_pressed = true;
    kf::world_tick(world, wiggle);
    kf::Input wiggle2{};
    wiggle2.right_pressed = true;
    kf::world_tick(world, wiggle2);
    // Two wiggles minus at most a few ticks of drift beats one relief worth.
    CHECK(world.tension + 40 < loaded);

    // A wiggle costs the fish stamina. Measured against the same world doing
    // the same tick without one, rather than over a window: the cooldown
    // means a run of ticks contains only one live wiggle, and the regen in
    // the ticks around it would swamp the reading.
    while (world.mode == kf::Mode::Fight && world.wiggle_cd > 0) {
        kf::world_tick(world, kf::Input{});
    }
    CHECK(world.mode == kf::Mode::Fight);
    kf::World idle = world;
    kf::Input flick{};
    flick.left_pressed = world.last_wiggle >= 0;
    flick.right_pressed = !flick.left_pressed;
    kf::world_tick(world, flick);
    kf::world_tick(idle, kf::Input{});
    CHECK(world.ev.wiggle);
    CHECK(world.stamina < idle.stamina);
}

// The cooldown is what keeps the wiggle a relief valve instead of an off
// switch. Alternating left and right every other tick must not be able to
// hold a fresh fish at zero effort: if it can, the reel can be held down
// from hook to net and the tension meter never means anything.
void test_wiggle_spam_cannot_pin_a_fish() {
    kf::World world;
    kf::world_init(world, 77);
    kf::world_test_hook(world, kf::k_species_count - 1, 200);
    kf::Input hook{};
    hook.a_pressed = true;
    kf::world_tick(world, hook);

    int live = 0;
    uint16_t peak_effort = 0;
    for (int t = 0; t < 600 && world.mode == kf::Mode::Fight; t++) {
        kf::Input input{};
        if (t % 2 == 0) input.left_pressed = true;
        else input.right_pressed = true;
        kf::world_tick(world, input);
        if (world.ev.wiggle) live++;
        if (world.fish_effort > peak_effort) peak_effort = world.fish_effort;
    }
    // Spamming buys no more wiggles than the cooldown allows.
    CHECK(live <= 600 / kf::k_wiggle_cooldown + 1);
    // And the fish still gets to fight: effort climbs through it.
    CHECK(peak_effort > 150);
}


// ---- the tournament ----

// Weight is what a quota is counted in, so the curve from a minnow to a
// legend has to be worth caring about. Cubed length means it is: a fish twice
// as long is eight times the fish.
void test_weight_grows_with_the_cube_of_length() {
    const uint32_t small = kf::fish_weight_g(0, 20);
    const uint32_t twice = kf::fish_weight_g(0, 40);
    CHECK(twice >= small * 7 && twice <= small * 9);
    CHECK(kf::fish_weight_g(0, 0) == 0);
    CHECK(kf::fish_weight_g(-1, 50) == 0);

    // Longer always weighs more, or a card would lie about which fish was the
    // better catch.
    uint32_t last = 0;
    for (int cm = 10; cm <= 200; cm += 10) {
        const uint32_t w = kf::fish_weight_g(0, cm);
        CHECK(w > last);
        last = w;
    }
}

// The quota climbs, and day one is the easiest day. A curve that started hard
// would end a run before it began.
void test_the_quota_climbs_every_day() {
    uint32_t last = 0;
    for (int day = 1; day <= kf::k_tour_days; day++) {
        const uint32_t target = kf::tour_target_for_day(day);
        CHECK(target > last);
        last = target;
    }
    CHECK(kf::tour_target_for_day(0) == kf::tour_target_for_day(1));
    CHECK(kf::tour_target_for_day(99) ==
          kf::tour_target_for_day(kf::k_tour_days));
}

// A day that misses its quota ends the run there and then.
void test_a_missed_quota_ends_the_run() {
    kf::World world;
    kf::world_init(world, 3);
    kf::world_start(world, kf::GameMode::Tournament);
    CHECK(world.tour_state == kf::TourState::Running);
    CHECK(world.tour_day == 1);

    for (uint32_t t = 0; t <= kf::k_day_length; t++) {
        kf::world_tick(world, kf::Input{});
    }
    CHECK(world.tour_state == kf::TourState::Lost);
    CHECK(world.tour_score == 0);   // nothing over quota, no days survived

    // A finished run stops the pond. The day clock in particular must not
    // roll on, or a lost run would keep ending days forever.
    const uint32_t day_tick = world.day_tick;
    for (int t = 0; t < 50; t++) kf::world_tick(world, kf::Input{});
    CHECK(world.tour_state == kf::TourState::Lost);
    CHECK(world.day_tick == day_tick);
}

// Making the quota carries the run to a harder day and banks the surplus.
void test_making_the_quota_carries_the_run() {
    kf::World world;
    kf::world_init(world, 4);
    kf::world_start(world, kf::GameMode::Tournament);
    const uint32_t day_one = world.tour_target_g;

    world.tour_today_g = day_one + 1000;
    for (uint32_t t = 0; t <= kf::k_day_length; t++) {
        kf::world_tick(world, kf::Input{});
        if (world.tour_state != kf::TourState::Running) break;
    }
    CHECK(world.tour_state == kf::TourState::DayPassed);
    CHECK(world.tour_day == 2);
    CHECK(world.tour_target_g > day_one);
    CHECK(world.tour_over_g == 1000);
    CHECK(world.tour_today_g == 0);
    CHECK(world.tour_card_timer > 0);

    // The card holds the run still, then the next day starts, and it starts
    // at dawn rather than wherever the last one ran out: a quota measured in
    // a day has to get a whole day.
    for (int t = 0; t < kf::k_tour_card_ticks + 2; t++) {
        kf::world_tick(world, kf::Input{});
    }
    CHECK(world.tour_state == kf::TourState::Running);
    CHECK(world.day_tick < 8);
}

// Ten days made is a win, scored as the surplus times the days.
void test_ten_days_wins_and_scores() {
    kf::World world;
    kf::world_init(world, 5);
    kf::world_start(world, kf::GameMode::Tournament);

    uint32_t expected_over = 0;
    for (int day = 1; day <= kf::k_tour_days; day++) {
        CHECK(world.tour_day == day);
        world.tour_today_g = world.tour_target_g + 500;
        expected_over += 500;
        for (uint32_t t = 0; t < kf::k_day_length + kf::k_tour_card_ticks + 4;
             t++) {
            kf::world_tick(world, kf::Input{});
            if (world.tour_state == kf::TourState::Won ||
                world.tour_state == kf::TourState::Lost) break;
            if (world.tour_state == kf::TourState::Running &&
                world.tour_day != day) break;
        }
        if (world.tour_state != kf::TourState::Running) break;
    }
    CHECK(world.tour_state == kf::TourState::Won);
    CHECK(world.tour_over_g == expected_over);
    CHECK(world.tour_score ==
          (expected_over / kf::k_tour_score_div) * kf::k_tour_days);
}

// Free fishing is the pond it always was: no quota, no clock, nothing that
// can end it.
void test_free_fishing_has_no_tournament() {
    kf::World world;
    kf::world_init(world, 6);
    kf::world_start(world, kf::GameMode::Free);
    CHECK(world.tour_state == kf::TourState::Idle);

    for (uint32_t t = 0; t < kf::k_day_length * 2 + 10; t++) {
        kf::world_tick(world, kf::Input{});
    }
    CHECK(world.tour_state == kf::TourState::Idle);
    CHECK(world.tour_today_g == 0);
    CHECK(world.tick == kf::k_day_length * 2 + 10);
}

// The board keeps the ten best, highest first.
void test_the_score_board_keeps_the_best_ten() {
    kf::Records records{};
    for (int i = 1; i <= 12; i++) kf::records_add_score(records, i * 10);
    CHECK(records.high[0] == 120);
    CHECK(records.high[1] == 110);
    CHECK(records.high[kf::k_high_scores - 1] == 30);
    for (int i = 1; i < kf::k_high_scores; i++) {
        CHECK(records.high[i - 1] >= records.high[i]);
    }

    kf::records_add_score(records, 5);    // worse than everything on it
    CHECK(records.high[kf::k_high_scores - 1] == 30);
    kf::records_add_score(records, 0);    // a run worth nothing is not a run
    CHECK(records.high[kf::k_high_scores - 1] == 30);

    kf::records_add_score(records, 115);  // slots into the middle
    CHECK(records.high[0] == 120);
    CHECK(records.high[1] == 115);
    CHECK(records.high[2] == 110);

    // Two runs worth the same both belong on the board, so an equal score
    // does go on again. That makes it the caller's job to add a run exactly
    // once, which is a real trap: a finished run stops the pond, so the event
    // that says it ended is not cleared by anything and a shell that adds on
    // the flag rather than on the edge fills the board with one score.
    kf::records_add_score(records, 115);
    CHECK(records.high[1] == 115);
    CHECK(records.high[2] == 115);
    CHECK(records.high[3] == 110);
}

// Wear a hooked fish down to its second wind the way a player does: work the
// reel while the meter is out of the red and come off it when it climbs.
//
// Simply holding the reel used to get here, and no longer does: on a strong
// fish a held pull now loads the rod past what it will stand long before the
// fish runs out of stamina, so a test that cranks from hook to net measures a
// snapped line rather than a spent fish.
void wear_down_until_spent(kf::World& world) {
    for (int t = 0; t < 30000 && world.mode == kf::Mode::Fight &&
                    world.spent_timer == 0; t++) {
        kf::Input input{};
        input.a = world.tension < kf::k_tension_danger - 80;
        if (t % 3 == 0) {
            if ((t / 3) % 2 == 0) input.left_pressed = true;
            else input.right_pressed = true;
        }
        kf::world_tick(world, input);
    }
}

// The meter has to climb with a held pull rather than switch on and off with
// it. Two ticks of cranking and two hundred must not read the same, or the
// length of a pull costs nothing and the only question the fight asks is
// whether a finger is on the button.
void test_stress_accumulates_with_a_held_pull() {
    kf::World world;
    kf::world_init(world, 31);
    kf::world_test_hook(world, kf::k_species_count - 1, 200);
    kf::Input hook{};
    hook.a_pressed = true;
    kf::world_tick(world, hook);

    kf::Input reel{};
    reel.a = true;
    for (int t = 0; t < 20 && world.mode == kf::Mode::Fight; t++) {
        kf::world_tick(world, reel);
    }
    CHECK(world.mode == kf::Mode::Fight);
    const uint16_t early = world.line_stress;

    for (int t = 0; t < 60 && world.mode == kf::Mode::Fight; t++) {
        kf::world_tick(world, reel);
    }
    CHECK(world.mode != kf::Mode::Fight || world.line_stress > early);
    CHECK(world.mode != kf::Mode::Fight || world.strain > 0);

    // And easing off has to unwind it, or a player who let it build has no
    // way back and the break is the game's decision rather than theirs.
    if (world.mode == kf::Mode::Fight) {
        const uint16_t loaded = world.strain;
        for (int t = 0; t < 40 && world.mode == kf::Mode::Fight; t++) {
            kf::world_tick(world, kf::Input{});
        }
        CHECK(world.mode != kf::Mode::Fight || world.strain < loaded);
    }
}

// Nothing in the lake may break the line on one tick's worth of pull. If it
// could, that species would be back to an on/off meter: down the reel, snap,
// with no climb to read and nothing the player could have done differently.
void test_no_fish_breaks_the_line_on_one_tick() {
    for (int species = 0; species < kf::k_species_count; species++) {
        kf::World world;
        kf::world_init(world, 200 + species);
        kf::world_test_hook(world, species, kf::k_species[species].size_max);
        kf::Input hook{};
        hook.a_pressed = true;
        kf::world_tick(world, hook);

        // Hand the fish everything: full effort, pulling away, reel down.
        world.fish_effort = 255;
        world.fish_dir = 1;
        world.strain = 0;
        kf::Input reel{};
        reel.a = true;
        kf::world_tick(world, reel);

        const int instant = world.line_stress - world.strain / kf::k_strain_fp;
        if (instant >= kf::k_rod_starter_max) {
            std::printf("  %s alone reaches %d of %d\n",
                        kf::k_species[species].name, instant,
                        kf::k_rod_starter_max);
        }
        CHECK(instant < kf::k_rod_starter_max);
    }
}

// A spent fish must not load the rod. The exhaustion window is the one time
// pulling is free, and it is the whole reward for wearing a fish down.
void test_a_spent_fish_builds_no_strain() {
    kf::World world;
    kf::world_init(world, 77);
    kf::world_test_hook(world, kf::k_species_count - 1, 200);
    kf::Input hook{};
    hook.a_pressed = true;
    kf::world_tick(world, hook);

    wear_down_until_spent(world);
    CHECK(world.mode == kf::Mode::Fight);
    CHECK(world.stamina == 0 || world.spent_timer > 0);

    // Hold the reel down through the window: the rod holds where it is
    // rather than loading further.
    kf::Input reel{};
    reel.a = true;
    const uint16_t held = world.strain;
    for (int t = 0; t < 30 && world.mode == kf::Mode::Fight &&
                    world.stamina == 0; t++) {
        kf::world_tick(world, reel);
    }
    CHECK(world.mode != kf::Mode::Fight || world.strain <= held);
}

// Running a fish out of stamina must open a real window and then close it
// again on its own: the fish goes limp, refills over about two and a half
// seconds whatever the player does, and comes back weaker than it was. That
// cycle is the fight, so all three halves of it are pinned here.
void test_second_wind_opens_and_closes() {
    kf::World world;
    kf::world_init(world, 91);
    kf::world_test_hook(world, kf::k_species_count - 1, 200);
    kf::Input hook{};
    hook.a_pressed = true;
    kf::world_tick(world, hook);
    const uint16_t first_cap = world.stamina_cap;
    CHECK(first_cap == world.stamina_max);

    // Work it down. Reeling is the only thing that empties a fish, but it has
    // to be reeling the meter allows: holding it flat out snaps the line.
    wear_down_until_spent(world);
    kf::Input reel{};
    reel.a = true;
    CHECK(world.mode == kf::Mode::Fight);
    CHECK(world.spent_timer > 0);          // the window opened
    CHECK(world.stamina_cap < first_cap);  // and cost the fish its ceiling

    // The window is the payoff: a spent fish cannot hold the line.
    CHECK(world.fish_effort < 60);

    // It refills on its own, even while the player keeps cranking, and it
    // does not last: within the recharge window the fish is back on its feet.
    const uint16_t low = world.stamina;
    for (int t = 0; t < kf::k_spent_recharge_ticks &&
                    world.mode == kf::Mode::Fight; t++) {
        kf::world_tick(world, reel);
    }
    CHECK(world.mode != kf::Mode::Fight || world.stamina > low);
    CHECK(world.mode != kf::Mode::Fight || world.spent_timer == 0);

    // And the comeback is smaller than the stand before it, every time, down
    // to a floor. Without that the cycle would never converge.
    CHECK(world.stamina_cap <= first_cap);
    CHECK(world.stamina_cap >= (world.stamina_max * kf::k_wind_cap_floor_num) /
                                   kf::k_wind_cap_floor_den);
}

// The reel is opposed by the fish, scaled by its stamina: a fresh strong
// fish gives up almost no line to the crank, a spent one comes in at full
// speed. This is what makes a fight a fight instead of a countdown.
void test_fresh_fish_resists_the_reel() {
    // A fish working flat out barely moves, a spent one comes in at the
    // fight's full rate, and neither ever matches an empty hook.
    auto reel_rate = [](int effort, int8_t dir) {
        kf::World world;
        kf::world_init(world, 71);
        kf::world_test_hook(world, kf::k_species_count - 1, 200);
        kf::Input hook{};
        hook.a_pressed = true;
        kf::world_tick(world, hook);
        world.fight_phase = kf::FightPhase::Tire;
        world.phase_timer = 5000;
        world.dir_timer = 5000;
        world.fish_dir = dir;
        world.fish_effort = static_cast<uint8_t>(effort);
        world.stamina = effort == 0 ? 0 : world.stamina_max;

        kf::Input reel{};
        reel.a = true;
        const int32_t before = world.line_len;
        for (int t = 0; t < 100 && world.mode == kf::Mode::Fight; t++) {
            // Pin the effort: this measures the rate at a given effort, not
            // the fish's own decisions about where to take it.
            world.fish_effort = static_cast<uint8_t>(effort);
            kf::world_tick(world, reel);
        }
        return before - world.line_len;   // fp gained in one second
    };

    const int32_t spent = reel_rate(0, 0);
    const int32_t fighting = reel_rate(255, 0);
    const int32_t tow = kf::k_retrieve_max_fp256 * 100 / 256;

    CHECK(spent > fighting);
    CHECK(fighting * 4 < spent);
    CHECK(spent < tow);            // a fish is never as quick as no fish
    CHECK(fighting > 0);           // but something always comes in

    // Against a fish swimming away flat out, the reel loses ground outright.
    CHECK(reel_rate(255, 1) < 0);
}

// Cranking against a fish that is swimming away is the hardest thing the
// player can ask of the line, and the meter has to say so: a quarter second
// of it must not reach the red zone (nothing teleports), but half a second
// must, or the warning arrives too late to mean anything.
void test_tension_climbs_hard_against_a_run() {
    kf::World world;
    kf::world_init(world, 73);
    kf::world_test_hook(world, kf::k_species_count - 1, 200);
    kf::Input hook{};
    hook.a_pressed = true;
    kf::world_tick(world, hook);
    world.fight_phase = kf::FightPhase::Run;
    world.phase_timer = 1000;

    kf::Input reel{};
    reel.a = true;
    for (int t = 0; t < 25 && world.mode == kf::Mode::Fight; t++) {
        kf::world_tick(world, reel);
    }
    CHECK(world.mode == kf::Mode::Fight);
    CHECK(world.tension < kf::k_tension_danger);

    for (int t = 0; t < 25 && world.mode == kf::Mode::Fight; t++) {
        kf::world_tick(world, reel);
    }
    CHECK(world.tension >= kf::k_tension_danger);
    // Reaching the red zone is a warning, never the loss itself: the line
    // still owes the player the whole danger window to escape in.
    CHECK(world.mode == kf::Mode::Fight);
}

// Reeling into a run has to cost more tension than riding it out, or there
// is no decision in the moment: the player would just always crank.
void test_reeling_into_a_run_costs_more_than_riding_it() {
    kf::World reeled;
    kf::world_init(reeled, 91);
    kf::world_test_hook(reeled, kf::k_species_count - 1, 200);
    kf::Input hook{};
    hook.a_pressed = true;
    kf::world_tick(reeled, hook);
    reeled.fight_phase = kf::FightPhase::Run;
    reeled.phase_timer = 1000;

    kf::World drifted = reeled;

    kf::Input reel{};
    reel.a = true;
    for (int t = 0; t < 60; t++) {
        kf::world_tick(reeled, reel);
        kf::world_tick(drifted, kf::Input{});
    }
    // Both the load itself and what the player is shown have to say it.
    CHECK(reeled.line_stress > drifted.line_stress);
    CHECK(reeled.tension > drifted.tension);
}

// The rule the whole meter rests on: a fish cannot break the line by itself.
// Hook the strongest fish there is, never touch the reel, and the line has to
// survive it however long the fight runs. If this ever fails, letting go
// stops being safe and the meter stops meaning anything.
void test_a_fish_alone_never_breaks_the_line() {
    for (int trial = 0; trial < 8; trial++) {
        kf::World world;
        kf::world_init(world, 3300 + trial);
        kf::world_test_hook(world, kf::k_species_count - 1, 200);
        kf::Input hook{};
        hook.a_pressed = true;
        kf::world_tick(world, hook);

        uint16_t worst = 0;
        for (int t = 0; t < 20000 && world.mode == kf::Mode::Fight; t++) {
            kf::world_tick(world, kf::Input{});   // never touch the reel
            if (world.line_stress > worst) worst = world.line_stress;
            CHECK(!world.ev.snap);
        }
        CHECK(worst < kf::k_rod_starter_max);
    }
}

// The complaint that started this: a fish on the line was coming in faster
// than a bare hook tows. However spent the fish, the reel can never beat the
// retrieve, because a fight is not a shortcut across the lake.
void test_the_reel_never_beats_the_tow() {
    const int tow_per_tick = kf::k_retrieve_max_fp256 / 256;
    CHECK(kf::k_fight_reel_max_fp256 < kf::k_retrieve_max_fp256);
    CHECK(kf::k_fight_reel_min_fp256 < kf::k_fight_reel_max_fp256);

    kf::World world;
    kf::world_init(world, 92);
    kf::world_test_hook(world, kf::k_species_count - 1, 200);
    kf::Input hook{};
    hook.a_pressed = true;
    kf::world_tick(world, hook);
    world.fight_phase = kf::FightPhase::Tire;
    world.phase_timer = 5000;
    world.stamina = 0;            // as easy as the fight ever gets

    kf::Input reel{};
    reel.a = true;
    int32_t worst = 0;
    for (int t = 0; t < 200 && world.mode == kf::Mode::Fight; t++) {
        const int32_t before = world.line_len;
        kf::world_tick(world, reel);
        const int32_t gain = before - world.line_len;
        if (gain > worst) worst = gain;
    }
    CHECK(worst <= tow_per_tick);
}


// The HUD distance reads zero exactly when the fish is collected, never a
// moment before: while the fight is on it is always positive, and the tick
// that lands the fish is the tick it goes to zero.
void test_distance_zero_means_collected() {
    kf::World idle;
    kf::world_init(idle, 80);
    CHECK(kf::hook_distance_dm(idle) == 0);   // no hook out, no number

    kf::World world;
    kf::world_init(world, 81);
    kf::world_test_hook(world, 6, 70);   // a carp, lure sinking at depth
    const int sinking_dm = kf::hook_distance_dm(world);
    CHECK(sinking_dm > 0);   // locality exists before any fish commits

    kf::Input hook{};
    hook.a_pressed = true;
    kf::world_tick(world, hook);
    CHECK(world.mode == kf::Mode::Fight);
    const int fighting_dm = kf::hook_distance_dm(world);
    CHECK(fighting_dm > 0);
    // The measure is continuous across the hook: no jump when the fight
    // takes over the number.
    const int step = fighting_dm - sinking_dm;
    CHECK(step > -10 && step < 10);

    bool caught = false;
    for (int t = 0; t < 30000 && world.mode == kf::Mode::Fight; t++) {
        kf::Input input{};
        const bool tired = world.fight_phase == kf::FightPhase::Tire ||
                           world.stamina == 0;
        input.a = tired && world.tension < kf::k_tension_danger - 80;
        if (t % 3 == 0) {
            if ((t / 3) % 2 == 0) input.left_pressed = true;
            else input.right_pressed = true;
        }
        kf::world_tick(world, input);
        if (world.mode == kf::Mode::Fight) {
            CHECK(kf::hook_distance_dm(world) > 0);
            if (g_failures > 3) return;
        }
        if (world.ev.caught) caught = true;
    }
    CHECK(caught);
    CHECK(kf::hook_distance_dm(world) == 0);
}


// A full power cast reaches about fifty meters; a limp one stays in the
// shallows. The power meter is the whole difficulty dial, so its range is
// pinned here.
void test_cast_range() {
    kf::World world;
    kf::world_init(world, 91);
    world.mode = kf::Mode::Aiming;
    world.power = 255;
    kf::Input release{};
    release.a_released = true;
    kf::world_tick(world, release);
    CHECK(world.mode == kf::Mode::Flying);
    for (int t = 0; t < 1000 && world.mode == kf::Mode::Flying; t++) {
        kf::world_tick(world, kf::Input{});
    }
    CHECK(world.mode == kf::Mode::Sinking);
    CHECK(world.lure_z > 44 * kf::k_one);
    CHECK(world.lure_z <= kf::k_lake_far_fp);

    kf::World weak;
    kf::world_init(weak, 92);
    weak.mode = kf::Mode::Aiming;
    weak.power = 0;
    kf::world_tick(weak, release);
    for (int t = 0; t < 1000 && weak.mode == kf::Mode::Flying; t++) {
        kf::world_tick(weak, kf::Input{});
    }
    CHECK(weak.mode == kf::Mode::Sinking);
    CHECK(weak.lure_z <= kf::k_shallow_max_fp);
}

// B is the instant recall: from the air or the water, one press brings the
// lure home and stands the rod down, and anything mouthing the lure flees
// rather than being left holding a bite window forever.
void test_b_recalls_instantly() {
    kf::World world;
    kf::world_init(world, 93);
    world.mode = kf::Mode::Aiming;
    world.power = 200;
    kf::Input release{};
    release.a_released = true;
    kf::world_tick(world, release);
    CHECK(world.mode == kf::Mode::Flying);
    kf::Input recall{};
    recall.b_pressed = true;
    kf::world_tick(world, recall);
    CHECK(world.mode == kf::Mode::Idle);
    CHECK(world.lure_z == kf::k_one);

    kf::World pond;
    kf::world_init(pond, 94);
    kf::world_test_hook(pond, 3, 30);   // leaves a fish Biting at the lure
    CHECK(pond.mode == kf::Mode::Sinking);
    kf::world_tick(pond, recall);
    CHECK(pond.mode == kf::Mode::Idle);
    for (const auto& fish : pond.fish) {
        CHECK(fish.state != kf::FishState::Biting);
        CHECK(fish.state != kf::FishState::Nibbling);
    }
}

// Towing the lure ramps up over about a second and cruises at about two
// meters per second, no faster. The reel ratchet clicks while line comes in
// at roughly one click per half meter, and never once the tow stops.
void test_retrieve_ramps_to_cruise() {
    kf::World world;
    kf::world_init(world, 95);
    world.mode = kf::Mode::Sinking;
    world.lure_z = 40 * kf::k_one;
    world.lure_y = kf::k_one;

    kf::Input tow{};
    tow.a = true;
    const int32_t start = world.lure_z;
    int clicks = 0;
    for (int t = 0; t < 50 && world.mode == kf::Mode::Sinking; t++) {
        kf::world_tick(world, tow);
        if (world.ev.reel_click) clicks++;
    }
    const int32_t early = start - world.lure_z;   // ramping: below cruise
    for (int t = 0; t < 150 && world.mode == kf::Mode::Sinking; t++) {
        kf::world_tick(world, tow);
        if (world.ev.reel_click) clicks++;
    }
    CHECK(world.mode == kf::Mode::Sinking);
    const int32_t late_start = world.lure_z;
    for (int t = 0; t < 100 && world.mode == kf::Mode::Sinking; t++) {
        kf::world_tick(world, tow);
        if (world.ev.reel_click) clicks++;
    }
    const int32_t cruise = late_start - world.lure_z;   // one second at max
    // The ramp: the first half second averages well under cruise.
    CHECK(early < cruise / 2 + 80);
    CHECK(cruise >= 980 && cruise <= 1075);   // ~4 m in one second
    // 3 seconds of tow covers ~10 m: one click per half meter, give or take
    // the ramp.
    CHECK(clicks >= 15 && clicks <= 26);

    // Ease off: no line moving, no ratchet.
    kf::Input rest{};
    for (int t = 0; t < 50 && world.mode == kf::Mode::Sinking; t++) {
        kf::world_tick(world, rest);
        CHECK(!world.ev.reel_click);
    }
}

// Doing nothing after the hook must end the fight too: the fish takes all the
// line and escapes. No fight lasts forever.
void test_fight_terminates_without_input() {
    kf::World world;
    kf::world_init(world, 9);
    kf::world_test_hook(world, 7, 90);   // a pike
    kf::Input hook{};
    hook.a_pressed = true;
    kf::world_tick(world, hook);

    bool over = false;
    for (int t = 0; t < 30000; t++) {
        kf::Input input{};
        kf::world_tick(world, input);
        if (world.mode != kf::Mode::Fight) { over = true; break; }
    }
    CHECK(over);
}


// Regression: fleeing fish must actually leave. Truncating steering used to
// strand them 23 fp short of the exit forever, leaking the slot until the
// pond was permanently empty.
void test_fleeing_fish_despawn() {
    kf::World world;
    kf::world_init(world, 21);
    kf::Fish& fish = world.fish[0];
    fish.species = 0;
    fish.state = kf::FishState::Flee;
    fish.size_cm = 10;
    fish.x = -2537;                       // the historical stall point
    fish.z = 4 * kf::k_one;
    fish.tx = -2560;
    fish.tz = fish.z;
    for (int t = 0; t < 3000 && fish.state != kf::FishState::Gone; t++) {
        kf::world_tick(world, kf::Input{});
        // Keep the spawner from recycling the slot mid test.
        if (fish.state == kf::FishState::Wander) fish.state = kf::FishState::Flee;
    }
    CHECK(fish.state == kf::FishState::Gone);
}

// Regression: a curious fish approaching from the positive side used to park
// fractionally out of range and never nibble.
void test_curious_fish_reaches_the_lure() {
    kf::World world;
    kf::world_init(world, 22);
    world.mode = kf::Mode::Sinking;
    world.lure_x = 0;
    world.lure_y = kf::k_one;
    world.lure_z = 4 * kf::k_one;
    kf::Fish& fish = world.fish[0];
    fish.species = 1;
    fish.state = kf::FishState::Curious;
    fish.size_cm = 20;
    fish.x = world.lure_x + 2 * kf::k_one;   // approach from +x and +z
    fish.z = world.lure_z + 2 * kf::k_one;
    bool nibbled = false;
    for (int t = 0; t < 2000; t++) {
        kf::world_tick(world, kf::Input{});
        if (fish.state == kf::FishState::Nibbling ||
            fish.state == kf::FishState::Biting) { nibbled = true; break; }
        if (fish.state != kf::FishState::Curious) break;
    }
    CHECK(nibbled);
}

// Regression: hooking used to leave bite_timer stuck at 50, painting a
// phantom bite alert on every later cast.
void test_bite_timer_cleared_by_hook() {
    kf::World world;
    kf::world_init(world, 23);
    kf::world_test_hook(world, 2, 25);
    kf::Input hook{};
    hook.a_pressed = true;
    kf::world_tick(world, hook);
    CHECK(world.mode == kf::Mode::Fight);
    CHECK(world.bite_timer == 0);
}

// Regression: only one fish may hold the bite window. A second committer used
// to refresh the shared timer and survive expiry as a hidden hookable fish.
void test_single_bite_window() {
    kf::World world;
    kf::world_init(world, 24);
    world.mode = kf::Mode::Sinking;
    world.lure_x = 0;
    world.lure_y = kf::k_one;
    world.lure_z = 4 * kf::k_one;
    world.bite_timer = 40;
    kf::Fish& first = world.fish[0];
    first.species = 1;
    first.state = kf::FishState::Biting;
    first.size_cm = 20;
    first.x = 0; first.z = 4 * kf::k_one;
    kf::Fish& second = world.fish[1];
    second.species = 2;
    second.state = kf::FishState::Nibbling;
    second.size_cm = 20;
    second.x = 0; second.z = 4 * kf::k_one;
    second.nibbles_left = 0;
    second.timer = 0;
    for (int t = 0; t < 200; t++) {
        kf::world_tick(world, kf::Input{});
        int biting = 0;
        for (const auto& f : world.fish) {
            if (f.state == kf::FishState::Biting) biting++;
        }
        if (biting > 1) { CHECK(false); return; }
        if (world.bite_timer == 0) break;
    }
    // After expiry no fish may linger in Biting.
    kf::world_tick(world, kf::Input{});
    for (const auto& f : world.fish) {
        CHECK(f.state != kf::FishState::Biting);
    }
}

// The clock is a deadline, so the two ends of it are worth pinning down: a
// tournament day opens at dawn and the last minute it can show is the one
// before midnight. If the span ever drifts, the HUD promises a player time
// that the sim does not give them.
void test_the_day_runs_dawn_to_midnight() {
    kf::World world;
    kf::world_init(world, 0xC10C1234u);
    kf::world_start(world, kf::GameMode::Tournament);

    CHECK(world.day_tick == 0);
    CHECK(kf::clock_minutes(world) == kf::k_day_start_hour * 60);

    world.day_tick = kf::k_day_length - 1;
    const uint16_t last = kf::clock_minutes(world);
    CHECK(last == kf::k_day_end_hour * 60 - 1);
    CHECK(last / 60 == 23);   // reads 11pm, and the next tick is a new day

    // Night is the sky's night, not half the tick range: mid afternoon is day
    // and late evening is night.
    world.day_tick = kf::k_day_length / 2;
    CHECK(kf::clock_minutes(world) / 60 == 15);
    CHECK(!kf::is_night(world));

    world.day_tick = kf::k_day_length - 1;
    CHECK(kf::is_night(world));

    // The clock never runs backwards or leaves the day.
    uint16_t previous = 0;
    for (uint16_t tick = 0; tick < kf::k_day_length; tick++) {
        world.day_tick = tick;
        const uint16_t now = kf::clock_minutes(world);
        if (now < previous) { check(false, "clock never goes backwards"); break; }
        if (now < kf::k_day_start_hour * 60 ||
            now >= kf::k_day_end_hour * 60) {
            check(false, "clock stays inside the fishing day");
            break;
        }
        previous = now;
    }
}

// Depth is a choice the player makes, so it has to stick, stay in the water,
// and actually gate what is willing to bite.
void test_the_hook_works_the_water_column() {
    kf::World world;
    kf::world_init(world, 0x0DEE9123u);
    kf::world_start(world, kf::GameMode::Free);

    // Cast, and let it fly and settle.
    kf::Input hold{};
    hold.a = true;
    hold.a_pressed = true;
    kf::world_tick(world, hold);
    for (int i = 0; i < 30; i++) { hold.a_pressed = false; kf::world_tick(world, hold); }
    kf::Input release{};
    release.a_released = true;
    kf::world_tick(world, release);
    for (int i = 0; i < 400 && world.mode != kf::Mode::Sinking; i++) {
        kf::world_tick(world, kf::Input{});
    }
    CHECK(world.mode == kf::Mode::Sinking);

    // Left alone it settles, as it always did, and stays in the water.
    for (int i = 0; i < 200; i++) kf::world_tick(world, kf::Input{});
    const int32_t settled = world.lure_y;
    CHECK(settled > 0);

    // Up raises it, and it stays raised once the button is let go.
    kf::Input up{};
    up.up = true;
    for (int i = 0; i < 60; i++) kf::world_tick(world, up);
    const int32_t raised = world.lure_y;
    CHECK(raised < settled);
    for (int i = 0; i < 120; i++) kf::world_tick(world, kf::Input{});
    CHECK(world.lure_y == raised);   // held, not sinking back on its own

    // It never leaves the water, however long either direction is held.
    for (int i = 0; i < 600; i++) kf::world_tick(world, up);
    CHECK(world.lure_y >= kf::k_lure_min_depth_fp);
    kf::Input down{};
    down.down = true;
    for (int i = 0; i < 600; i++) kf::world_tick(world, down);
    CHECK(world.lure_y > raised);
    CHECK(world.lure_y <= kf::k_pond_floor_fp);   // never below the viewport

    // A fish far off the lure's level never takes an interest. Park the lure
    // at the surface and a fish on the bottom right beside it, and run long
    // enough that any interest roll would have fired many times over.
    kf::Input surface{};
    surface.up = true;
    for (int i = 0; i < 300; i++) kf::world_tick(world, surface);
    for (auto& f : world.fish) f.state = kf::FishState::Gone;
    kf::Fish& deep = world.fish[0];
    deep.state = kf::FishState::Wander;
    deep.species = 0;
    deep.size_cm = 10;
    deep.x = world.lure_x;
    deep.z = world.lure_z;
    deep.y = world.lure_y + 2 * kf::k_lure_depth_reach_fp;
    deep.tx = deep.x;
    deep.tz = deep.z;
    deep.timer = 40000;
    for (int i = 0; i < 1200; i++) {
        kf::world_tick(world, surface);
        if (world.fish[0].state != kf::FishState::Wander) break;
        world.fish[0].y = world.lure_y + 2 * kf::k_lure_depth_reach_fp;
    }
    CHECK(world.fish[0].state == kf::FishState::Wander);
}

// Nothing lives below the bottom of the underwater viewport, because the
// player would be fishing for something they cannot see.
void test_nothing_swims_below_the_viewport() {
    kf::World world;
    kf::world_init(world, 0x5EA1F00Du);
    kf::world_start(world, kf::GameMode::Free);

    kf::Input cast{};
    cast.a = true;
    cast.a_pressed = true;
    kf::world_tick(world, cast);
    for (int i = 0; i < 40; i++) { cast.a_pressed = false; kf::world_tick(world, cast); }
    kf::Input release{};
    release.a_released = true;
    kf::world_tick(world, release);

    kf::Input down{};
    down.down = true;
    for (int i = 0; i < 4000; i++) {
        kf::world_tick(world, down);
        if (world.lure_y > kf::k_pond_floor_fp) {
            check(false, "the hook stays above the pond floor");
            break;
        }
        for (const auto& f : world.fish) {
            if (f.state == kf::FishState::Gone) continue;
            if (f.y > kf::k_pond_floor_fp) {
                check(false, "no fish swims below the pond floor");
                return;
            }
        }
    }
}

void test_records_and_save_roundtrip() {
    kf::World world;
    kf::world_init(world, 11);
    kf::world_test_hook(world, 2, 28);
    kf::Input hook{};
    hook.a_pressed = true;
    kf::world_tick(world, hook);
    for (int t = 0; t < 30000 && world.mode == kf::Mode::Fight; t++) {
        kf::Input input{};
        input.a = world.fight_phase == kf::FightPhase::Tire ||
                  world.stamina == 0;
        kf::world_tick(world, input);
    }
    CHECK(world.records.caught[2] == 1);
    CHECK(world.records.best_cm[2] == 28);
    CHECK(world.records.score > 0);
    CHECK(world.save_pending);

    kf::SaveData data;
    kf::world_make_save(world, data);
    kf::World fresh;
    kf::world_init(fresh, 12);
    CHECK(kf::world_load(fresh, data));
    CHECK(fresh.records.best_cm[2] == 28);

    data.magic = 0xDEADBEEF;
    CHECK(!kf::world_load(fresh, data));
}

void test_memory_budget() {
    CHECK(sizeof(kf::World) <= 768);
    CHECK(sizeof(kf::Fish) <= 44);
    CHECK(sizeof(kf::SaveData) <= 128);
}

}  // namespace

// Any allocation inside world_tick is a bug: the whole sim is fixed size by
// design, because heap exhaustion on a 264 KB device is not a recoverable
// event.
void* operator new(size_t size) {
    if (g_alloc_guard) {
        std::fprintf(stderr, "allocation inside the sim\n");
        std::abort();
    }
    return std::malloc(size);
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, size_t) noexcept { std::free(p); }

int main() {
    test_monkey();
    test_determinism();
    test_patient_bot_lands_everything();
    test_greedy_bot_pays_for_it();
    test_break_needs_sustained_danger();
    test_weight_grows_with_the_cube_of_length();
    test_the_quota_climbs_every_day();
    test_the_day_runs_dawn_to_midnight();
    test_the_hook_works_the_water_column();
    test_nothing_swims_below_the_viewport();
    test_a_missed_quota_ends_the_run();
    test_making_the_quota_carries_the_run();
    test_ten_days_wins_and_scores();
    test_free_fishing_has_no_tournament();
    test_the_score_board_keeps_the_best_ten();
    test_stamina_drains_and_regens();
    test_second_wind_opens_and_closes();
    test_wiggle_spam_cannot_pin_a_fish();
    test_stress_accumulates_with_a_held_pull();
    test_no_fish_breaks_the_line_on_one_tick();
    test_a_spent_fish_builds_no_strain();
    test_wiggle_relieves_and_tires();
    test_fresh_fish_resists_the_reel();
    test_tension_climbs_hard_against_a_run();
    test_reeling_into_a_run_costs_more_than_riding_it();
    test_the_reel_never_beats_the_tow();
    test_a_fish_alone_never_breaks_the_line();
    test_distance_zero_means_collected();
    test_cast_range();
    test_b_recalls_instantly();
    test_retrieve_ramps_to_cruise();
    test_fight_terminates_without_input();
    test_fleeing_fish_despawn();
    test_curious_fish_reaches_the_lure();
    test_bite_timer_cleared_by_hook();
    test_single_bite_window();
    test_records_and_save_roundtrip();
    test_memory_budget();

    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
