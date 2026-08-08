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

#include "menu.hpp"
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

// Attitudes here are built with pse::quat_from_axis_angle, which takes
// radians. The sim itself never sees one: it integrates body rates and has no
// angle in it anywhere.
constexpr float k_pi = 3.14159265f;

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
        int expect_ux;      // sign of the hull's up vector, 0 for none
        int expect_uz;
        int expect_vx;
        int expect_vz;
    };
    // Stated as the hull's UP VECTOR rather than as a pair of angles, because
    // the sim no longer holds angles and because the up vector is the thing
    // that stays meaningful at every attitude. Lift a corner and the up vector
    // leans AWAY from it, which is also why the hull travels away from a
    // firing pod.
    //
    // The velocity column has never changed through any of this, including the
    // move off Euler angles: the net force was always right, and what was
    // wrong was only ever the attitude it was drawn at.
    const Case cases[] = {
        {tl::kPodFront,  0, -1,  0, -1},   // nose up, slides back
        {tl::kPodBack,   0, +1,  0, +1},   // nose down, slides forward
        {tl::kPodRight, -1,  0, -1,  0},   // right side up, slides left
        {tl::kPodLeft,  +1,  0, +1,  0},   // left side up, slides right
    };

    for (const Case& c : cases) {
        tl::World world;
        init_airborne(world);
        run(world, fire(c.pod), 150);

        int32_t ux, uy, uz;
        tl::hull_up(world, ux, uy, uz);
        if (c.expect_ux > 0) CHECK(ux > 0);
        if (c.expect_ux < 0) CHECK(ux < 0);
        if (c.expect_ux == 0) CHECK(ux == 0);
        if (c.expect_uz > 0) CHECK(uz > 0);
        if (c.expect_uz < 0) CHECK(uz < 0);
        if (c.expect_uz == 0) CHECK(uz == 0);

        if (c.expect_vx > 0) CHECK(world.vx > 0);
        if (c.expect_vx < 0) CHECK(world.vx < 0);
        if (c.expect_vx == 0) CHECK(world.vx == 0);
        if (c.expect_vz > 0) CHECK(world.vz > 0);
        if (c.expect_vz < 0) CHECK(world.vz < 0);
        if (c.expect_vz == 0) CHECK(world.vz == 0);
    }
}

