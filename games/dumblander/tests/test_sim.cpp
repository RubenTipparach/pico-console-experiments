// Host side tests for Dumb Lander's rules.
//
// The sim is pure integer C++ with no SDK and no renderer, so the promises it
// makes are proven here rather than asserted in a comment. The interesting
// ones are the two the cart got wrong and the one the mockup found by flying:
// the whole footprint has to be on the deck, the opening frame is not a crash,
// and the tank has to last a crossing.
//
// No test framework. A failure prints the expression and the process exits
// non zero.

#include <cstdint>
#include <cstdio>

#include "sim.hpp"

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

dl::Input none() { return dl::Input{false, false, false, false}; }

dl::World launch(uint32_t seed) {
    dl::World world;
    dl::world_init(world, seed);
    dl::Input start = none();
    start.any_pressed = true;
    dl::world_tick(world, start);
    return world;
}

// The bench autopilot from mockups/dumblander, in C++ so the same flight can
// be flown here. It is a test fixture and not part of the game: nothing in
// src/ knows it exists.
struct Autopilot {
    bool descending = false;

    dl::Input operator()(const dl::World& w) {
        dl::Input in = none();
        const int32_t target_x = (w.goal.x + w.goal.w / 2) * dl::k_one;
        const int32_t dx = target_x - w.x;

        // Clearance is measured over the highest ground between here and the
        // deck, not over the decks themselves: the ridge between them is what
        // you hit. Flattening a pad also leaves a step at its edge, so a
        // controller that slides sideways while still climbing flies into it.
        int32_t peak = dl::k_screen_h << 8;
        const int from = (w.x >> dl::k_fp) < (target_x >> dl::k_fp)
                             ? (w.x >> dl::k_fp) : (target_x >> dl::k_fp);
        const int to = (w.x >> dl::k_fp) < (target_x >> dl::k_fp)
                           ? (target_x >> dl::k_fp) : (w.x >> dl::k_fp);
        for (int x = from; x <= to; x++) {
            const int32_t h = dl::ground_at(w, x);
            if (h < peak) peak = h;
        }
        const int32_t cruise = (peak << 8) - 26 * dl::k_one;

        const int32_t adx = dx < 0 ? -dx : dx;
        if (!descending && adx < 4 * dl::k_one &&
            (w.vx < dl::k_one / 7 && w.vx > -dl::k_one / 7)) {
            descending = true;
        }
        if (descending && adx > 14 * dl::k_one) descending = false;

        // Aim BELOW the deck, not at it. Aimed at it the controller holds
        // station two pixels up and hovers until the tank is empty, which is
        // how the mockup first reported that the tank was too small.
        const int32_t want = descending ? (w.goal.y << 8) + 8 * dl::k_one : cruise;
        const int32_t err_y = w.y - want;
        int32_t desired_vy = -(err_y / 8);
        const int32_t vy_cap = (dl::k_safe * 72) / 100;
        if (desired_vy > vy_cap) desired_vy = vy_cap;
        if (desired_vy < -(dl::k_one + dl::k_one / 5)) desired_vy = -(dl::k_one + dl::k_one / 5);
        in.thrust = w.vy > desired_vy;

        const bool clear = w.y < cruise + 10 * dl::k_one;
        const int32_t cap = descending ? dl::k_one / 4 : (clear ? (dl::k_one * 4) / 5 : 0);
        int32_t desired_vx = dx / 16;
        if (desired_vx > cap) desired_vx = cap;
        if (desired_vx < -cap) desired_vx = -cap;
        if (w.vx > desired_vx + dl::k_one / 80) in.left = true;
        else if (w.vx < desired_vx - dl::k_one / 80) in.right = true;
        return in;
    }
};

void test_a_run_starts_only_on_a_press() {
    dl::World world;
    dl::world_init(world, 7);
    CHECK(world.state == dl::State::title);
    for (int i = 0; i < 50; i++) dl::world_tick(world, none());
    CHECK(world.state == dl::State::title);

    dl::Input start = none();
    start.any_pressed = true;
    dl::world_tick(world, start);
    CHECK(world.state == dl::State::fly);
    CHECK(world.leg == 1);
    CHECK(world.fuel == dl::k_tank);
}

void test_sitting_on_the_pad_is_not_a_crash() {
    // The cart's take off latch. Without it the first tick reports contact
    // with the deck the lander is standing on and the run ends before it
    // starts, which is a real way this has gone wrong.
    dl::World world = launch(3);
    for (int i = 0; i < 200; i++) dl::world_tick(world, none());
    CHECK(world.state == dl::State::fly);
    CHECK(world.ending == dl::Ending::none);
    CHECK(!world.took_off);
}

