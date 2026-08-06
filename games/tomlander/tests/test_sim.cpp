// Host side tests for Tom Lander's sim. Pure integer C++, so the claims the
// flight is balanced on are proven here rather than asserted in comments:
// that one pod cannot hold the ship up and four can, that a pod lifts its own
// corner and the ship then travels away from it, that the leveller converges
// rather than diverges, and that the ground is where every part of the code
// agrees it is.
//
// The sign checks are the point of this file. Both torque signs were inverted
// at one stage of the mockup this game came from, with a comment claiming the
// opposite, and the symptom was a thruster firing on one corner while the
// opposite corner rose. Nothing about that looked wrong in a still frame.

#include <cstdio>

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

void run(tl::World& world, const tl::Input& input, int ticks) {
    for (int i = 0; i < ticks; i++) tl::world_tick(world, input);
}

tl::Input fire(int pod) {
    tl::Input in{};
    in.pod[pod] = true;
    return in;
}

tl::Input fire_all() {
    tl::Input in{};
    for (int i = 0; i < tl::kPodCount; i++) in.pod[i] = true;
    return in;
}

// A world parked high in clear air, so physics can be measured without the
// ground or a pad deck interfering.
void init_airborne(tl::World& world) {
    tl::world_init(world);
    world.y = 90 << 16;
    world.grounded = false;
    world.landed_on = 0xFF;
}

int32_t iabs(int32_t v) { return v < 0 ? -v : v; }

// ---- trigonometry ----

void test_trig_is_a_circle() {
    // sin^2 + cos^2 = 1 everywhere, which catches a table typo, a bad
    // quadrant fold and a wrong interpolation in one check.
    int32_t worst = 0;
    for (int32_t a = -tl::k_turn; a < 2 * tl::k_turn; a += 7) {
        const int32_t s = tl::sin_fp(a);
        const int32_t c = tl::cos_fp(a);
        const int32_t sum = (s * s + c * c) >> 14;
        const int32_t err = iabs(sum - tl::k_trig_one);
        if (err > worst) worst = err;
    }
    CHECK(worst < 40);      // 40/16384 is a quarter of one percent

    CHECK(tl::sin_fp(0) == 0);
    CHECK(tl::sin_fp(tl::k_quarter) == tl::k_trig_one);
    CHECK(tl::sin_fp(2 * tl::k_quarter) == 0);
    CHECK(tl::sin_fp(3 * tl::k_quarter) == -tl::k_trig_one);
    CHECK(tl::cos_fp(0) == tl::k_trig_one);
    // Negative angles must fold, not clamp: the hull rolls both ways.
    CHECK(tl::sin_fp(-tl::k_quarter) == -tl::k_trig_one);
}

// ---- thrust to weight ----

// The number the whole flight model hangs off. One pod is 0.70 and cannot
// hold the ship up; four are 2.80 and climb briskly. Both are true in
// tom-lander and both have to stay true here, so this measures them rather
// than trusting the constants.
void test_one_pod_cannot_hover_and_four_can() {
    CHECK(tl::k_pod_thrust * 4 > tl::k_gravity);      // four can lift
    CHECK(tl::k_pod_thrust < tl::k_gravity);          // one cannot

    tl::World world;
    init_airborne(world);
    const int32_t start = world.y;
    run(world, fire(tl::kPodRight), 200);
    CHECK(world.y < start);                            // one pod: still falling

    init_airborne(world);
    const int32_t start4 = world.y;
    run(world, fire_all(), 200);
    CHECK(world.y > start4);                           // four pods: climbing
}

void test_terminal_fall_matches_the_tuning() {
    tl::World world;
    init_airborne(world);
    world.y = 140 << 16;
    tl::Input none{};
    run(world, none, 600);
    // v_terminal = gravity * drag_mul, within the rounding of the shift.
    const int32_t expect = tl::k_gravity * tl::k_drag_mul;
    CHECK(iabs(tl::descent(world) - expect) < expect / 8);
}

// ---- the signs, which is what this file is really for ----

// Firing one pod must lift THAT pod's corner, and the ship must then travel
// AWAY from it. Four cases, and every one of them was wrong at some point.
void test_a_pod_lifts_its_own_corner() {
    struct Case {
        int pod;
        int expect_pitch;   // sign, 0 for none
        int expect_roll;
        int expect_vx;
        int expect_vz;
    };
    // Positive pitch is nose UP and positive roll is right side up, the same
    // convention draw_mesh draws with. The velocity column is what did NOT
    // change when that convention was fixed: the net force was always right,
    // and only the sign the hull was drawn at was wrong.
    const Case cases[] = {
        {tl::kPodFront, +1,  0,  0, -1},   // nose up, slides back
        {tl::kPodBack,  -1,  0,  0, +1},   // nose down, slides forward
        {tl::kPodRight,  0, +1, -1,  0},   // right side up, slides left
        {tl::kPodLeft,   0, -1, +1,  0},   // left side up, slides right
    };

    for (const Case& c : cases) {
        tl::World world;
        init_airborne(world);
        run(world, fire(c.pod), 150);

        if (c.expect_pitch > 0) CHECK(world.pitch > 0);
        if (c.expect_pitch < 0) CHECK(world.pitch < 0);
        if (c.expect_pitch == 0) CHECK(world.pitch == 0);
        if (c.expect_roll > 0) CHECK(world.roll > 0);
        if (c.expect_roll < 0) CHECK(world.roll < 0);
        if (c.expect_roll == 0) CHECK(world.roll == 0);

        if (c.expect_vx > 0) CHECK(world.vx > 0);
        if (c.expect_vx < 0) CHECK(world.vx < 0);
        if (c.expect_vx == 0) CHECK(world.vx == 0);
        if (c.expect_vz > 0) CHECK(world.vz > 0);
        if (c.expect_vz < 0) CHECK(world.vz < 0);
        if (c.expect_vz == 0) CHECK(world.vz == 0);
    }
}

