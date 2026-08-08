// Host side tests for Pico Space Program's flight model. Pure integer C++, so
// the claims the game is balanced on are proven here rather than asserted in
// comments.
//
// The load bearing one is at the bottom: an autopilot that reads nothing but
// the numbers the HUD shows flies the whole Pip mission, off the pad, into
// orbit, through the transfer window the map lights, and down onto the moon.
// A space game can be right about every equation and still be unwinnable, and
// nothing short of flying it end to end finds that out.
//
// The autopilot uses floating point. The SIM does not, and that is the line
// that matters: the thing under test is integer, and a test harness that has
// to steer is allowed the same maths a person with a pencil would use. What it
// is not allowed is a second copy of the orbital mechanics, so it steers by
// ps::elements, ps::prograde_angle and ps::burn_window, which is exactly what
// the HUD and the map draw.

#include <cmath>
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

int32_t iabs(int32_t v) { return v < 0 ? -v : v; }

int32_t wrap_s(int32_t a) {
    a %= ps::k_turn;
    if (a > ps::k_turn / 2) a -= ps::k_turn;
    if (a <= -ps::k_turn / 2) a += ps::k_turn;
    return a;
}

void run(ps::World& w, const ps::Input& in, int ticks) {
    for (int i = 0; i < ticks && ps::flying(w); i++) ps::world_tick(w, in);
}

// A ship parked on an exact circular orbit, so orbital mechanics can be
// measured without an ascent in the way.
void park(ps::World& w, uint8_t body, int32_t radius_m, ps::Mission mission) {
    ps::world_init(w, mission);
    w.grounded = false;
    w.landed_on = ps::kBodyCount;
    w.stage = 1;
    w.fuel_kg = ps::k_stages[1].fuel_kg * ps::k_fp8;
    int32_t bx, by, bvx, bvy;
    ps::body_position(w, body, bx, by);
    ps::body_velocity(w, body, bvx, bvy);
    const double mu = static_cast<double>(ps::k_bodies[body].mu);
    const double v = std::sqrt(mu / radius_m);
    w.x = (static_cast<int64_t>(bx) + radius_m) << 16;
    w.y = static_cast<int64_t>(by) << 16;
    w.vx = bvx;
    w.vy = bvy + static_cast<int32_t>(std::lround(v * ps::k_fp16));
    w.ref_body = ps::reference_body(w);
}

int32_t radius_from(const ps::World& w, uint8_t body) {
    int32_t bx, by;
    ps::body_position(w, body, bx, by);
    const int64_t dx = (w.x >> 16) - bx;
    const int64_t dy = (w.y >> 16) - by;
    return static_cast<int32_t>(ps::isqrt64(dx * dx + dy * dy));
}

// ---- the autopilot ---------------------------------------------------------

void aim(const ps::World& w, ps::Input& in, int32_t want) {
    const int32_t err = wrap_s(want - w.angle / ps::k_fp16);
    if (err > 2) in.left = true;
    if (err < -2) in.right = true;
}

void relative(const ps::World& w, double& rx, double& ry,
              double& vx, double& vy) {
    int32_t bx, by, bvx, bvy;
    ps::body_position(w, w.ref_body, bx, by);
    ps::body_velocity(w, w.ref_body, bvx, bvy);
    rx = static_cast<double>(w.x >> 16) - bx;
    ry = static_cast<double>(w.y >> 16) - by;
    vx = (w.vx - bvx) / 65536.0;
    vy = (w.vy - bvy) / 65536.0;
}

double climb_ms(const ps::World& w) {
    double rx, ry, vx, vy;
    relative(w, rx, ry, vx, vy);
    const double r = std::sqrt(rx * rx + ry * ry);
    return r > 0.0 ? (vx * rx + vy * ry) / r : 0.0;
}

int32_t units_of(double radians) {
    int32_t a = static_cast<int32_t>(
        std::lround(radians * ps::k_turn / (2.0 * 3.14159265358979)));
    return ((a % ps::k_turn) + ps::k_turn) % ps::k_turn;
}

