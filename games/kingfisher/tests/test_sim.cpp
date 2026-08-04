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

// A patient angler reels only while the fish is tired, and must land every
// species at maximum size without ever snapping the line.
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
            input.a = world.fight_phase == kf::FightPhase::Tire ||
                      world.stamina == 0;
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
