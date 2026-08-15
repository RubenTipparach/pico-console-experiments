// Host side tests for Dust Rider's sim. Pure integer C++, so the promises
// the game is balanced on are proven here instead of asserted in comments:
// the window can never outrun a flat out bike, the road is always
// followable at top speed, no cactus ever grows where the tarmac is, and
// the same seed always rides the same run.

#include <cstdio>

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

// A world with straight road and no hazards, for measuring pure physics.
void init_bare(dr::World& world, uint32_t seed) {
    dr::world_init(world, seed);
    dr::world_test_straight(world, true);
    dr::world_test_clear_hazards(world);
}

// ---- physics ----

// Terminal velocity on the road must be the documented k_bike_vmax, and
// the screen cap must be exactly 90% of it: the project rule that the
// window can never move faster than the bike is a compile time inequality
// plus this measured check.
void test_top_speed_and_screen_cap() {
    static_assert(dr::k_screen_vmax * 10 <= dr::k_bike_vmax * 9,
                  "the window cap must never exceed 90% of bike top speed");
    static_assert(dr::k_screen_vmax * 10 > dr::k_bike_vmax * 9 - 10,
                  "the window cap should sit right at 90%, not below it");

    // Re-center the window every tick so the ride never ends before the
    // speed converges.
    dr::World world;
    init_bare(world, 1);
    int32_t v_peak = 0;
    for (int i = 0; i < 4000; i++) {
        dr::world_tick(world, throttle_only());
        world.screen_x = world.x;
        if (world.v > v_peak) v_peak = world.v;
    }
    CHECK(world.alive);
    CHECK(v_peak > dr::k_bike_vmax - 300);
    CHECK(v_peak <= dr::k_bike_vmax);
    CHECK(v_peak > dr::k_screen_vmax);   // the bike can always gain
}

// The window's speed never exceeds its cap and never changes faster than
// its own acceleration limit, which is far below the bike's thrust.
void test_screen_is_bounded() {
    dr::World world;
    init_bare(world, 7);
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
    init_bare(slow, 3);
    run(slow, throttle_only(), 50);      // start the run, then give up
    run(slow, dr::Input{}, 6000);
    CHECK(!slow.alive);
    CHECK(slow.death == dr::Death::Behind);

    dr::World fast;
    init_bare(fast, 3);
    run(fast, throttle_only(), 20000);
    CHECK(!fast.alive);
    CHECK(fast.death == dr::Death::Ahead);
}

// Sand is slow: terminal velocity off the tarmac must sit well below the
// road's, low enough that the shoulder cannot win the race for long.
void test_sand_slows() {
    dr::World road;
    init_bare(road, 5);
    dr::World sand = road;
    dr::Input sand_in = throttle_only();
    sand_in.north = true;

    int32_t road_peak = 0, sand_peak = 0;
    for (int i = 0; i < 1500; i++) {
        dr::world_tick(road, throttle_only());
        dr::world_tick(sand, sand_in);
        road.screen_x = road.x;     // keep the window out of the physics
        sand.screen_x = sand.x;
        if (road.v > road_peak) road_peak = road.v;
        if (sand.v > sand_peak) sand_peak = sand.v;
    }
    CHECK(dr::off_road(sand));
    CHECK(!dr::off_road(road));
    CHECK(sand_peak < (road_peak * 6) / 10);
    CHECK(sand_peak < dr::k_screen_vmax);
}

// The dunes bound how far into the sand the bike can get, measured from
// the centerline wherever the road has wandered to.
void test_offroad_is_bounded() {
    dr::World world;
    dr::world_init(world, 17);
    dr::Input in = throttle_only();
    in.north = true;
    for (int i = 0; i < 2000 && world.alive; i++) {
        dr::world_tick(world, in);
        CHECK(dr::road_offset(world) <= dr::k_offroad_max);
    }
    dr::World south;
    dr::world_init(south, 17);
    dr::Input in_s = throttle_only();
    in_s.south = true;
    for (int i = 0; i < 2000 && south.alive; i++) {
        dr::world_tick(south, in_s);
        CHECK(dr::road_offset(south) >= -dr::k_offroad_max);
    }
}