// Horizontal in the direction of travel, pitched up by whatever it takes to
// stop the ship falling. Burning straight along a velocity that already points
// downhill spends the burn stretching the orbit instead of rounding it, which
// is a real result and not a harness detail: it is the reason the upper stage
// carries the thrust it does.
int32_t level_aim(const ps::World& w) {
    double rx, ry, vx, vy;
    relative(w, rx, ry, vx, vy);
    const double r = std::sqrt(rx * rx + ry * ry);
    const double ux = rx / r, uy = ry / r;
    const double climb = vx * ux + vy * uy;
    const double hx = vx - climb * ux, hy = vy - climb * uy;
    const double lift = -climb * 3.0;
    return units_of(std::atan2(hy + uy * lift, hx + ux * lift));
}

void ascend(ps::World& w, int32_t target_ap_m, int max_ticks) {
    for (int i = 0; i < max_ticks && ps::flying(w); i++) {
        const ps::Elements el = ps::elements(w);
        if (el.closed && el.apoapsis_m - ps::k_home_radius_m >= target_ap_m) {
            return;
        }
        ps::Input in{};
        in.up = w.throttle < 255;
        const int32_t alt = ps::altitude_m(w);
        int32_t want = ps::k_turn / 4;
        if (alt > 900) {
            const int32_t t = alt > 42000 ? 1024 : (alt - 900) * 1024 / 41100;
            want = 1024 - t;
        }
        aim(w, in, want);
        if (w.stage == 0 && w.fuel_kg <= 0) in.stage = true;
        ps::world_tick(w, in);
    }
}

template <typename F>
void coast_until(ps::World& w, F stop, int max_ticks) {
    for (int i = 0; i < max_ticks && ps::flying(w); i++) {
        ps::Input in{};
        if (w.throttle > 0) { in.down = true; ps::world_tick(w, in); continue; }
        if (stop(w)) return;
        if (w.warp_step == 0 && ps::warp_allowed(w)) in.warp = true;
        ps::world_tick(w, in);
    }
}

template <typename F>
void burn_until(ps::World& w, F stop, bool retro, int max_ticks,
                bool level = false) {
    for (int i = 0; i < max_ticks && ps::flying(w); i++) {
        if (stop(w)) break;
        ps::Input in{};
        in.up = w.throttle < 255;
        aim(w, in, level ? level_aim(w)
                         : ps::prograde_angle(w) + (retro ? ps::k_turn / 2 : 0));
        if (w.stage == 0 && w.fuel_kg <= 0) in.stage = true;
        ps::world_tick(w, in);
    }
    for (int i = 0; i < 80 && ps::flying(w) && w.throttle > 0; i++) {
        ps::Input in{};
        in.down = true;
        ps::world_tick(w, in);
    }
}

// Round the orbit off by trimming at whichever apsis comes next. One pass does
// not get there from a launch ellipse, because the ship falls while it burns;
// a few passes converge the way a player circularising over a couple of orbits
// does.
void circularise(ps::World& w, int32_t gap) {
    for (int pass = 0; pass < 6 && ps::flying(w); pass++) {
        const ps::Elements before = ps::elements(w);
        if (before.closed && before.apoapsis_m - before.periapsis_m < gap) {
            return;
        }
        coast_until(w, [](const ps::World& s) { return climb_ms(s) <= 0.0; },
                    400000);
        burn_until(w, [gap](const ps::World& s) {
            const ps::Elements el = ps::elements(s);
            return !el.closed || el.apoapsis_m - el.periapsis_m < gap;
        }, false, 60000, true);
    }
}

// A retrograde descent flown against a speed ceiling that falls with altitude:
// fast while there is room to stop, slow when there is not.
void descend(ps::World& w, int max_ticks) {
    for (int i = 0; i < max_ticks && ps::flying(w); i++) {
        ps::Input in{};
        // Measured from the GEAR, not from the ship's origin. altitude_m never
        // reads below the height the ship rests at, so a ceiling built on it
        // bottoms out six metres up and the last six metres are flown with no
        // ceiling at all: the descent came in at 12.1 m/s against a limit of
        // 12 and the mission failed by a tenth of a metre a second.
        const double alt = ps::clearance_m(w);
        const double spd = ps::speed_fp16(w) / 65536.0;
        const double near = 2.0 + alt / 18.0;
        const double far = 2.0 + std::sqrt(6.0 * alt);
        const double ceiling = near < far ? near : far;
        aim(w, in, ps::prograde_angle(w) + ps::k_turn / 2);
        // Lit a little before the ceiling rather than at it: the throttle
        // takes half a second to open and a controller that waits for the
        // limit is always chasing it downward.
        if (spd > ceiling * 0.88) in.up = w.throttle < 255;
        else in.down = w.throttle > 0;
        ps::world_tick(w, in);
    }
}