// Opposite pods must cancel exactly, which is the other half of the same
// claim: if the signs were right for one pod but the arms disagreed, this is
// where it would show.
void test_opposite_pods_cancel() {
    tl::World world;
    init_airborne(world);
    tl::Input sides{};
    sides.pod[tl::kPodLeft] = sides.pod[tl::kPodRight] = true;
    run(world, sides, 200);
    CHECK(world.roll == 0);
    CHECK(world.vx == 0);

    init_airborne(world);
    tl::Input ends{};
    ends.pod[tl::kPodFront] = ends.pod[tl::kPodBack] = true;
    run(world, ends, 200);
    CHECK(world.pitch == 0);
    CHECK(world.vz == 0);
}

// The hull's up vector has to agree with the angles, because that is the one
// place the renderer and the sim have to mean the same thing.
void test_up_vector_follows_the_hull() {
    tl::World world;
    tl::world_init(world);
    int32_t ux, uy, uz;

    tl::hull_up(world, ux, uy, uz);
    CHECK(ux == 0 && uz == 0 && uy == tl::k_trig_one);   // level

    world.pitch = (tl::k_turn / 8) << 8;                 // 45 deg nose UP
    tl::hull_up(world, ux, uy, uz);
    CHECK(uz < 0);                                       // lift leans backward

    world.pitch = 0;
    world.roll = (tl::k_turn / 8) << 8;
    tl::hull_up(world, ux, uy, uz);
    CHECK(ux < 0);                                       // lift leans left
}

// ---- the leveller ----

// The down button has to CONVERGE from any attitude. Its correction fires the
// pod that opposes the tilt, so if the torque signs were flipped without it
// moving too, it becomes positive feedback and drives the hull over. That is
// a real failure mode this project has already produced once.
void test_level_converges_from_every_attitude() {
    const int32_t big = (tl::k_turn * 50 / 360) << 8;    // 50 degrees
    const struct { int32_t pitch, roll; } starts[] = {
        {big, 0}, {-big, 0}, {0, big}, {0, -big}, {big, big}, {-big, -big},
    };
    tl::Input level{};
    level.level = true;

    for (const auto& s : starts) {
        tl::World world;
        init_airborne(world);
        world.pitch = s.pitch;
        world.roll = s.roll;
        const int32_t before = tl::tilt(world);
        run(world, level, 300);
        CHECK(tl::tilt(world) < before / 4);
        CHECK(world.state == tl::Flight::Flying);        // never tumbles
    }
}

// ---- the ground ----

void test_the_ship_starts_parked_on_pad_a() {
    tl::World world;
    tl::world_init(world);
    CHECK(world.grounded);
    CHECK(world.landed_on == 0);
    CHECK(world.target == 1);
    CHECK(tl::altitude(world) == 0);
    CHECK(world.x == world.pads[0].x && world.z == world.pads[0].z);
    // And it is resting on the DECK, not the apron: the deck stands proud.
    CHECK(world.y > world.pads[0].y + tl::k_pad_rise);
}

void test_the_apron_is_flat_and_the_hills_are_not() {
    tl::World world;
    tl::world_init(world);
    const tl::Pad& pad = world.pads[1];

    // Everywhere inside the apron is exactly the pad's own height, so an
    // approach is never spoiled by a slope nobody could have seen.
    for (int32_t dx = -tl::k_pad_flat; dx <= tl::k_pad_flat;
         dx += tl::k_pad_flat / 4) {
        for (int32_t dz = -tl::k_pad_flat; dz <= tl::k_pad_flat;
             dz += tl::k_pad_flat / 4) {
            CHECK(tl::terrain_height(world, pad.x + dx, pad.z + dz) == pad.y);
        }
    }

    // And well outside it, the landscape is doing something.
    int32_t lo = 1 << 30, hi = -(1 << 30);
    for (int32_t i = 0; i < 64; i++) {
        const int32_t h = tl::terrain_height(world, (i * 7) << 16, (i * 11) << 16);
        if (h < lo) lo = h;
        if (h > hi) hi = h;
    }
    CHECK(hi - lo > (4 << 16));
}