// ---- hazards ----

// A cactus kills off the tarmac, and can never be touched from on it. The
// geometric half of that promise is a static_assert in tuning.hpp; this is
// the behavioural half.
void test_cactus_kills_off_road_only() {
    dr::World world;
    init_bare(world, 11);
    dr::world_test_place_cactus(world, world.x + (12 << 8),
                                dr::k_cactus_off_min);
    for (int i = 0; i < 800 && world.alive; i++) {
        dr::Input in = throttle_only();
        // Steer out to the cactus's own line and hold it, rather than
        // sailing past it to the dunes beyond.
        in.north = dr::road_offset(world) < dr::k_cactus_off_min;
        dr::world_tick(world, in);
        world.screen_x = world.x;    // this test is about the cactus
    }
    CHECK(!world.alive);
    CHECK(world.death == dr::Death::Cactus);

    // The same cactus never reaches a rider who stays on the road, even
    // riding its north edge.
    dr::World safe;
    init_bare(safe, 11);
    dr::world_test_place_cactus(safe, safe.x + (12 << 8),
                                dr::k_cactus_off_min);
    for (int i = 0; i < 800 && safe.alive; i++) {
        dr::Input hold = throttle_only();
        // Hug the north edge without crossing it.
        hold.north = dr::road_offset(safe) < dr::k_road_half - dr::k_steer_rate;
        dr::world_tick(safe, hold);
        CHECK(!dr::off_road(safe));
        CHECK(safe.death != dr::Death::Cactus);
    }
}

void test_rail_kills_leaving_not_riding() {
    // Crossing the north edge under a rail is death.
    dr::World crossing;
    init_bare(crossing, 13);
    for (int32_t x = 0; x < (200 << 8); x += dr::k_chunk_len) {
        dr::world_test_set_rail(crossing, x, true);
    }
    dr::Input swerve = throttle_only();
    run(crossing, swerve, 60);
    swerve.north = true;
    run(crossing, swerve, 200);
    CHECK(!crossing.alive);
    CHECK(crossing.death == dr::Death::Rail);

    // Riding the railed road is fine, and so is bailing SOUTH: the rail is
    // one wall, not a tunnel.
    dr::World straight;
    init_bare(straight, 13);
    for (int32_t x = 0; x < (200 << 8); x += dr::k_chunk_len) {
        dr::world_test_set_rail(straight, x, true);
    }
    dr::Input south = throttle_only();
    south.south = true;
    for (int i = 0; i < 500 && straight.alive; i++) {
        dr::world_tick(straight, south);
        straight.screen_x = straight.x;   // this test is about the rail
        CHECK(straight.death != dr::Death::Rail);
    }
    CHECK(straight.alive);
    CHECK(dr::off_road(straight));
}

// ---- generation fairness ----

// The tightest bend taken at top speed must demand less lateral speed than
// the bike can steer, with room to spare. This is the whole reason the road
// is followable, and it is arithmetic, not opinion.
void test_curve_is_followable() {
    // Ticks to cross one chunk at top speed. v is 16.16 m/tick and a chunk
    // is k_chunk_len fp8, so ticks = chunk * 256 / v.
    const int32_t ticks = (dr::k_chunk_len * 256) / dr::k_bike_vmax;
    CHECK(ticks > 0);
    const int32_t demand = dr::k_curve_max / ticks;   // fp8 z per tick
    CHECK(demand * 2 <= dr::k_steer_rate);
}