// ---- the tests -------------------------------------------------------------

void test_pad() {
    ps::World w;
    ps::world_init(w, ps::Mission::Orbit);
    CHECK(w.grounded);
    CHECK(w.landed_on == ps::kPicopiter);
    CHECK(w.stage == 0);
    CHECK(w.throttle == 0);
    CHECK(w.fuel_kg == ps::k_stages[0].fuel_kg * ps::k_fp8);
    CHECK(w.angle / ps::k_fp16 == ps::k_turn / 4);      // nose straight up
    // The pad apron is levelled, so the ship stands exactly its own height
    // over the mean radius and nothing else. With the booster still on, that
    // is the engine bell and the deck under it, not the legs.
    CHECK(ps::terrain_m(ps::kPicopiter, ps::k_turn / 4) == 0);
    CHECK(ps::stand_m(w) == ps::k_stack_m);
    CHECK(ps::altitude_m(w) == ps::k_stack_m);
    CHECK(ps::clearance_m(w) == 0);          // it is sitting on something
    // Off the plane the ship flies in is scenery, and in the plane it is the
    // sim's own ground. The identity is what lets the renderer draw a sphere
    // while the collision reads a profile.
    for (int32_t a = 0; a < ps::k_turn; a += 137) {
        CHECK(ps::terrain_at(ps::kPicopiter, a, 0) ==
              ps::terrain_m(ps::kPicopiter, a));
        CHECK(ps::terrain_at(ps::kPip, a, 0) == ps::terrain_m(ps::kPip, a));
    }
    CHECK(ps::mass_fp8(w) == ps::k_launch_mass_kg * ps::k_fp8);

    // A rocket with the engine off sits there. It does not sink into its pad
    // and it does not drift off it.
    ps::Input none{};
    run(w, none, 500);
    CHECK(ps::flying(w));
    CHECK(w.grounded);
    CHECK(ps::altitude_m(w) == ps::k_stack_m);
    CHECK(ps::speed_fp16(w) == 0);
}

void test_liftoff() {
    ps::World w;
    ps::world_init(w, ps::Mission::Orbit);
    ps::Input up{};
    up.up = true;

    // Half throttle is under the rocket's own weight, so nothing happens.
    for (int i = 0; i < 25; i++) ps::world_tick(w, up);
    CHECK(w.throttle > 0 && w.throttle < 160);
    CHECK(w.grounded);
    CHECK(ps::clearance_m(w) == 0);

    run(w, up, 1400);
    CHECK(!w.grounded);
    CHECK(ps::altitude_m(w) > 300);
    CHECK(w.fuel_kg < ps::k_stages[0].fuel_kg * ps::k_fp8);
    // Straight up: the ship has climbed and it has not gone sideways.
    CHECK(iabs(static_cast<int32_t>(w.x >> 16)) < 40);
}

void test_thrust_follows_the_nose() {
    // Off the ground, out of the air, engine along +x. The velocity it builds
    // has to be along +x too, because thrust acts along the nose and that is
    // the whole flight model.
    ps::World w;
    park(w, ps::kPicopiter, ps::k_home_radius_m + 200000, ps::Mission::Pip);
    w.vx = 0;
    w.vy = 0;
    w.angle = 0;                        // nose along +x
    ps::Input up{};
    up.up = true;
    run(w, up, 300);
    CHECK(w.vx > 0);
    CHECK(iabs(w.vy) < w.vx / 6);       // gravity pulls -x here, not -y

    // And the other way: nose reversed, velocity reverses.
    ps::World back;
    park(back, ps::kPicopiter, ps::k_home_radius_m + 200000, ps::Mission::Pip);
    back.vx = 0;
    back.vy = 0;
    back.angle = (ps::k_turn / 2) * ps::k_fp16;
    run(back, up, 300);
    CHECK(back.vx < 0);
}

