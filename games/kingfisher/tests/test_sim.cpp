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
    if (world.lure_x < -12 * kf::k_one || world.lure_x > 12 * kf::k_one ||
        world.lure_z < -2 * kf::k_one || world.lure_z > 18 * kf::k_one) {
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
// size without ever losing one: reel while the fish is tired and the tension
// is safe, wiggle the rod throughout to shed tension and wear down runs.
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
            const bool tired = world.fight_phase == kf::FightPhase::Tire ||
                               world.stamina == 0;
            input.a = tired && world.tension < kf::k_tension_danger - 80;
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

    // Counter wiggles during a run drain stamina faster than resting does.
    while (world.mode == kf::Mode::Fight &&
           world.fight_phase != kf::FightPhase::Run) {
        kf::world_tick(world, kf::Input{});
    }
    CHECK(world.mode == kf::Mode::Fight);
    const uint16_t before = world.stamina;
    int flips = 0;
    for (int t = 0; t < 30 && world.mode == kf::Mode::Fight &&
                    world.fight_phase == kf::FightPhase::Run; t++) {
        kf::Input input{};
        if (t % 3 == 0) {
            if (flips++ % 2 == 0) input.left_pressed = true;
            else input.right_pressed = true;
        }
        kf::world_tick(world, input);
    }
    CHECK(world.mode != kf::Mode::Fight || world.stamina < before);
}


// The reel is opposed by the fish, scaled by its stamina: a fresh strong
// fish gives up almost no line to the crank, a spent one comes in at full
// speed. This is what makes a fight a fight instead of a countdown.
void test_fresh_fish_resists_the_reel() {
    kf::World world;
    kf::world_init(world, 71);
    kf::world_test_hook(world, kf::k_species_count - 1, 200);
    kf::Input hook{};
    hook.a_pressed = true;
    kf::world_tick(world, hook);
    CHECK(world.mode == kf::Mode::Fight);

    // Pin the phase so the comparison isolates stamina.
    world.fight_phase = kf::FightPhase::Tire;
    world.phase_timer = 1000;

    kf::Input reel{};
    reel.a = true;
    const int32_t before_fresh = world.line_len;
    kf::world_tick(world, reel);
    const int32_t gain_fresh = before_fresh - world.line_len;

    world.stamina = 0;
    const int32_t before_spent = world.line_len;
    kf::world_tick(world, reel);
    const int32_t gain_spent = before_spent - world.line_len;

    CHECK(gain_fresh >= 0);
    CHECK(gain_spent > gain_fresh);
    CHECK(gain_fresh * 2 < gain_spent);

    // And against a fresh run, the reel loses ground outright.
    kf::World running;
    kf::world_init(running, 72);
    kf::world_test_hook(running, kf::k_species_count - 1, 200);
    kf::world_tick(running, hook);
    running.fight_phase = kf::FightPhase::Run;
    running.phase_timer = 1000;
    const int32_t before_run = running.line_len;
    kf::world_tick(running, reel);
    CHECK(running.line_len > before_run);
}

// The tension climbs toward the red zone instead of teleporting there:
// nearly a second of flat out greed against the legend must not reach it.
void test_tension_builds_gradually() {
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
    for (int t = 0; t < 55 && world.mode == kf::Mode::Fight; t++) {
        kf::world_tick(world, reel);
    }
    CHECK(world.mode == kf::Mode::Fight);
    CHECK(world.tension < kf::k_tension_danger);
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
    CHECK(sizeof(kf::SaveData) <= 64);
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
    test_stamina_drains_and_regens();
    test_wiggle_relieves_and_tires();
    test_fresh_fish_resists_the_reel();
    test_tension_builds_gradually();
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