void test_thrust_is_two_gravities_and_only_it_burns_fuel() {
    dl::World world = launch(3);
    const int32_t fuel_before = world.fuel;

    dl::Input side = none();
    side.left = true;
    for (int i = 0; i < 100; i++) dl::world_tick(world, side);
    CHECK(world.fuel == fuel_before);           // the jets are free, as the cart had them

    dl::World climb = launch(3);
    dl::Input up = none();
    up.thrust = true;
    for (int i = 0; i < 100; i++) dl::world_tick(climb, up);
    CHECK(climb.vy < 0);                        // it climbs, so thrust beats gravity
    CHECK(climb.fuel < fuel_before);
    // 100 ticks at the burn rate, to the fixed point rounding.
    const int32_t burned = fuel_before - climb.fuel;
    CHECK(burned >= dl::k_burn * 99 && burned <= dl::k_burn * 101);
    // Two gravities up means one gravity of net climb.
    CHECK(dl::k_thrust == -2 * dl::k_gravity);
}

void test_the_whole_footprint_has_to_be_on_the_deck() {
    // The cart tested the hull's centre against the pad's left half, so the
    // right half of every deck did not count. Both edges are tested here.
    dl::World world = launch(11);
    const int centre = world.goal.x + world.goal.w / 2;

    world.x = centre * dl::k_one;
    CHECK(dl::on_goal(world));

    // A hull hanging off the left edge by one pixel is not on the deck.
    world.x = (world.goal.x + dl::k_hull_w / 2 - 1) * dl::k_one;
    CHECK(!dl::on_goal(world));
    world.x = (world.goal.x + dl::k_hull_w / 2) * dl::k_one;
    CHECK(dl::on_goal(world));

    // And the same at the right edge, which is the half the cart lost.
    world.x = (world.goal.x + world.goal.w - dl::k_hull_w / 2) * dl::k_one;
    CHECK(dl::on_goal(world));
    world.x = (world.goal.x + world.goal.w - dl::k_hull_w / 2 + 1) * dl::k_one;
    CHECK(!dl::on_goal(world));
}

void test_a_fast_arrival_wrecks_and_a_slow_one_does_not() {
    dl::World fast = launch(5);
    fast.took_off = true;
    fast.x = (fast.goal.x + fast.goal.w / 2) * dl::k_one;
    fast.y = (static_cast<int32_t>(fast.goal.y) << 8) - 60 * dl::k_one;
    fast.vy = dl::k_safe * 3;
    for (int i = 0; i < 400 && fast.state == dl::State::fly; i++) {
        dl::world_tick(fast, none());
    }
    CHECK(fast.state == dl::State::over);
    CHECK(fast.ending == dl::Ending::crashed);

    dl::World slow = launch(5);
    slow.took_off = true;
    slow.x = (slow.goal.x + slow.goal.w / 2) * dl::k_one;
    slow.y = (static_cast<int32_t>(slow.goal.y) << 8) - 2 * dl::k_one;
    slow.vy = dl::k_safe / 2;
    for (int i = 0; i < 40 && slow.state == dl::State::fly; i++) {
        dl::world_tick(slow, none());
    }
    CHECK(slow.state == dl::State::landed);
}

void test_a_leg_regenerates_and_refuels() {
    dl::World world = launch(9);
    world.took_off = true;
    world.fuel = dl::k_tank / 4;
    world.x = (world.goal.x + world.goal.w / 2) * dl::k_one;
    world.y = (static_cast<int32_t>(world.goal.y) << 8) - 2 * dl::k_one;
    world.vy = dl::k_safe / 2;
    for (int i = 0; i < 40 && world.state == dl::State::fly; i++) {
        dl::world_tick(world, none());
    }
    CHECK(world.state == dl::State::landed);

    const int16_t old_goal_w = world.goal.w;
    for (int i = 0; i < dl::k_landed_hold + 2; i++) dl::world_tick(world, none());
    CHECK(world.state == dl::State::fly);
    CHECK(world.leg == 2);
    CHECK(world.fuel == dl::k_tank / 4 + dl::k_refuel);
    CHECK(world.goal.w <= old_goal_w);          // the deck narrows, never widens
    CHECK(!world.took_off);                     // and the latch is armed again
}