void test_staging() {
    ps::World w;
    ps::world_init(w, ps::Mission::Orbit);
    const int32_t before = ps::mass_fp8(w);
    CHECK(!w.debris);

    ps::Input stage{};
    stage.stage = true;
    ps::world_tick(w, stage);
    CHECK(w.stage == 1);
    CHECK(w.fuel_kg == ps::k_stages[1].fuel_kg * ps::k_fp8);
    CHECK(w.debris);
    // The booster's dry mass and everything left in its tank both leave.
    CHECK(ps::mass_fp8(w) < before);
    CHECK(ps::mass_fp8(w) ==
          (ps::k_stages[1].dry_kg + ps::k_stages[1].fuel_kg) * ps::k_fp8);

    // A second press does nothing: there is no third stage to light.
    ps::world_tick(w, stage);
    CHECK(w.stage == 1);

    // Staging on the pad is legal and it is a mistake: the upper stage has the
    // thrust to fly off Picopiter on its own and nothing like the propellant
    // to reach orbit once it has thrown the booster away. The sim does not
    // stop a player doing it, and it should not: a rocket that refuses a
    // command is worse than one that obeys a bad one.
    ps::Input up{};
    up.up = true;
    run(w, up, 1400);
    CHECK(!w.grounded);
    CHECK(ps::altitude_m(w) > 300);
}

void test_circular_orbit_stays_circular() {
    // Flown as the Pip mission, not the orbit one: reaching a stable orbit IS
    // the end of the orbit mission, so that mission would call the flight over
    // on the first tick and there would be nothing left to integrate.
    ps::World w;
    park(w, ps::kPicopiter, ps::k_home_radius_m + 80000, ps::Mission::Pip);
    const ps::Elements start = ps::elements(w);
    CHECK(start.closed);
    CHECK(iabs(start.apoapsis_m - start.periapsis_m) < 600);

    // Ten thousand seconds, which is about eleven revolutions, at the warp the
    // ladder tops out at. A step that long is the real test of the integrator:
    // an explicit one loses energy every step and spirals in within a lap.
    w.warp_step = ps::k_warp_count - 1;
    ps::Input none{};
    run(w, none, 5000);
    const ps::Elements after = ps::elements(w);
    std::printf("  11 revolutions at x200: ap %d -> %d, pe %d -> %d, "
                "radius %d -> %d\n", start.apoapsis_m, after.apoapsis_m,
                start.periapsis_m, after.periapsis_m,
                ps::k_home_radius_m + 80000, radius_from(w, ps::kPicopiter));
    CHECK(after.closed);
    // The orbit stays an orbit. A two second step walks the apsides about a
    // little, which a symplectic step does and an explicit one would not: an
    // explicit Euler loses energy every step and spirals in, so the failure
    // this guards against is one sided and large, not a wobble.
    CHECK(iabs(after.apoapsis_m - start.apoapsis_m) < 6000);
    CHECK(iabs(after.periapsis_m - start.periapsis_m) < 6000);
    // And it has not quietly decayed: the ship is still up there.
    CHECK(radius_from(w, ps::kPicopiter) > ps::k_home_radius_m + 60000);
    CHECK(ps::flying(w));
}

void test_warp_agrees_with_real_time() {
    // The same orbit stepped at x1 and at x50 has to end up in the same place.
    // Warp lengthens the tick rather than running more of them, so this is the
    // check that lengthening it does not change the physics.
    ps::World slow, fast;
    park(slow, ps::kPicopiter, ps::k_home_radius_m + 60000, ps::Mission::Pip);
    park(fast, ps::kPicopiter, ps::k_home_radius_m + 60000, ps::Mission::Pip);
    fast.warp_step = 2;                          // x50

    ps::Input none{};
    run(slow, none, 20000);                      // 200 s
    run(fast, none, 400);                        // 200 s
    CHECK(slow.mission_ms == fast.mission_ms);

    const int64_t dx = (slow.x >> 16) - (fast.x >> 16);
    const int64_t dy = (slow.y >> 16) - (fast.y >> 16);
    const int32_t miss = static_cast<int32_t>(ps::isqrt64(dx * dx + dy * dy));
    // A fiftyfold step walks round the orbit slightly out of phase. Under a
    // kilometre over 200 seconds on a 160 km orbit is a tenth of a degree.
    std::printf("  warp x50 drifts %d m over 200 s\n", miss);
    CHECK(miss < 1000);
}

