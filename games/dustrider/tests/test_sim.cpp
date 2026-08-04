// Host side tests for Dust Rider's sim. Pure integer C++, so the promises
// the game is balanced on are proven here instead of asserted in comments:
// the window can never outrun a flat out bike, the generator never deals an
// undodgeable hand, and the same seed always rides the same run.

#include <cstdio>
#include <cstring>

#include "bot.hpp"
#include "sim.hpp"
#include "tuning.hpp"

namespace {

int g_failures = 0;
int g_checks = 0;

void check(bool condition, const char* expression, const char* file, int line) {
    g_checks++;
    if (condition) return;
    g_failures++;
    std::printf("FAIL %s:%d: %s\n", file, line, expression);
}

#define CHECK(expr) check((expr), #expr, __FILE__, __LINE__)

dr::Input throttle_only() {
    dr::Input in{};
    in.throttle = true;
    return in;
}

void run(dr::World& world, const dr::Input& input, int ticks) {
    for (int i = 0; i < ticks; i++) dr::world_tick(world, input);
}

// ---- physics ----

// Terminal velocity on the flat road must be the documented k_bike_vmax,
// and the screen cap must be exactly 90% of it: the project rule that the
// window can never move faster than the bike is a compile time inequality
// plus this measured check.
void test_top_speed_and_screen_cap() {
    static_assert(dr::k_screen_vmax * 10 <= dr::k_bike_vmax * 9,
                  "the window cap must never exceed 90% of bike top speed");
    static_assert(dr::k_screen_vmax * 10 > dr::k_bike_vmax * 9 - 10,
                  "the window cap should sit right at 90%, not below it");

    // Measure pure physics: re-center the window every tick so the ride
    // never ends before the speed converges.
    dr::World fresh;
    dr::world_init(fresh, 1);
    dr::world_test_flat(fresh, true);
    dr::world_test_clear_hazards(fresh);
    int32_t v_peak = 0;
    for (int i = 0; i < 4000; i++) {
        dr::world_tick(fresh, throttle_only());
        fresh.screen_x = fresh.x;
        if (fresh.v > v_peak) v_peak = fresh.v;
    }
    CHECK(fresh.alive);
    CHECK(v_peak > dr::k_bike_vmax - 300);
    CHECK(v_peak <= dr::k_bike_vmax);
    CHECK(v_peak > dr::k_screen_vmax);   // the bike can always gain
}

// The window's speed never exceeds its cap and never changes faster than
// its own acceleration limit, which is far below the bike's thrust.
void test_screen_is_bounded() {
    dr::World world;
    dr::world_init(world, 7);
    dr::world_test_flat(world, true);
    dr::world_test_clear_hazards(world);
    int32_t last_v = 0;
    bool released = false;
    for (int i = 0; i < 12000; i++) {
        dr::world_tick(world, throttle_only());
        if (!world.alive) break;
        if (released) {
            const int32_t dv = world.screen_v - last_v;
            CHECK(dv <= dr::k_screen_accel && dv >= -dr::k_screen_accel);
        }
        released = world.started &&
                   world.tick - world.start_tick > dr::k_start_grace;
        CHECK(world.screen_v <= dr::k_screen_vmax);
        last_v = world.screen_v;
    }
}

// Refusing to ride loses to the window's left edge; riding flat out forever
// walks out of its right edge. Both ends of the rule are real deaths.
void test_window_kills_both_ways() {
    dr::World slow;
    dr::world_init(slow, 3);
    dr::world_test_flat(slow, true);
    dr::world_test_clear_hazards(slow);
    run(slow, throttle_only(), 50);      // start the run, then give up
    dr::Input idle{};
    run(slow, idle, 6000);
    CHECK(!slow.alive);
    CHECK(slow.death == dr::Death::Behind);

    dr::World fast;
    dr::world_init(fast, 3);
    dr::world_test_flat(fast, true);
    dr::world_test_clear_hazards(fast);
    run(fast, throttle_only(), 20000);
    CHECK(!fast.alive);
    CHECK(fast.death == dr::Death::Ahead);
}

// Sand is slow: terminal velocity on the shoulder must sit well below the
// road's, low enough that the shoulder cannot win the race for long.
void test_sand_slows() {
    dr::World road;
    dr::world_init(road, 5);
    dr::world_test_flat(road, true);
    dr::world_test_clear_hazards(road);

    dr::World sand = road;
    dr::Input sand_in = throttle_only();
    sand_in.to_sand = true;

    int32_t road_peak = 0, sand_peak = 0;
    for (int i = 0; i < 1500; i++) {
        dr::world_tick(road, throttle_only());
        dr::world_tick(sand, sand_in);
        road.screen_x = road.x;     // keep the window out of the physics
        sand.screen_x = sand.x;
        if (road.v > road_peak) road_peak = road.v;
        if (sand.v > sand_peak) sand_peak = sand.v;
    }
    CHECK(sand.z > dr::k_road_edge_z);
    CHECK(sand_peak < (road_peak * 6) / 10);
    CHECK(sand_peak < dr::k_screen_vmax);
}

// ---- hazards ----

void test_cactus_kills_in_lane_only() {
    dr::World world;
    dr::world_init(world, 11);
    dr::world_test_flat(world, true);
    dr::world_test_clear_hazards(world);
    dr::world_test_place_cactus(world, world.x + (12 << 8), false);
    run(world, throttle_only(), 800);
    CHECK(!world.alive);
    CHECK(world.death == dr::Death::Cactus);

    // The same cactus on the sand lane never touches a rider on the road.
    dr::World safe;
    dr::world_init(safe, 11);
    dr::world_test_flat(safe, true);
    dr::world_test_clear_hazards(safe);
    dr::world_test_place_cactus(safe, safe.x + (12 << 8), true);
    for (int i = 0; i < 800 && safe.alive; i++) {
        dr::world_tick(safe, throttle_only());
        CHECK(safe.death != dr::Death::Cactus);
    }
}

void test_rail_kills_crossing_not_riding() {
    // Crossing the road edge under a rail is death.
    dr::World crossing;
    dr::world_init(crossing, 13);
    dr::world_test_flat(crossing, true);
    dr::world_test_clear_hazards(crossing);
    for (int32_t x = 0; x < (200 << 8); x += dr::k_chunk_len) {
        dr::world_test_set_rail(crossing, x, true);
    }
    dr::Input swerve = throttle_only();
    run(crossing, swerve, 60);
    swerve.to_sand = true;
    run(crossing, swerve, 200);
    CHECK(!crossing.alive);
    CHECK(crossing.death == dr::Death::Rail);

    // Riding straight down a railed road is fine.
    dr::World straight;
    dr::world_init(straight, 13);
    dr::world_test_flat(straight, true);
    dr::world_test_clear_hazards(straight);
    for (int32_t x = 0; x < (200 << 8); x += dr::k_chunk_len) {
        dr::world_test_set_rail(straight, x, true);
    }
    for (int i = 0; i < 500 && straight.alive; i++) {
        dr::world_tick(straight, throttle_only());
        CHECK(straight.death != dr::Death::Rail);
    }
}

// ---- generation fairness ----

// Audit thousands of chunks per seed: every cactus is dodgeable (never in
// or near a railed stretch, never closer to the last one than a lane
// change), and the terrain stays inside its stated bounds.
void test_generation_is_fair() {
    for (uint32_t seed = 1; seed <= 40; seed++) {
        dr::World world;
        dr::world_init(world, seed * 977u);

        int last_cactus = -1000;
        int rail_run_end = -1000;
        bool prev_rail = false;
        int16_t prev_h = 0;
        bool have_prev_h = false;

        for (int i = 0; i < 3000; i++) {
            const dr::Chunk chunk = dr::world_test_generate_chunk(world);
            const int index = world.gen_next - 1;
            const bool rail = (chunk.flags & dr::k_flag_rail) != 0;
            const bool cactus = (chunk.flags & dr::k_flag_cactus) != 0;

            if (prev_rail && !rail) rail_run_end = index;
            prev_rail = rail;

            if (cactus) {
                CHECK(!rail);
                CHECK(index - rail_run_end >= dr::k_rail_clear_chunks);
                CHECK(index - last_cactus >= dr::k_cactus_min_gap);
                last_cactus = index;

                // And no rail follows too closely for the dodge back.
                dr::World peek = world;
                for (int j = 0; j < dr::k_rail_clear_chunks; j++) {
                    const dr::Chunk ahead = dr::world_test_generate_chunk(peek);
                    CHECK(!(ahead.flags & dr::k_flag_rail));
                }
            }

            CHECK(chunk.h <= dr::k_height_limit);
            CHECK(chunk.h >= -dr::k_height_limit);
            if (have_prev_h) {
                const int dh = chunk.h - prev_h;
                CHECK(dh <= dr::k_slope_max * 2 && dh >= -dr::k_slope_max * 2);
            }
            prev_h = chunk.h;
            have_prev_h = true;
        }
    }
}

// ---- the whole game holds together ----

// The bot from the title screen is deliberately simple: station keeping
// plus one lane dodge. Runs are SUPPOSED to end eventually: the window's
// ramp is the roguelike difficulty. What must never happen is a trap: the
// bot has to ride deep into the escalation and, when it finally loses, it
// must lose the race, not hit something it was never given room to dodge.
void test_bot_survives() {
    for (uint32_t seed = 1; seed <= 6; seed++) {
        dr::World world;
        dr::world_init(world, seed * 31337u);
        for (int i = 0; i < 25000 && world.alive; i++) {
            dr::world_tick(world, dr::bot_input(world));
        }
        std::printf("  bot seed %u: %dm, %s (cause %d)\n", seed,
                    dr::distance_m(world),
                    world.alive ? "alive" : "dead",
                    static_cast<int>(world.death));
        CHECK(dr::distance_m(world) > 1200);
        CHECK(world.death != dr::Death::Cactus);
        CHECK(world.death != dr::Death::Rail);
    }
}

// Crests launch the bike and the ground always catches it again.
void test_airtime_happens_and_ends() {
    dr::World world;
    dr::world_init(world, 42);
    bool flew = false;
    int air_ticks = 0;
    for (int i = 0; i < 25000 && world.alive; i++) {
        dr::world_tick(world, dr::bot_input(world));
        if (!world.grounded) {
            flew = true;
            air_ticks++;
            CHECK(air_ticks < 600);   // nothing stays up six seconds
        } else {
            air_ticks = 0;
        }
        const int32_t ground = dr::track_height(world, world.x);
        CHECK((world.y16 >> 8) >= ground - 2);
    }
    CHECK(flew);
}

// A run is a pure function of its seed.
void test_determinism() {
    dr::World a, b;
    dr::world_init(a, 20260804);
    dr::world_init(b, 20260804);
    for (int i = 0; i < 8000; i++) {
        dr::world_tick(a, dr::bot_input(a));
        dr::world_tick(b, dr::bot_input(b));
    }
    CHECK(a.x == b.x);
    CHECK(a.v == b.v);
    CHECK(a.y16 == b.y16);
    CHECK(a.rng == b.rng);
    CHECK(a.alive == b.alive);
}

// ---- persistence ----

void test_save_roundtrip() {
    dr::World world;
    dr::world_init(world, 9);
    world.best_m = 4242;
    dr::SaveData data;
    dr::world_make_save(world, data);

    dr::World other;
    dr::world_init(other, 10);
    CHECK(dr::world_load(other, data));
    CHECK(other.best_m == 4242);

    data.magic = 0xBAD0BAD0u;
    CHECK(!dr::world_load(other, data));
}

void test_memory_budget() {
    CHECK(sizeof(dr::World) <= 1024);
    CHECK(sizeof(dr::SaveData) <= 16);
}

}  // namespace

int main() {
    test_top_speed_and_screen_cap();
    test_screen_is_bounded();
    test_window_kills_both_ways();
    test_sand_slows();
    test_cactus_kills_in_lane_only();
    test_rail_kills_crossing_not_riding();
    test_generation_is_fair();
    test_bot_survives();
    test_airtime_happens_and_ends();
    test_determinism();
    test_save_roundtrip();
    test_memory_budget();

    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