void test_ground_at_agrees_with_the_deck() {
    tl::World world;
    tl::world_init(world);
    const tl::Pad& pad = world.pads[0];

    CHECK(tl::pad_at(world, pad.x, pad.z) == 0);
    CHECK(tl::pad_at(world, pad.x + tl::k_pad_half + 65536, pad.z) < 0);

    // Standing on the deck is exactly k_pad_rise above standing beside it.
    const int32_t on = tl::ground_at(world, pad.x, pad.z);
    const int32_t off = tl::ground_at(world, pad.x + tl::k_pad_half + (2 << 16),
                                      pad.z);
    CHECK(on - off == tl::k_pad_rise);
}

// ---- landing ----

void test_a_gentle_touchdown_on_the_target_wins() {
    tl::World world;
    tl::world_init(world);
    // Start hovering just over pad B and settle onto it.
    world.x = world.pads[1].x;
    world.z = world.pads[1].z;
    world.y = tl::ground_at(world, world.x, world.z) + (6 << 16);
    world.grounded = false;

    tl::Input hold{};
    hold.level = true;
    for (int i = 0; i < 400 && world.state == tl::Flight::Flying; i++) {
        // Ease off so it drifts down rather than hovering forever.
        hold.level = tl::descent(world) > tl::k_safe_descent / 2;
        tl::world_tick(world, hold);
    }
    CHECK(world.state == tl::Flight::Landed);
    CHECK(world.landed_on == 1);
}

void test_arriving_too_fast_is_a_crash() {
    tl::World world;
    tl::world_init(world);
    world.x = world.pads[1].x;
    world.z = world.pads[1].z;
    world.y = tl::ground_at(world, world.x, world.z) + (60 << 16);
    world.grounded = false;
    tl::Input none{};
    run(world, none, 900);
    CHECK(world.state == tl::Flight::Crashed);
    CHECK(world.fault == tl::Fault::TooFast);
}

void test_arriving_too_steep_is_a_crash() {
    tl::World world;
    tl::world_init(world);
    world.x = world.pads[1].x;
    world.z = world.pads[1].z;
    world.y = tl::ground_at(world, world.x, world.z) + (1 << 16);
    world.roll = tl::k_safe_tilt * 2;
    world.grounded = false;
    tl::Input none{};
    run(world, none, 200);
    CHECK(world.state == tl::Flight::Crashed);
    CHECK(world.fault == tl::Fault::TooSteep);
}

// Landing well anywhere that is NOT the target just parks the ship, and it
// can take off again. That is what makes mission one a hop rather than a
// single descent.
void test_landing_off_target_parks_rather_than_ends() {
    tl::World world;
    tl::world_init(world);
    CHECK(world.state == tl::Flight::Flying);
    CHECK(world.grounded);

    run(world, fire_all(), 120);
    CHECK(!world.grounded);
    CHECK(tl::altitude(world) > 0);
    CHECK(world.state == tl::Flight::Flying);
}

// ---- fuel ----

void test_fuel_burns_with_throttle_and_runs_out() {
    tl::World world;
    init_airborne(world);
    const int32_t full = world.fuel;
    run(world, fire(tl::kPodRight), 100);
    const int32_t one_pod = full - world.fuel;

    init_airborne(world);
    run(world, fire_all(), 100);
    const int32_t four_pods = full - world.fuel;
    CHECK(four_pods > one_pod);

    // A dry tank stops the pods, so the ship falls no matter what is held.
    init_airborne(world);
    world.fuel = 0;
    run(world, fire_all(), 60);
    CHECK(world.throttle[0] == 0);
    CHECK(tl::descent(world) > 0);
}

// ---- housekeeping ----

void test_a_flight_is_deterministic() {
    tl::World a, b;
    tl::world_init(a);
    tl::world_init(b);
    for (int i = 0; i < 500; i++) {
        tl::Input in{};
        in.pod[i % tl::kPodCount] = (i % 7) < 3;
        in.level = (i % 23) == 0;
        tl::world_tick(a, in);
        tl::world_tick(b, in);
    }
    CHECK(a.x == b.x && a.y == b.y && a.z == b.z);
    CHECK(a.pitch == b.pitch && a.roll == b.roll);
    CHECK(a.fuel == b.fuel);
}

void test_memory_budget() {
    // The sim is the cheap part and has to stay that way: the rasterizer's
    // depth buffer and the frame queue are what the RAM actually goes on.
    CHECK(sizeof(tl::World) <= 256);
}

}  // namespace

int main() {
    test_trig_is_a_circle();
    test_one_pod_cannot_hover_and_four_can();
    test_terminal_fall_matches_the_tuning();
    test_a_pod_lifts_its_own_corner();
    test_opposite_pods_cancel();
    test_up_vector_follows_the_hull();
    test_level_converges_from_every_attitude();
    test_the_ship_starts_parked_on_pad_a();
    test_the_apron_is_flat_and_the_hills_are_not();
    test_ground_at_agrees_with_the_deck();
    test_a_gentle_touchdown_on_the_target_wins();
    test_arriving_too_fast_is_a_crash();
    test_arriving_too_steep_is_a_crash();
    test_landing_off_target_parks_rather_than_ends();
    test_fuel_burns_with_throttle_and_runs_out();
    test_a_flight_is_deterministic();
    test_memory_budget();

    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