void test_air() {
    // Low and fast, in the thick of it: the air has to take speed away.
    ps::World w;
    ps::world_init(w, ps::Mission::Orbit);
    w.grounded = false;
    w.landed_on = ps::kBodyCount;
    w.y = static_cast<int64_t>(ps::k_home_radius_m + 3000) << 16;
    w.vx = 400 * ps::k_fp16;
    ps::Input none{};
    run(w, none, 100);
    const int32_t low_loss = 400 * ps::k_fp16 - w.vx;
    CHECK(low_loss > 0);

    // The same speed above the air. It is not compared against zero, because
    // gravity is still acting and a ship that has moved sideways is being
    // pulled sideways: what the test says is that above the air the loss is a
    // rounding error against what the air takes, which is the claim.
    ps::World high;
    ps::world_init(high, ps::Mission::Orbit);
    high.grounded = false;
    high.landed_on = ps::kBodyCount;
    high.y = static_cast<int64_t>(ps::k_home_radius_m +
                                   ps::k_home_atmo_m + 30000) << 16;
    high.vx = 400 * ps::k_fp16;
    run(high, none, 100);
    const int32_t high_loss = iabs(400 * ps::k_fp16 - high.vx);
    std::printf("  one second at 400 m/s costs %.2f m/s of speed at 3 km and "
                "%.2f m/s above the air\n", low_loss / 65536.0,
                high_loss / 65536.0);
    CHECK(low_loss > high_loss * 20);
}

void test_touchdown_bands() {
    struct Case { int32_t speed_ms; bool survives; };
    const Case cases[] = {
        {2, true}, {4, true}, {11, true}, {13, false}, {40, false},
    };
    for (const Case& c : cases) {
        ps::World w;
        ps::world_init(w, ps::Mission::Orbit);
        w.grounded = false;
        w.landed_on = ps::kBodyCount;
        // Placed exactly at its resting height, so the speed it touches down
        // at is the speed in the case. Dropping it from even forty metres adds
        // twenty five metres a second on the way down, which puts every case
        // in the same band and tests nothing.
        w.stage = 1;                        // on its legs, as a lander is
        w.fuel_kg = 0;
        w.y = static_cast<int64_t>(ps::k_home_radius_m + ps::k_gear_m) << 16;
        w.vy = -c.speed_ms * ps::k_fp16;
        ps::Input none{};
        run(w, none, 400);
        CHECK(!ps::flying(w));
        // The band the HUD colours by and the verdict the sim reaches are the
        // same function, so a green readout can never end in a wreck.
        const bool clean = ps::touchdown_band(c.speed_ms * ps::k_fp16) !=
                           ps::Touchdown::Fatal;
        CHECK(clean == c.survives);
        if (c.survives) {
            CHECK(w.state == ps::Flight::Landed);
            CHECK(w.grounded);
        } else {
            CHECK(w.state == ps::Flight::Crashed);
            CHECK(w.fault == ps::Fault::Impact);
        }
    }

    // Gently, but on its side. The gear does not take that either, and the
    // game says which of the two things went wrong.
    ps::World tipped;
    ps::world_init(tipped, ps::Mission::Orbit);
    tipped.grounded = false;
    tipped.landed_on = ps::kBodyCount;
    tipped.stage = 1;
    tipped.fuel_kg = 0;
    tipped.y = static_cast<int64_t>(ps::k_home_radius_m + ps::k_gear_m) << 16;
    tipped.vy = -2 * ps::k_fp16;
    tipped.angle = 0;                     // a quarter turn off vertical
    ps::Input none{};
    run(tipped, none, 400);
    CHECK(tipped.state == ps::Flight::Crashed);
    CHECK(tipped.fault == ps::Fault::Toppled);
}

void test_stranded() {
    // An empty tank on an orbit that never comes down is the end of a flight,
    // and only after the grace period: a ship coasting to its landing with
    // nothing left has still done the work.
    ps::World w;
    park(w, ps::kPicopiter, ps::k_home_radius_m + 60000, ps::Mission::Pip);
    w.fuel_kg = 0;
    CHECK(ps::out_of_fuel(w));

    ps::Input none{};
    run(w, none, 60);
    CHECK(ps::flying(w));                 // still inside the grace period

    w.warp_step = 1;                      // x10, so the grace passes
    run(w, none, 1000);
    CHECK(w.state == ps::Flight::Stranded);
    CHECK(w.fault == ps::Fault::Dry);
}