void test_the_tank_is_capped_and_the_deck_stops_narrowing() {
    dl::World world = launch(4);
    // Far enough in that the difficulty ramp has run out of steps.
    for (int leg = 0; leg < 9; leg++) {
        world.leg = static_cast<uint16_t>(leg + 1);
        world.fuel = dl::k_tank;
        world.took_off = true;
        world.x = (world.goal.x + world.goal.w / 2) * dl::k_one;
        world.y = (static_cast<int32_t>(world.goal.y) << 8) - 2 * dl::k_one;
        world.vy = dl::k_safe / 2;
        for (int i = 0; i < 40 && world.state == dl::State::fly; i++) {
            dl::world_tick(world, none());
        }
        CHECK(world.state == dl::State::landed);
        for (int i = 0; i < dl::k_landed_hold + 2; i++) dl::world_tick(world, none());
        CHECK(world.fuel <= dl::k_tank);        // a full tank plus a top up is still full
        CHECK(world.goal.w >= 18);              // and the deck never shrinks to nothing
        CHECK(world.goal.x + world.goal.w <= dl::k_screen_w - 4);
    }
}

void test_dry_and_resting_off_the_deck_ends_the_run() {
    dl::World world = launch(21);
    world.took_off = true;
    world.fuel = 0;
    // Put it over ground that is not either deck.
    world.x = 118 * dl::k_one;
    world.y = (static_cast<int32_t>(dl::ground_at(world, 118)) << 8) - 4 * dl::k_one;
    world.vy = dl::k_safe / 4;
    for (int i = 0; i < 400 && world.state == dl::State::fly; i++) {
        dl::world_tick(world, none());
    }
    CHECK(world.state == dl::State::over);
    CHECK(world.ending == dl::Ending::stranded);
}

void test_the_ceiling_holds_it_on_screen() {
    dl::World world = launch(2);
    dl::Input up = none();
    up.thrust = true;
    for (int i = 0; i < 600; i++) dl::world_tick(world, up);
    CHECK((world.y >> dl::k_fp) >= dl::k_ceiling - 1);
}

// The measurement the mockup made, kept here so it cannot quietly stop being
// true. The cart's drain rate landed 3 of 6 legs with an empty tank every
// time; this rate has to land every one of them with fuel in hand.
void test_the_tank_lasts_a_crossing() {
    int landed = 0;
    int32_t worst_fuel = dl::k_tank;
    for (int seed = 1; seed <= 24; seed++) {
        dl::World world = launch(static_cast<uint32_t>(seed * 37));
        Autopilot pilot;
        for (int i = 0; i < 4000 && world.state == dl::State::fly; i++) {
            dl::world_tick(world, pilot(world));
        }
        if (world.state == dl::State::landed) {
            landed++;
            if (world.fuel < worst_fuel) worst_fuel = world.fuel;
        }
    }
    std::printf("autopilot landed %d of 24, worst fuel left %d%%\n", landed,
                static_cast<int>(worst_fuel / (dl::k_tank / 100)));
    CHECK(landed == 24);
    // Every clean leg leaves real margin. An autopilot that only just makes it
    // is a game a person cannot fly.
    CHECK(worst_fuel > dl::k_tank / 4);
}

void test_terrain_stays_on_screen_and_pads_are_flat() {
    for (int seed = 1; seed <= 40; seed++) {
        dl::World world;
        dl::world_init(world, static_cast<uint32_t>(seed * 13));
        for (int x = 0; x < dl::k_screen_w; x++) {
            const int h = world.terrain[x] >> 8;
            CHECK(h >= 0 && h < dl::k_screen_h);
        }
        const dl::Pad* pads[2] = {&world.start, &world.goal};
        for (const dl::Pad* pad : pads) {
            CHECK(pad->x >= 0);
            CHECK(pad->x + pad->w <= dl::k_screen_w);
            for (int i = pad->x; i < pad->x + pad->w; i++) {
                CHECK(world.terrain[i] == pad->y);
            }
        }
        // The decks must not overlap, or one landing is both landings.
        CHECK(world.start.x + world.start.w < world.goal.x);
    }
}

}  // namespace

int main() {
    test_a_run_starts_only_on_a_press();
    test_sitting_on_the_pad_is_not_a_crash();
    test_thrust_is_two_gravities_and_only_it_burns_fuel();
    test_the_whole_footprint_has_to_be_on_the_deck();
    test_a_fast_arrival_wrecks_and_a_slow_one_does_not();
    test_a_leg_regenerates_and_refuels();
    test_the_tank_is_capped_and_the_deck_stops_narrowing();
    test_dry_and_resting_off_the_deck_ends_the_run();
    test_the_ceiling_holds_it_on_screen();
    test_the_tank_lasts_a_crossing();
    test_terrain_stays_on_screen_and_pads_are_flat();

    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