// No pod can yaw the hull, at any attitude, ever. Every pod thrusts along the
// hull's own centre line, and a force through the centre line has no moment
// about it. Worth pinning rather than assuming: it is the reason this game
// needs no yaw control, and if a future arm ever gained a y component the
// silence would be the only warning.
void test_no_pod_can_yaw_the_hull() {
    for (int pod = 0; pod < tl::kPodCount; pod++) {
        tl::World world;
        init_airborne(world);
        // Start well off level, so the claim is about the hull's own axes and
        // not about the one attitude where every frame agrees.
        world.q = pse::quat_from_axis_angle(0.4f, 0.0f, 1.0f, 1.1f);
        run(world, fire(pod), 200);
        CHECK(world.wy == 0);
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
    CHECK(world.wz == 0);
    CHECK(tl::tilt(world) == 0);
    CHECK(world.vx == 0);

    init_airborne(world);
    tl::Input ends{};
    ends.pod[tl::kPodFront] = ends.pod[tl::kPodBack] = true;
    run(world, ends, 200);
    CHECK(world.wx == 0);
    CHECK(tl::tilt(world) == 0);
    CHECK(world.vz == 0);
}

// The hull's up vector has to agree with the attitude, because that is the one
// place the renderer and the sim have to mean the same thing.
void test_up_vector_follows_the_hull() {
    tl::World world;
    tl::world_init(world);
    int32_t ux, uy, uz;

    tl::hull_up(world, ux, uy, uz);
    CHECK(ux == 0 && uz == 0 && uy == pse::k_quat_one);  // level

    // Nose up is a NEGATIVE turn about the hull's own x: a positive one takes
    // its up toward +z, which is the nose going down.
    world.q = pse::quat_from_axis_angle(1.0f, 0.0f, 0.0f, -k_pi / 4);
    tl::hull_up(world, ux, uy, uz);
    CHECK(uz < 0);                                       // lift leans backward

    // Right side up is a POSITIVE turn about the hull's own z.
    world.q = pse::quat_from_axis_angle(0.0f, 0.0f, 1.0f, k_pi / 4);
    tl::hull_up(world, ux, uy, uz);
    CHECK(ux < 0);                                       // lift leans left
}

// The world's up, read in the hull's own frame, is the levelling error, and
// it has to lean the opposite way to the hull's up. Both are needed and they
// are not the same vector: one says where thrust goes, the other says which
// pod will fix the tilt.
void test_up_in_hull_is_the_mirror_of_hull_up() {
    tl::World world;
    tl::world_init(world);
    int32_t bx, by, bz;

    world.q = pse::quat_from_axis_angle(1.0f, 0.0f, 0.0f, -k_pi / 4);
    tl::up_in_hull(world, bx, by, bz);
    CHECK(bz > 0);                     // nose up: world up leans toward +z

    world.q = pse::quat_from_axis_angle(0.0f, 0.0f, 1.0f, k_pi / 4);
    tl::up_in_hull(world, bx, by, bz);
    CHECK(bx > 0);                     // right side up: it leans toward +x
}

// The claim the whole quaternion change exists to make.
//
// A pod turns the hull about the HULL's axes, at every attitude. The Euler
// pair this replaced could not do that: measured on it, firing the front pod
// at 30 degrees of roll turned the hull about an axis 30 degrees off the
// right one, and the error tracked the roll angle degree for degree out to 90,
// where a pod that should have yawed the hull pitched it instead.
//
// Rolling the hull and then firing the front pod must move the nose along the
// hull's own x axis. Checked by the invariant that names it without any
// trigonometry: a pure turn about the hull's x cannot change where the hull's
// x axis points, at any roll at all.
void test_a_pod_turns_the_hull_about_its_own_axis() {
    const float rolls[] = {0.0f, 0.26f, 0.52f, 1.05f, 1.57f};   // 0 to 90 deg
    for (float roll : rolls) {
        tl::World world;
        init_airborne(world);
        world.q = pse::quat_from_axis_angle(0.0f, 0.0f, 1.0f, roll);

        int32_t ax, ay, az;
        pse::quat_rotate(world.q, pse::k_quat_one, 0, 0, ax, ay, az);

        run(world, fire(tl::kPodFront), 120);

        int32_t bx, by, bz;
        pse::quat_rotate(world.q, pse::k_quat_one, 0, 0, bx, by, bz);

        // Within a degree, in the fp14 the vector is carried in.
        const int32_t tol = 300;
        CHECK(iabs(bx - ax) < tol);
        CHECK(iabs(by - ay) < tol);
        CHECK(iabs(bz - az) < tol);
    }
}

// ---- the leveller ----

// The down button has to CONVERGE from any attitude. Its correction fires the
// pod that opposes the tilt, so if the torque signs were flipped without it
// moving too, it becomes positive feedback and drives the hull over. That is
// a real failure mode this project has already produced once.
void test_level_converges_from_every_attitude() {
    // Axis and angle, so a start can be a tilt about anything rather than
    // about one of two named axes. The last two are attitudes the Euler pair
    // could not have been tested at honestly: a tilt about a diagonal, and
    // one at 80 degrees, where the old model put a pod's torque on an axis
    // most of a quarter turn away from the right one.
    const struct { float ax, ay, az, angle; } starts[] = {
        {1, 0, 0,  0.87f}, {1, 0, 0, -0.87f},        // 50 degrees, nose
        {0, 0, 1,  0.87f}, {0, 0, 1, -0.87f},        // 50 degrees, side
        {1, 0, 1,  0.87f}, {1, 0, -1, -0.87f},       // 50 degrees, diagonal
        {0.5f, 0, 1, 1.40f},                         // 80 degrees, nearly over
    };
    tl::Input level{};
    level.level = true;

    for (const auto& s : starts) {
        tl::World world;
        init_airborne(world);
        world.q = pse::quat_from_axis_angle(s.ax, s.ay, s.az, s.angle);
        const int32_t before = tl::tilt(world);
        run(world, level, 300);
        // tilt is 1 - cos, which is quadratic in the angle near level, so a
        // sixteenth of it is a quarter of the angle. Stated in the measure the
        // gates use rather than converted, so the number here is the number
        // the sim compares.
        CHECK(tl::tilt(world) < before / 16);
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
    // Well past the 20 degree gate, as an attitude rather than an angle.
    world.q = pse::quat_from_axis_angle(0.0f, 0.0f, 1.0f, 0.7f);
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
    CHECK(a.q.w == b.q.w && a.q.x == b.q.x);
    CHECK(a.q.y == b.q.y && a.q.z == b.q.z);
    CHECK(a.fuel == b.fuel);
}


// ---- mission two, the delivery ----

namespace {

// Put the ship a unit above a deck and let it settle onto it. A unit of fall
// reaches about 7000 fp16 per tick, comfortably inside k_safe_descent, so this
// is a good landing and not a lucky one.
void land_on(tl::World& world, int pad) {
    world.x = world.pads[pad].x;
    world.z = world.pads[pad].z;
    world.y = tl::ground_at(world, world.x, world.z) + (1 << 16);
    world.vx = world.vy = world.vz = 0;
    world.grounded = false;
    tl::Input none{};
    for (int i = 0; i < 200 && !world.grounded &&
                    world.state == tl::Flight::Flying; i++) {
        tl::world_tick(world, none);
    }
}

}  // namespace

void test_mission_two_starts_with_the_crate_on_the_middle_deck() {
    tl::World world;
    tl::world_init(world, tl::Mission::Delivery);
    CHECK(world.mission == tl::Mission::Delivery);
    CHECK(world.cargo == 1);
    CHECK(world.target == 1);            // fly to the crate first
    CHECK(!tl::carrying(world));
    CHECK(world.landed_on == 0);         // parked on A, same as mission one

    // Mission one must not grow a crate.
    tl::World hop;
    tl::world_init(hop, tl::Mission::Hop);
    CHECK(hop.cargo == tl::kCargoNone);
    CHECK(!tl::carrying(hop));
}

// The pickup is a LEG, not an ending: setting down on the crate's deck loads
// it, re-aims at the next one, and the flight carries on.
void test_landing_on_the_crate_loads_it_and_flies_on() {
    tl::World world;
    tl::world_init(world, tl::Mission::Delivery);
    land_on(world, 1);

    CHECK(world.state == tl::Flight::Flying);   // NOT over
    CHECK(tl::carrying(world));
    CHECK(world.target == 2);                   // now aimed at C
    CHECK(world.grounded);                      // parked, ready to lift off
}

void test_delivering_to_the_third_deck_ends_the_flight() {
    tl::World world;
    tl::world_init(world, tl::Mission::Delivery);
    land_on(world, 1);
    CHECK(tl::carrying(world));
    land_on(world, 2);

    CHECK(world.state == tl::Flight::Landed);
    CHECK(world.cargo == tl::kCargoDone);
    CHECK(!tl::carrying(world));
    CHECK(world.landed_on == 2);
}

// Putting the crate's own deck down as the target twice must not re-load it,
// which is what would happen if the pickup checked "am I on a pad" rather than
// "is the crate on THIS pad".
void test_the_crate_is_only_picked_up_once() {
    tl::World world;
    tl::world_init(world, tl::Mission::Delivery);
    land_on(world, 1);
    const uint8_t target_after = world.target;
    land_on(world, 1);                          // sit back down on B
    CHECK(world.target == target_after);        // still aimed at C
    CHECK(tl::carrying(world));
}

// ---- what the crate does to the flight ----

void test_the_crate_is_heavy() {
    // Four pods lift an empty ship faster than a loaded one, and the loaded
    // one still lifts. Same input, same ticks, so the only difference is mass.
    tl::World light, heavy;
    init_airborne(light);
    init_airborne(heavy);
    heavy.cargo = tl::kCargoHeld;

    const int32_t start = light.y;
    run(light, fire_all(), 200);
    run(heavy, fire_all(), 200);

    CHECK(heavy.y > start);                     // loaded still climbs
    CHECK(heavy.y < light.y);                   // but not as well

    // The rule the whole game is built on has to survive the load: one pod
    // cannot hold the ship up, empty or full.
    tl::World one;
    init_airborne(one);
    one.cargo = tl::kCargoHeld;
    const int32_t before = one.y;
    run(one, fire(tl::kPodRight), 200);
    CHECK(one.y < before);
}

void test_the_crate_sways() {
    // Spin the hull up on one pod, then let go. The loaded ship keeps turning
    // for longer, which is the sway: same torque, less damping.
    tl::World light, heavy;
    init_airborne(light);
    init_airborne(heavy);
    heavy.cargo = tl::kCargoHeld;

    run(light, fire(tl::kPodFront), 60);
    run(heavy, fire(tl::kPodFront), 60);
    tl::Input none{};
    run(light, none, 60);
    run(heavy, none, 60);

    CHECK(iabs(heavy.wx) > iabs(light.wx));     // still moving when light is not

    // And it is a settle, not a runaway: it does stop eventually.
    run(heavy, none, 2000);
    CHECK(iabs(heavy.wx) < iabs(light.wx) + 200);
}


// ---- mission three, the ocean salvage ----

void test_the_salvage_starts_ashore_with_the_wreck_at_sea() {
    tl::World w;
    tl::world_init(w, tl::Mission::Salvage);
    CHECK(w.mission == tl::Mission::Salvage);
    CHECK(w.sea == tl::k_sea_level);
    CHECK(w.cargo == 1);            // the section is the load
    CHECK(w.target == 1);           // fly out to it first
    CHECK(w.deliver_to == 0);       // and bring it BACK, not onward

    // The shore is dry and stands above the waterline; the wreck floats.
    CHECK(!tl::over_water(w, w.pads[0].x, w.pads[0].z));
    CHECK(w.pads[0].y > w.sea);
    CHECK(w.pads[1].y == w.sea + tl::k_float_rise);

    // And there is a real crossing, not a puddle. Walk the straight line from
    // the shore to the wreck and count how much of it is open water: the shore
    // apron eats the first third whatever happens, so the check is that a
    // decent share of the leg is genuinely sea.
    int wet = 0;
    for (int i = 0; i <= 20; i++) {
        const int32_t x = w.pads[0].x + (w.pads[1].x - w.pads[0].x) / 20 * i;
        const int32_t z = w.pads[0].z + (w.pads[1].z - w.pads[0].z) / 20 * i;
        if (tl::over_water(w, x, z)) wet++;
    }
    CHECK(wet >= 8);        // at least 40 percent of the crossing is water

    // The other missions must not have grown a sea.
    tl::World dry;
    tl::world_init(dry, tl::Mission::Delivery);
    CHECK(dry.sea == tl::k_no_sea);
    CHECK(!tl::over_water(dry, dry.pads[0].x, dry.pads[0].z));
    CHECK(dry.deliver_to == 2);     // the delivery still goes onward
}

// The sea floor slopes away from the coast, and it has to, because a flat
// datum over three sine waves of relief leaves half of it standing dry: the
// crossing came out as a tidal flat with sandbanks the whole way across and
// the wreck sitting on one of them.
void test_the_sea_floor_falls_away_from_the_coast() {
    tl::World w;
    tl::world_init(w, tl::Mission::Salvage);

    // Seaward is the way the wreck lies. Walk out along it and the floor has
    // to keep going down, never up: reading the depths at the same z takes
    // the terrain's own relief out of the comparison at nothing but the wave
    // along x, which the slope has to beat.
    const int32_t z = w.pads[1].z;
    int32_t shallow = tl::terrain_height(w, w.pads[0].x - (50 << 16), z);
    const int32_t deep = tl::terrain_height(w, w.pads[1].x, z);
    CHECK(deep < shallow);
    CHECK(deep < w.sea - (20 << 16));   // real depth under the wreck, not a puddle

    // At the edge itself nothing has moved yet. Checked against a mission with
    // no ocean at the very same point, which is the untouched landscape: the
    // drop has to start where the shore deck's apron has finished rather than
    // fighting it, or the beach would be pulled out from under the deck.
    tl::World raw;
    tl::world_init(raw, tl::Mission::Hop);
    const int32_t edge = w.pads[0].x - tl::k_shore_edge;
    CHECK(tl::terrain_height(w, edge, w.pads[0].z) ==
          tl::terrain_height(raw, edge, w.pads[0].z));
    CHECK(w.pads[0].y > w.sea);

    // And the land BEHIND the shore is still land. Measuring the drop in every
    // direction rather than seaward would have sunk the hinterland with the
    // sea and left the shore deck standing on an islet.
    int inland_dry = 0;
    for (int i = 1; i <= 6; i++) {
        const int32_t x = w.pads[0].x + (i * 20 << 16);
        if (!tl::over_water(w, x, w.pads[0].z)) inland_dry++;
    }
    CHECK(inland_dry >= 3);

    // Out past the wreck the floor stops falling, so the depth cannot run away
    // to somewhere the height field's own numbers stop meaning anything.
    const int32_t far_out = tl::terrain_height(w, w.pads[1].x - (400 << 16), z);
    CHECK(far_out > w.sea - tl::k_seabed_floor - (20 << 16));

    // None of this touches a mission with no ocean.
    tl::World dry;
    tl::world_init(dry, tl::Mission::Hop);
    tl::World dry2;
    tl::world_init(dry2, tl::Mission::Hop);
    CHECK(tl::terrain_height(dry, -300 << 16, 0) ==
          tl::terrain_height(dry2, -300 << 16, 0));
    CHECK(tl::terrain_height(dry, -300 << 16, 0) > -(60 << 16));
}

// The salvage's landing square has to stay inside the mesh that is drawn for
// it. The section is half the size it first was, and a square left at a built
// deck's k_pad_half would have reached five units clear of a seven unit long
// object: you could put down on open water beside it and score the landing.
void test_the_salvage_square_fits_the_section() {
    tl::World w;
    tl::world_init(w, tl::Mission::Salvage);
    CHECK(w.pads[1].half == tl::k_salvage_half);
    CHECK(w.pads[0].half == tl::k_pad_half);
    CHECK(tl::k_salvage_half < tl::k_pad_half);

    // Dead centre is the deck; a built pad's half width out from it is not.
    CHECK(tl::pad_at(w, w.pads[1].x, w.pads[1].z) == 1);
    CHECK(tl::pad_at(w, w.pads[1].x + tl::k_pad_half, w.pads[1].z) < 0);
    CHECK(tl::pad_at(w, w.pads[1].x, w.pads[1].z + tl::k_pad_half) < 0);
    // And what is not the deck out there is water, which is a lost flight.
    CHECK(tl::over_water(w, w.pads[1].x + tl::k_pad_half, w.pads[1].z));

    // The shore deck keeps the full square, so the mission is only harder
    // where it means to be.
    CHECK(tl::pad_at(w, w.pads[0].x + tl::k_pad_half, w.pads[0].z) == 0);
}

// ---- the descent bands, the hull, and the tank ----

// The number on the HUD is the whole of the landing rule, so the two lines it
// is read against have to sit on exact printed values. The readout prints
// (descent * 100) >> 16, which is whole units per second at a 100 Hz tick and
// it TRUNCATES: with the gate a third of the way up a printed step, a 17 was
// sometimes a landing and sometimes a wreck and nothing at the stick could
// tell them apart.
int32_t hud_number(int32_t fall) { return (fall * 100) >> 16; }

void test_the_bands_land_on_exact_readout_values() {
    // The last descent in each band prints the number the band is named for,
    // and the first descent past it prints the next one up.
    CHECK(hud_number(tl::k_soft_descent) == 10);
    CHECK(hud_number(tl::k_soft_descent + 1) == 11);
    CHECK(hud_number(tl::k_safe_descent) == 16);
    CHECK(hud_number(tl::k_safe_descent + 1) == 17);

    // So every printed value maps to exactly one band, with no straddling.
    // Counted rather than checked one descent at a time: this walks every fp16
    // value up to 20 u/s, and thirteen thousand passing checks would drown the
    // handful that say something about the flight model.
    int straddled = 0;
    for (int32_t fall = 0; fall <= 20 * 656; fall++) {
        const int32_t shown = hud_number(fall);
        const tl::Touchdown band = tl::descent_band(fall);
        const tl::Touchdown want = shown <= 10  ? tl::Touchdown::Clean
                                 : shown <= 16 ? tl::Touchdown::Hard
                                               : tl::Touchdown::Fatal;
        if (band != want) straddled++;
    }
    CHECK(straddled == 0);

    // And the bands are ordered, which the switch that colours them assumes.
    CHECK(tl::k_soft_descent < tl::k_safe_descent);
    CHECK(tl::descent_band(0) == tl::Touchdown::Clean);
}

// Green sets down clean, amber sets down and costs a point of hull, and the
// second amber ends the flight. Flown by dropping onto deck A, so the sim
// judges it through the same path a real touchdown takes.
// Drop onto deck A with a chosen closing speed and report the descent the sim
// actually judged, which is the one on the last tick the ship was still
// flying. Not the speed it was given: gravity and drag both land inside the
// tick before the touchdown is looked at, so a test that assumed the two were
// the same would be testing its own arithmetic.
int32_t drop_onto_pad_a(tl::World& w, int32_t closing, uint8_t damage) {
    tl::world_init(w);
    w.damage = damage;
    w.y = tl::ground_at(w, w.x, w.z) + (2 << 16);
    w.grounded = false;
    w.landed_on = 0xFF;
    w.vy = -closing;
    tl::Input none{};
    int32_t judged = 0;
    for (int i = 0; i < 400 && w.state == tl::Flight::Flying && !w.grounded; i++) {
        judged = tl::descent(w);
        tl::world_tick(w, none);
    }
    return judged;
}

// A drop's recorded descent is the last one visible from outside the sim, and
// the sim then applies one more tick of gravity and drag before it judges. The
// two land within a gravity step of each other, so a drop that finishes within
// a step of a band edge could honestly go either way and says nothing. Skip
// those: the edges themselves are already proven exactly, over every value in
// range, by test_the_bands_land_on_exact_readout_values, and what is left for
// a real landing to prove is that the sim WIRES each band to the right
// consequence.
bool near_a_band_edge(int32_t fall) {
    const int32_t slop = 2 * tl::k_gravity;
    return iabs(fall - tl::k_soft_descent) <= slop ||
           iabs(fall - tl::k_safe_descent) <= slop;
}

void test_a_hard_landing_costs_hull_and_two_of_them_end_it() {
    // Whatever speed each drop works out at, the outcome has to be the one the
    // band function promised for it. That is the contract the HUD colour is
    // sold on, checked against the landing rather than against a constant.
    for (int32_t closing = 0; closing <= 14000; closing += 250) {
        tl::World w;
        const int32_t judged = drop_onto_pad_a(w, closing, 0);
        if (near_a_band_edge(judged)) continue;
        switch (tl::descent_band(judged)) {
            case tl::Touchdown::Clean:
                CHECK(w.state == tl::Flight::Flying);
                CHECK(w.damage == 0);
                break;
            case tl::Touchdown::Hard:
                CHECK(w.state == tl::Flight::Flying);
                CHECK(w.damage == 1);
                break;
            case tl::Touchdown::Fatal:
                CHECK(w.state == tl::Flight::Crashed);
                CHECK(w.fault == tl::Fault::TooFast);
                CHECK(w.damage == 0);       // a wreck is not a dent
                break;
        }
    }

    // A hull with one dent already in it does not survive the next hard one,
    // and the fault says so rather than blaming the speed: the same descent
    // was survivable a moment ago.
    tl::World w;
    const int32_t judged = drop_onto_pad_a(w, tl::k_soft_descent + 2000, 1);
    CHECK(tl::descent_band(judged) == tl::Touchdown::Hard);
    CHECK(w.state == tl::Flight::Crashed);
    CHECK(w.fault == tl::Fault::Broke);
    CHECK(w.damage == tl::k_damage_max);

    // And a red one on a dented hull is still TOO FAST, not BROKE UP: the
    // speed gate is judged first because it is the truer reason.
    tl::World fast;
    CHECK(tl::descent_band(drop_onto_pad_a(fast, 14000, 1)) ==
          tl::Touchdown::Fatal);
    CHECK(fast.fault == tl::Fault::TooFast);

    // A fresh flight starts on a whole hull.
    tl::World fresh;
    tl::world_init(fresh, tl::Mission::Delivery);
    CHECK(fresh.damage == 0);
}

// An empty tank ends a mission, but only after five seconds AND only once the
// hull has stopped moving. Running dry is the end of the control, not the end
// of the flight: the ship still has its speed and its lean and a long way to
// fall, and where it ends up is a real outcome that has to be allowed to
// happen.
void test_running_dry_is_a_glide_before_it_is_a_fail() {
    tl::Input none{};

    // Empty the tank at altitude and the flight carries on. Checked well past
    // the grace period, because the grace alone must not end it: terminal
    // descent is 17.7 units a second and there is a good deal more than five
    // seconds of ceiling above the valley floor.
    tl::World w;
    tl::world_init(w);
    w.y = 120 << 16;
    w.grounded = false;
    w.landed_on = 0xFF;
    w.fuel = 0;
    int airborne = 0;
    while (w.state == tl::Flight::Flying && !w.grounded && airborne < 4000) {
        tl::world_tick(w, none);
        airborne++;
    }
    CHECK(airborne > tl::k_dry_grace);      // it was still flying past the grace
    CHECK(w.dry_ticks > tl::k_dry_grace);

    // Parked with an empty tank: the case that used to hang. It gets the same
    // five seconds and then it is over.
    tl::World parked;
    tl::world_init(parked);
    parked.fuel = 0;
    for (int i = 0; i < tl::k_dry_grace - 1; i++) {
        tl::world_tick(parked, none);
        CHECK(parked.state == tl::Flight::Flying);
    }
    CHECK(tl::at_rest(parked));
    tl::world_tick(parked, none);
    CHECK(parked.state == tl::Flight::Crashed);
    CHECK(parked.fault == tl::Fault::Dry);

    // A tank that is not empty resets the clock, so a leg refuel wipes out a
    // grace period already half spent rather than carrying it forward.
    tl::World topped;
    tl::world_init(topped);
    topped.fuel = 0;
    for (int i = 0; i < 100; i++) tl::world_tick(topped, none);
    CHECK(topped.dry_ticks == 100);
    topped.fuel = tl::k_fuel_full;
    tl::world_tick(topped, none);
    CHECK(topped.dry_ticks == 0);

    // Not once the flight is over, either way. The gauge reading empty on the
    // deck you were sent to is a finished mission, not a failed one, and the
    // dry check is judged after the touchdown for exactly that reason.
    tl::World done;
    tl::world_init(done);
    done.fuel = 0;
    done.state = tl::Flight::Landed;
    for (int i = 0; i < tl::k_dry_grace * 2; i++) tl::world_tick(done, none);
    CHECK(done.state == tl::Flight::Landed);
    CHECK(done.fault == tl::Fault::None);
}

// Where a dead stick glide ends is judged by the touchdown, not by the gauge.
// Three endings, and only one of them is called NO FUEL.
void test_what_a_dead_stick_glide_lands_on_is_what_decides_it() {
    tl::Input none{};

    // Onto the deck it was sent to, gently: that is a landing and it counts.
    // The whole point of the grace, and it would have been unreachable when
    // the tank emptying ended the flight where it stood.
    tl::World winner;
    tl::world_init(winner);
    winner.fuel = 0;
    winner.x = winner.pads[1].x;
    winner.z = winner.pads[1].z;
    winner.y = tl::ground_at(winner, winner.x, winner.z) + (3 << 16);
    winner.grounded = false;
    winner.landed_on = 0xFF;
    for (int i = 0; i < tl::k_dry_grace * 2 &&
                    winner.state == tl::Flight::Flying; i++) {
        tl::world_tick(winner, none);
    }
    CHECK(winner.state == tl::Flight::Landed);
    CHECK(winner.fault == tl::Fault::None);

    // Down in one piece but not on a deck: nothing left to lift off on, and
    // this is the ending that has no other name.
    tl::World stuck;
    tl::world_init(stuck);
    stuck.fuel = 0;
    stuck.x = stuck.pads[0].x + (40 << 16);
    stuck.z = stuck.pads[0].z + (40 << 16);
    stuck.y = tl::ground_at(stuck, stuck.x, stuck.z) + (3 << 16);
    stuck.grounded = false;
    stuck.landed_on = 0xFF;
    for (int i = 0; i < tl::k_dry_grace * 3 &&
                    stuck.state == tl::Flight::Flying; i++) {
        tl::world_tick(stuck, none);
    }
    CHECK(stuck.state == tl::Flight::Crashed);
    CHECK(stuck.fault == tl::Fault::Dry);

    // And straight in from the ceiling: the fault is what actually broke the
    // ship, not what led to it. The player watched the tank empty; they do not
    // need telling, and TOO FAST is the more useful thing to be told.
    tl::World smashed;
    tl::world_init(smashed);
    smashed.fuel = 0;
    smashed.y = 140 << 16;
    smashed.grounded = false;
    smashed.landed_on = 0xFF;
    for (int i = 0; i < 4000 && smashed.state == tl::Flight::Flying; i++) {
        tl::world_tick(smashed, none);
    }
    CHECK(smashed.state == tl::Flight::Crashed);
    CHECK(smashed.fault == tl::Fault::TooFast);
}

// No deck refuels any more. Landing used to fill the tank, which made every
// deck free: arrive on fumes every time and fuel stopped being a decision. The
// fuel is out on the map in crates now, so a touchdown is worth exactly the
// deck it is on and nothing else.
void test_a_deck_no_longer_refuels() {
    tl::World w;
    tl::world_init(w, tl::Mission::Delivery);
    const int32_t low = tl::k_fuel_full / 5;
    w.fuel = low;
    CHECK(w.target == 1);

    // Set down on deck B, where the cargo is: a leg, not an ending.
    w.x = w.pads[1].x;
    w.z = w.pads[1].z;
    w.y = tl::ground_at(w, w.x, w.z);
    w.grounded = false;
    w.landed_on = 0xFF;
    tl::Input none{};
    tl::world_tick(w, none);
    CHECK(w.state == tl::Flight::Flying);   // the flight carries on
    CHECK(tl::carrying(w));
    CHECK(w.target == 2);
    CHECK(w.fuel == low);                   // and it carries on with what it had

    // Deck C ends it, and leaves the tank where the flying left it, which is
    // what keeps the fuel at the finish worth flying for.
    w.fuel = tl::k_fuel_full / 3;
    w.x = w.pads[2].x;
    w.z = w.pads[2].z;
    w.y = tl::ground_at(w, w.x, w.z);
    w.grounded = false;
    w.landed_on = 0xFF;
    tl::world_tick(w, none);
    CHECK(w.state == tl::Flight::Landed);
    CHECK(w.cargo == tl::kCargoDone);
    CHECK(w.fuel == tl::k_fuel_full / 3);
}

// Flying through a green cube is half a tank. The one piece of fuel the player
// can go and get, so everything about reaching it is checked here: that it is
// only collected in the box the reach describes, that it is collected once,
// that it cannot overfill, and that it takes the dry countdown off the hull.
void test_a_fuel_crate_is_half_a_tank_and_is_taken_once() {
    tl::World w;
    tl::world_init(w, tl::Mission::Delivery);
    CHECK(w.crate_count > 0);
    CHECK(w.crates_taken == 0);

    // The first crate that is actually out on the map.
    int idx = -1;
    for (int i = 0; i < w.crate_count; i++) {
        if (w.crates[i].state == tl::Crate::Out) { idx = i; break; }
    }
    CHECK(idx >= 0);
    const tl::FuelCrate at = w.crates[idx];

    // Just outside the reach on one axis is not a pickup. Checked on all
    // three, because a box test that forgot an axis would be a crate you
    // collected by flying over it at any altitude.
    for (int axis = 0; axis < 3; axis++) {
        tl::World miss;
        tl::world_init(miss, tl::Mission::Delivery);
        miss.x = at.x; miss.y = at.y; miss.z = at.z;
        const int32_t off = tl::k_crate_reach + (1 << 16);
        if (axis == 0) miss.x += off;
        if (axis == 1) miss.y += off;
        if (axis == 2) miss.z += off;
        miss.grounded = false;
        miss.landed_on = 0xFF;
        const int32_t before = miss.fuel;
        tl::Input none{};
        tl::world_tick(miss, none);
        CHECK(miss.crates_taken == 0);
        CHECK(miss.fuel <= before);
    }

    // Straight through the middle of it is.
    w.fuel = tl::k_fuel_full / 4;
    w.x = at.x; w.y = at.y; w.z = at.z;
    w.grounded = false;
    w.landed_on = 0xFF;
    w.dry_ticks = 200;
    tl::Input none{};
    tl::world_tick(w, none);
    CHECK(w.crates_taken == 1);
    CHECK(w.crates[idx].state == tl::Crate::Taken);
    // A quarter tank plus half a tank, minus whatever the tick's own drift
    // cost, which is nothing with no pod firing.
    CHECK(w.fuel == tl::k_fuel_full / 4 + tl::k_crate_fuel);
    // Fuel back in the tank is control back, so the countdown starts again.
    CHECK(w.dry_ticks == 0);

    // Sitting in the same place does not collect it twice.
    tl::world_tick(w, none);
    CHECK(w.crates_taken == 1);

    // And a tank does not overfill, which is what makes the order they are
    // taken in worth thinking about.
    tl::World full;
    tl::world_init(full, tl::Mission::Delivery);
    int out = -1;
    for (int i = 0; i < full.crate_count; i++) {
        if (full.crates[i].state == tl::Crate::Out) { out = i; break; }
    }
    full.x = full.crates[out].x;
    full.y = full.crates[out].y;
    full.z = full.crates[out].z;
    full.grounded = false;
    full.landed_on = 0xFF;
    tl::world_tick(full, none);
    CHECK(full.fuel == tl::k_fuel_full);
}

// Where the crates are, per mission, and the one that teaches what they are.
void test_the_delivery_teaches_crates_and_the_hop_has_none() {
    // The hop is one leg on one tank with room to spare. Meeting a crate there
    // would spend the lesson on a flight that did not need it.
    tl::World hop;
    tl::world_init(hop, tl::Mission::Hop);
    CHECK(hop.crate_count == 0);

    // The delivery places three, and exactly one of them waits: it appears
    // when the cargo comes aboard, eleven units off the deck the player is
    // standing on, so the first crate they ever meet is the next thing they
    // fly through.
    tl::World w;
    tl::world_init(w, tl::Mission::Delivery);
    int waiting = 0, out = 0;
    for (int i = 0; i < w.crate_count; i++) {
        if (w.crates[i].state == tl::Crate::Waiting) waiting++;
        if (w.crates[i].state == tl::Crate::Out) out++;
    }
    CHECK(w.crate_count == 3);
    CHECK(waiting == 1);
    CHECK(out == 2);

    // Find it, and check it really is beside deck B rather than somewhere the
    // player would have to go looking.
    int idx = -1;
    for (int i = 0; i < w.crate_count; i++) {
        if (w.crates[i].state == tl::Crate::Waiting) { idx = i; break; }
    }
    const int32_t dx = w.crates[idx].x - w.pads[1].x;
    const int32_t dz = w.crates[idx].z - w.pads[1].z;
    const int32_t reach = (iabs(dx) > iabs(dz) ? iabs(dx) : iabs(dz)) >> 16;
    CHECK(reach > (tl::k_pad_half >> 16));   // off the deck, not on it
    CHECK(reach < 16);                       // and in sight of it

    // Picking the cargo up is what puts it out.
    w.x = w.pads[1].x;
    w.z = w.pads[1].z;
    w.y = tl::ground_at(w, w.x, w.z);
    w.grounded = false;
    w.landed_on = 0xFF;
    tl::Input none{};
    tl::world_tick(w, none);
    CHECK(tl::carrying(w));
    CHECK(w.crates[idx].state == tl::Crate::Out);

    // The salvage places two, both out from the start: by then the player has
    // met one and there is nothing left to teach.
    tl::World sea;
    tl::world_init(sea, tl::Mission::Salvage);
    CHECK(sea.crate_count == 2);
    for (int i = 0; i < sea.crate_count; i++) {
        CHECK(sea.crates[i].state == tl::Crate::Out);
    }

    // And they hang over the WATER, not over the sea floor forty units down.
    // ground_at clamps to the waterline, so this is really a check that the
    // crates were placed through it rather than through terrain_height.
    for (int i = 0; i < sea.crate_count; i++) {
        CHECK(sea.crates[i].y > sea.sea);
        CHECK(sea.crates[i].y < sea.sea + (30 << 16));
    }
}


// ---- the shell around a flight ----
//
// These rules used to live inline in game.cpp, which is SDK code that nothing
// in a host checkout can build, so they shipped unverified every time. Three
// player visible bugs came out of that in a row and all three are checked here.

// Every mission number reaches its own flight, and back again.
//
// The bug: start_flight read `mission >= 2 ? Delivery : Hop`, which was correct
// with two missions and wrong with three. Picking mission three flew the
// DELIVERY, so finishing it set progress to three and the game never changed
// again. The gauge went up and the flight did not, which is the worst shape a
// progression bug can take because it looks like it is working.
void test_every_mission_number_reaches_its_own_flight() {
    CHECK(tl::mission_for(1) == tl::Mission::Hop);
    CHECK(tl::mission_for(2) == tl::Mission::Delivery);
    CHECK(tl::mission_for(3) == tl::Mission::Salvage);

    // Every number in range is a DIFFERENT flight. This is the check the old
    // code failed: two numbers mapping to one mission is the whole bug.
    for (uint8_t a = 1; a <= tl::k_mission_count; a++) {
        for (uint8_t b = 1; b <= tl::k_mission_count; b++) {
            if (a == b) continue;
            CHECK(tl::mission_for(a) != tl::mission_for(b));
        }
    }

    // And it round trips, so nothing has to open code the mapping backwards.
    for (uint8_t n = 1; n <= tl::k_mission_count; n++) {
        CHECK(tl::number_of(tl::mission_for(n)) == n);
    }

    // A number out of range is the hop, not undefined behaviour: a corrupt
    // save should drop the player somewhere winnable rather than refuse.
    CHECK(tl::mission_for(0) == tl::Mission::Hop);
    CHECK(tl::mission_for(99) == tl::Mission::Hop);

    // Every mission actually builds a world, which is what makes the mapping
    // worth anything. Checked through world_init rather than by inspection.
    for (uint8_t n = 1; n <= tl::k_mission_count; n++) {
        tl::World w;
        tl::world_init(w, tl::mission_for(n));
        CHECK(w.mission == tl::mission_for(n));
        CHECK(w.state == tl::Flight::Flying);
    }
}

// Landing a mission opens the next one, and nothing ever takes one away.
void test_landing_opens_the_next_mission() {
    CHECK(tl::progress_after(1, tl::Mission::Hop) == 2);
    CHECK(tl::progress_after(2, tl::Mission::Delivery) == 3);

    // The last mission opens nothing, and does not run off the end.
    CHECK(tl::progress_after(3, tl::Mission::Salvage) == 3);
    CHECK(tl::next_mission(tl::Mission::Salvage) == tl::k_mission_count);

    // Replaying an early mission with everything open takes nothing away.
    CHECK(tl::progress_after(3, tl::Mission::Hop) == 3);
    CHECK(tl::progress_after(3, tl::Mission::Delivery) == 3);

    // But pressing on from a replayed hop goes to the DELIVERY, not to the
    // furthest thing unlocked: onward means the next one, and the menu is
    // where a player goes to skip about.
    CHECK(tl::next_mission(tl::Mission::Hop) == 2);
    CHECK(tl::next_mission(tl::Mission::Delivery) == 3);

    // Walked end to end: a player who lands every mission in turn opens each
    // one exactly when they should, and finishes with all of them.
    uint8_t progress = 1;
    for (uint8_t n = 1; n <= tl::k_mission_count; n++) {
        CHECK(n <= progress);          // the mission is open before it is flown
        progress = tl::progress_after(progress, tl::mission_for(n));
    }
    CHECK(progress == tl::k_mission_count);
}

// The title menu is a level select: one row per mission unlocked, then SOUND.
//
// The bug: the title had a single START row that flew the furthest mission
// reached, so a player with three missions open had no way to fly the first
// two ever again.
void test_the_title_lists_every_unlocked_mission() {
    // A first boot is one mission and SOUND.
    CHECK(tl::title_row_count(1) == 2);
    CHECK(tl::title_row_mission(1, 0) == 1);
    CHECK(tl::title_row_mission(1, 1) == 0);      // SOUND, never a mission

    // And it grows with progress, one row at a time.
    for (uint8_t progress = 1; progress <= tl::k_mission_count; progress++) {
        CHECK(tl::title_row_count(progress) == progress + 1);
        for (uint8_t row = 0; row < progress; row++) {
            CHECK(tl::title_row_mission(progress, row) == row + 1);
        }
        // The last row is always SOUND, whatever the progress.
        CHECK(tl::title_row_mission(progress,
                                    tl::title_row_count(progress) - 1) == 0);
        // Every unlocked mission is reachable from some row. This is the
        // check the single START row failed.
        for (uint8_t n = 1; n <= progress; n++) {
            bool listed = false;
            for (uint8_t row = 0; row < tl::title_row_count(progress); row++) {
                if (tl::title_row_mission(progress, row) == n) listed = true;
            }
            CHECK(listed);
        }
        // And nothing LOCKED is on the list.
        for (uint8_t row = 0; row < tl::title_row_count(progress); row++) {
            CHECK(tl::title_row_mission(progress, row) <= progress);
        }
    }

    // Every row name is real, and they are all different, so no two rows read
    // the same.
    for (uint8_t n = 1; n <= tl::k_mission_count; n++) {
        const char* name = tl::mission_name(n);
        CHECK(name != nullptr && name[0] != 0);
        for (uint8_t m = 1; m <= tl::k_mission_count; m++) {
            if (m != n) CHECK(tl::mission_name(m) != name);
        }
    }
}

// Pause has a way back to the title, which is the row that was missing: without
// it the only ways out of a flight were to finish it or wreck it, and on the
// web that meant reloading the page to pick a different mission.
void test_pause_can_leave_the_flight() {
    CHECK(tl::kPauseRowCount == 4);
    CHECK(tl::kPauseResume == 0);
    CHECK(tl::kPauseMenu < tl::kPauseRowCount);
    // The four rows are distinct, which a hand written switch on indices would
    // not guarantee.
    CHECK(tl::kPauseResume != tl::kPauseRestart);
    CHECK(tl::kPauseRestart != tl::kPauseMenu);
    CHECK(tl::kPauseMenu != tl::kPauseSound);

    // Leaving a flight puts the cursor on the mission that was abandoned, so
    // stepping out to try a different one lands where you were rather than at
    // the top of the list.
    for (uint8_t n = 1; n <= tl::k_mission_count; n++) {
        const uint8_t row =
            static_cast<uint8_t>(tl::number_of(tl::mission_for(n)) - 1);
        CHECK(tl::title_row_mission(tl::k_mission_count, row) == n);
    }
}

// A cursor wraps both ways and never leaves the list, at every length the
// title menu can be.
void test_a_menu_cursor_wraps_and_stays_in_range() {
    for (uint8_t count = 1; count <= tl::k_mission_count + 1; count++) {
        CHECK(tl::menu_step(0, -1, count) == count - 1);   // up from the top
        CHECK(tl::menu_step(count - 1, 1, count) == 0);    // down from the end
        // Walked all the way round in both directions, never out of range.
        uint8_t at = 0;
        for (int step = 0; step < count * 3; step++) {
            at = tl::menu_step(at, 1, count);
            CHECK(at < count);
        }
        CHECK(at == (count * 3) % count);
        for (int step = 0; step < count * 3; step++) {
            at = tl::menu_step(at, -1, count);
            CHECK(at < count);
        }
    }
    // An empty list cannot move, rather than dividing by zero.
    CHECK(tl::menu_step(0, 1, 0) == 0);
}

// Touching the sea is lost however gently, because a lander is not a boat and
// a soft ditching that merely parked the ship would make the ocean scenery.
void test_ditching_is_a_crash_at_any_speed() {
    tl::World w;
    tl::world_init(w, tl::Mission::Salvage);
    // Halfway between the shore apron's edge and the wreck, which is open
    // sea rather than the midpoint of the leg (that lands on the apron).
    w.x = w.pads[0].x + (w.pads[1].x - w.pads[0].x) * 3 / 4;
    w.z = w.pads[0].z + (w.pads[1].z - w.pads[0].z) * 3 / 4;
    w.y = tl::ground_at(w, w.x, w.z) + (1 << 16);   // a gentle unit of fall
    w.grounded = false;
    tl::Input none{};
    run(w, none, 200);
    CHECK(w.state == tl::Flight::Crashed);
    CHECK(w.fault == tl::Fault::Ditched);
}

void test_the_salvage_is_carried_back_to_the_shore() {
    tl::World w;
    tl::world_init(w, tl::Mission::Salvage);
    land_on(w, 1);                                  // down on the section
    CHECK(w.state == tl::Flight::Flying);           // a leg, not an ending
    CHECK(tl::carrying(w));
    CHECK(w.target == 0);                           // now aimed at the shore

    land_on(w, 0);
    CHECK(w.state == tl::Flight::Landed);
    CHECK(w.cargo == tl::kCargoDone);
    CHECK(w.landed_on == 0);
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
    test_no_pod_can_yaw_the_hull();
    test_opposite_pods_cancel();
    test_up_vector_follows_the_hull();
    test_up_in_hull_is_the_mirror_of_hull_up();
    test_a_pod_turns_the_hull_about_its_own_axis();
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
    test_mission_two_starts_with_the_crate_on_the_middle_deck();
    test_landing_on_the_crate_loads_it_and_flies_on();
    test_delivering_to_the_third_deck_ends_the_flight();
    test_the_crate_is_only_picked_up_once();
    test_the_crate_is_heavy();
    test_the_crate_sways();
    test_the_salvage_starts_ashore_with_the_wreck_at_sea();
    test_ditching_is_a_crash_at_any_speed();
    test_the_salvage_is_carried_back_to_the_shore();
    test_the_sea_floor_falls_away_from_the_coast();
    test_the_salvage_square_fits_the_section();
    test_the_bands_land_on_exact_readout_values();
    test_a_hard_landing_costs_hull_and_two_of_them_end_it();
    test_running_dry_is_a_glide_before_it_is_a_fail();
    test_what_a_dead_stick_glide_lands_on_is_what_decides_it();
    test_a_deck_no_longer_refuels();
    test_a_fuel_crate_is_half_a_tank_and_is_taken_once();
    test_the_delivery_teaches_crates_and_the_hop_has_none();
    test_every_mission_number_reaches_its_own_flight();
    test_landing_opens_the_next_mission();
    test_the_title_lists_every_unlocked_mission();
    test_pause_can_leave_the_flight();
    test_a_menu_cursor_wraps_and_stays_in_range();
    test_memory_budget();

    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