void test_moons_on_rails() {
    ps::World w;
    ps::world_init(w, ps::Mission::Pom);
    w.warp_step = ps::k_warp_count - 1;
    ps::Input none{};
    for (uint8_t b = 1; b < ps::kBodyCount; b++) {
        int32_t bx, by;
        ps::body_position(w, b, bx, by);
        const int32_t r = static_cast<int32_t>(
            ps::isqrt64(static_cast<int64_t>(bx) * bx +
                        static_cast<int64_t>(by) * by));
        CHECK(iabs(r - ps::k_bodies[b].orbit_m) < 40);
    }
    run(w, none, 4000);                   // 8000 s, more than one Pip lap
    for (uint8_t b = 1; b < ps::kBodyCount; b++) {
        int32_t bx, by;
        ps::body_position(w, b, bx, by);
        const int32_t r = static_cast<int32_t>(
            ps::isqrt64(static_cast<int64_t>(bx) * bx +
                        static_cast<int64_t>(by) * by));
        CHECK(iabs(r - ps::k_bodies[b].orbit_m) < 40);
        // And its speed is the speed a circle at that radius runs at.
        int32_t vx, vy;
        ps::body_velocity(w, b, vx, vy);
        const double speed = ps::isqrt64(static_cast<int64_t>(vx) * vx +
                                          static_cast<int64_t>(vy) * vy) /
                             65536.0;
        const double want = std::sqrt(
            static_cast<double>(ps::k_bodies[ps::kPicopiter].mu) /
            ps::k_bodies[b].orbit_m);
        CHECK(std::fabs(speed - want) < want * 0.01);
    }
}

void test_burn_window() {
    // The window is a statement about geometry: the moon must lead the ship by
    // half a turn less however far it travels while the ship is on its way.
    // Park a ship, wait for the light, and check the angle it lit at.
    ps::World w;
    park(w, ps::kPicopiter, ps::k_home_radius_m + 80000, ps::Mission::Pip);
    ps::Input none{};
    bool lit = false;
    for (int i = 0; i < 200000 && !lit; i++) {
        w.warp_step = 2;
        ps::world_tick(w, none);
        lit = ps::burn_window(w);
    }
    CHECK(lit);
    if (!lit) return;

    const int32_t moon = w.moon_phase[ps::kPip] / ps::k_fp16;
    const int32_t ship = ps::bearing_from(w, ps::kPicopiter);
    int32_t lead = (moon - ship) % ps::k_turn;
    if (lead < 0) lead += ps::k_turn;

    // The textbook answer, worked out in doubles from the same constants.
    const double mu = static_cast<double>(ps::k_bodies[ps::kPicopiter].mu);
    const double r1 = ps::k_home_radius_m + 80000.0;
    const double r2 = ps::k_bodies[ps::kPip].orbit_m;
    const double at = (r1 + r2) / 2.0;
    const double t_half = 3.14159265358979 * std::sqrt(at * at * at / mu);
    const double moon_rate = ps::k_bodies[ps::kPip].rate_fp16 / 65536.0;
    const double want = ps::k_turn / 2.0 - moon_rate * t_half;
    std::printf("  window lit at a lead of %d units, textbook says %.0f\n",
                lead, want);
    CHECK(std::fabs(lead - want) <= ps::k_window_tol + 8);
}

void test_orbit_mission() {
    ps::World w;
    ps::world_init(w, ps::Mission::Orbit);
    ascend(w, 80000, 60000);
    CHECK(ps::flying(w));
    CHECK(w.stage == 1);                  // the booster cannot do it alone
    circularise(w, 14000);
    CHECK(w.state == ps::Flight::Complete);
    const ps::Elements el = ps::elements(w);
    std::printf("  orbit mission: %u s, %d kg left, ap %d pe %d\n",
                w.mission_ms / 1000, w.fuel_kg / ps::k_fp8,
                el.apoapsis_m - ps::k_home_radius_m,
                el.periapsis_m - ps::k_home_radius_m);
}