// Audit thousands of chunks per seed: every cactus sits clear of the road
// and clear of the rails, the road never bends harder than it promises,
// and the centerline stays inside its band.
void test_generation_is_fair() {
    for (uint32_t seed = 1; seed <= 40; seed++) {
        dr::World world;
        dr::world_init(world, seed * 977u);

        int last_cactus = -1000;
        int rail_run_end = -1000;
        bool prev_rail = false;
        int16_t prev_c = 0;
        bool have_prev_c = false;
        int cacti = 0;

        for (int i = 0; i < 3000; i++) {
            const dr::Chunk chunk = dr::world_test_generate_chunk(world);
            const int index = world.gen_next - 1;
            const bool rail = (chunk.flags & dr::k_flag_rail) != 0;
            const bool cactus = (chunk.flags & dr::k_flag_cactus) != 0;

            if (prev_rail && !rail) rail_run_end = index;
            prev_rail = rail;

            if (cactus) {
                cacti++;
                CHECK(!rail);
                CHECK(index - rail_run_end >= dr::k_rail_clear_chunks);
                CHECK(index - last_cactus >= dr::k_cactus_min_gap);
                last_cactus = index;

                // Off the tarmac by construction, and reachable.
                const int32_t off =
                    dr::k_cactus_off_min + chunk.cactus_z * 4;
                CHECK(off - dr::k_cactus_z_reach > dr::k_road_half);
                CHECK(off < dr::k_offroad_max);

                // And no rail follows too closely, which would put the
                // cactus behind a wall.
                dr::World peek = world;
                for (int j = 0; j < dr::k_rail_clear_chunks; j++) {
                    const dr::Chunk ahead = dr::world_test_generate_chunk(peek);
                    CHECK(!(ahead.flags & dr::k_flag_rail));
                }
            }

            CHECK(chunk.c <= dr::k_center_limit);
            CHECK(chunk.c >= -dr::k_center_limit);
            if (have_prev_c) {
                const int dc = chunk.c - prev_c;
                CHECK(dc <= dr::k_curve_max && dc >= -dr::k_curve_max);
            }
            prev_c = chunk.c;
            have_prev_c = true;
        }
        CHECK(cacti > 20);   // a shoulder with no cacti is not a hazard
    }
}

// The road must actually bend: a generator that quietly emits a straight
// line would pass every safety check above.
void test_road_actually_curves() {
    dr::World world;
    dr::world_init(world, 4242);
    int16_t lo = 0, hi = 0;
    int bends = 0;
    int16_t prev = 0;
    for (int i = 0; i < 600; i++) {
        const dr::Chunk chunk = dr::world_test_generate_chunk(world);
        if (chunk.c < lo) lo = chunk.c;
        if (chunk.c > hi) hi = chunk.c;
        if (i > 0) {
            const int dc = chunk.c - prev;
            if (dc > 40 || dc < -40) bends++;
        }
        prev = chunk.c;
    }
    CHECK(hi - lo > 1024);   // at least 4 m of wander
    CHECK(bends > 100);      // and it spends real time turning
}

// ---- the whole game holds together ----

