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
    test_memory_budget();

    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