// The whole thing, and the reason this file exists. Nothing else proves the
// game can be finished.
//
// Run for both moons, because they are not the same flight: Pom is twice the
// range on the same tank, its window opens at a different lead angle, and its
// transfer takes long enough that the warp ladder has to hold an orbit
// together for over an hour of mission time. A game whose last mission cannot
// be flown is a game with a last mission nobody has ever seen.
void fly_mission(ps::Mission mission, uint8_t moon) {
    ps::World w;
    ps::world_init(w, mission);

    ascend(w, 80000, 60000);
    circularise(w, 14000);
    CHECK(ps::flying(w));
    CHECK(w.ref_body == ps::kPicopiter);

    coast_until(w, [](const ps::World& s) { return ps::burn_window(s); },
                900000);
    CHECK(ps::burn_window(w));

    burn_until(w, [moon](const ps::World& s) {
        const ps::Elements el = ps::elements(s);
        return !el.closed ||
               el.apoapsis_m >= ps::k_bodies[moon].orbit_m - 3000;
    }, false, 60000);
    CHECK(ps::flying(w));

    // How close the transfer would come if nobody touched it again, which is
    // the number that says whether the window cue is honest. Measured on a
    // COPY, coasted past the encounter: measuring it on the real ship and
    // stopping at the capture radius only ever reports the capture radius,
    // which is a tautology dressed up as a test.
    {
        ps::World drift = w;
        int32_t closest = 0x7FFFFFFF;
        const ps::Input none{};
        for (int i = 0; i < 400000 && ps::flying(drift); i++) {
            drift.warp_step = 2;
            ps::world_tick(drift, none);
            int32_t bx, by;
            ps::body_position(drift, moon, bx, by);
            const int64_t dx = (drift.x >> 16) - bx;
            const int64_t dy = (drift.y >> 16) - by;
            const int32_t d =
                static_cast<int32_t>(ps::isqrt64(dx * dx + dy * dy));
            if (d < closest) closest = d;
            if (d > closest && closest < ps::k_bodies[moon].ref_m) break;
        }
        std::printf("  the transfer passes %d m from %s, capture radius %d\n",
                    closest, ps::k_bodies[moon].name, ps::k_bodies[moon].ref_m);
        CHECK(closest < ps::k_bodies[moon].ref_m);
    }

    coast_until(w, [moon](const ps::World& s) { return s.ref_body == moon; },
                900000);
    CHECK(w.ref_body == moon);
    if (w.ref_body != moon) return;

    descend(w, 400000);
    std::printf("  %s mission: %u s, %d kg left, touchdown at %d cm/s, "
                "state %d\n", ps::k_bodies[moon].name, w.mission_ms / 1000,
                w.fuel_kg / ps::k_fp8, w.touch_speed * 100 / ps::k_fp16,
                static_cast<int>(w.state));
    CHECK(w.state == ps::Flight::Complete);
    CHECK(w.landed_on == moon);
    CHECK(w.fuel_kg > 0);                 // and with something in the tank
}

void test_pip_mission() { fly_mission(ps::Mission::Pip, ps::kPip); }
void test_pom_mission() { fly_mission(ps::Mission::Pom, ps::kPom); }

void test_determinism() {
    // Same inputs, same flight. A sim that is not a pure function of its
    // inputs cannot be tested at all, and this is the check that says so.
    ps::World a, b;
    ps::world_init(a, ps::Mission::Pip);
    ps::world_init(b, ps::Mission::Pip);
    for (int i = 0; i < 4000; i++) {
        ps::Input in{};
        in.up = (i % 7) != 0;
        in.left = (i / 300) % 2 == 0;
        in.right = (i / 300) % 2 == 1;
        in.stage = i == 3000;
        ps::world_tick(a, in);
        ps::world_tick(b, in);
    }
    CHECK(a.x == b.x && a.y == b.y);
    CHECK(a.vx == b.vx && a.vy == b.vy);
    CHECK(a.angle == b.angle);
    CHECK(a.fuel_kg == b.fuel_kg);
}

void test_ram_budget() {
    // The device has 264 KB and the SDK has already spent 115 of it on a
    // framebuffer. A sim state that grows without anyone noticing is how a
    // console build runs out.
    std::printf("  sizeof(ps::World) = %zu bytes\n", sizeof(ps::World));
    CHECK(sizeof(ps::World) <= 192);
}

}  // namespace

int main() {
    test_pad();
    test_liftoff();
    test_thrust_follows_the_nose();
    test_staging();
    test_circular_orbit_stays_circular();
    test_warp_agrees_with_real_time();
    test_air();
    test_touchdown_bands();
    test_stranded();
    test_moons_on_rails();
    test_burn_window();
    test_orbit_mission();
    test_pip_mission();
    test_pom_mission();
    test_determinism();
    test_ram_budget();

    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