// The bot steers at the centerline and manages the throttle, nothing else.
// Runs are SUPPOSED to end eventually: the window's ramp is the roguelike
// difficulty. What must never happen is a trap, so when the bot finally
// loses it has to lose the race, never a collision it was given no room to
// avoid.
void test_bot_survives() {
    for (uint32_t seed = 1; seed <= 6; seed++) {
        dr::World world;
        dr::world_init(world, seed * 31337u);
        int32_t worst_off = 0;
        for (int i = 0; i < 25000 && world.alive; i++) {
            dr::world_tick(world, dr::bot_input(world));
            const int32_t off = dr::road_offset(world);
            const int32_t mag = off < 0 ? -off : off;
            if (mag > worst_off) worst_off = mag;
        }
        std::printf("  bot seed %u: %dm, %s (cause %d), worst offset %d\n",
                    seed, dr::distance_m(world),
                    world.alive ? "alive" : "dead",
                    static_cast<int>(world.death), worst_off);
        CHECK(dr::distance_m(world) > 1200);
        CHECK(world.death != dr::Death::Cactus);
        CHECK(world.death != dr::Death::Rail);
        // Following the road means staying ON it, not carving the sand.
        CHECK(worst_off <= dr::k_road_half);
    }
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
    CHECK(a.z == b.z);
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

void test_the_ride_says_what_it_sounds_like() {
    // The sim tells the sound layer what happened; the sound layer holds no
    // state about the ride and the ride holds no opinion about the sound.
    dr::World world;
    init_bare(world, 3);

    dr::Input idle{};
    dr::world_tick(world, idle);
    CHECK(!world.ev.launched);
    CHECK(world.ev.rev == 0);

    dr::Input go = throttle_only();
    dr::world_tick(world, go);
    CHECK(world.ev.launched);        // exactly once, on the first throttle
    dr::world_tick(world, go);
    CHECK(!world.ev.launched);

    // The engine climbs with speed, or the drone it drives is a constant tone
    // with a volume knob.
    //
    // The window is re-centred every tick, the way the top speed test does it:
    // held flat out with the chase running, this ride ends against the window's
    // right edge after 38 metres, and a rev that never gets near the top is a
    // measurement of that rather than of the engine.
    uint8_t quiet = 255, loud = 0;
    int milestones = 0;
    for (int i = 0; i < 4000 && world.alive; i++) {
        dr::world_tick(world, go);
        world.screen_x = world.x;
        if (world.ev.rev < quiet) quiet = world.ev.rev;
        if (world.ev.rev > loud) loud = world.ev.rev;
        if (world.ev.milestone) milestones++;
    }
    CHECK(world.alive);
    CHECK(loud > 200);
    CHECK(loud - quiet > 80);
    // One chime per hundred metres, and no more: a milestone counted off a
    // countdown rather than off the distance actually crossed can be stepped
    // over by a fast bike, or fire twice at a boundary.
    CHECK(milestones == dr::distance_m(world) / 100);

    std::printf("  ride: rev %d..%d, %d milestones over %dm\n", quiet, loud,
                milestones, dr::distance_m(world));
}

void test_a_frame_hears_every_tick_in_it() {
    // Edges accumulate, levels take the latest value. OR'd like the rest, the
    // rev would stick at the loudest tick of the frame and one tick in the
    // sand would latch the rumble on for all of it.
    dr::Events frame{};
    dr::Events a{};
    a.milestone = true;
    a.rev = 250;
    a.offroad = true;
    dr::Events b{};
    b.died = true;
    b.rev = 40;
    b.offroad = false;

    dr::merge_events(frame, a);
    dr::merge_events(frame, b);
    CHECK(frame.milestone);
    CHECK(frame.died);
    CHECK(frame.rev == 40);
    CHECK(!frame.offroad);
}

void test_sound_survives_a_save_written_before_it_existed() {
    // sound_off rather than sound_on, so the zero an older save has in that
    // byte means what it always meant. Written the other way round every
    // existing save comes back muted, which reads as the toggle being broken.
    dr::World world;
    dr::world_init(world, 9);
    world.best_m = 1234;
    dr::SaveData data;
    dr::world_make_save(world, data);
    CHECK(data.sound_off == 0);

    // And an old record, byte for byte: magic and best where they were, zeros
    // in the tail. It has to load, and it has to load with the sound ON.
    dr::SaveData old{};
    old.magic = dr::k_save_magic;
    old.best_m = 777;
    dr::World other;
    dr::world_init(other, 10);
    CHECK(dr::world_load(other, old));
    CHECK(other.best_m == 777);
    // THE INTERPRETATION, not just the byte. Reading that field as "sound on"
    // rather than "sound off" is one character in one line, it compiles, and
    // its whole effect is that every save anyone already has comes back muted.
    CHECK(dr::save_sound_on(old));
    CHECK(dr::save_sound_on(data));

    dr::SaveData muted = data;
    muted.sound_off = 1;
    CHECK(!dr::save_sound_on(muted));
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
    test_offroad_is_bounded();
    test_cactus_kills_off_road_only();
    test_rail_kills_leaving_not_riding();
    test_curve_is_followable();
    test_generation_is_fair();
    test_road_actually_curves();
    test_bot_survives();
    test_determinism();
    test_save_roundtrip();
    test_the_ride_says_what_it_sounds_like();
    test_a_frame_hears_every_tick_in_it();
    test_sound_survives_a_save_written_before_it_existed();
    test_memory_budget();

    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
