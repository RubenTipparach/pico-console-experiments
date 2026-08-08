#include "sim.hpp"

namespace ps {
namespace {

// A quarter turn of sine, fp14, every eighth angle unit. The same table the
// lander uses, for the same reason: 129 int16 is 258 bytes of flash against
// the 8 KB a full turn at unit resolution would cost, and the interpolation
// between entries is one multiply.
const int16_t k_sin_quarter[129] = {
         0,    201,    402,    603,    804,   1005,   1205,   1406,
      1606,   1806,   2006,   2205,   2404,   2603,   2801,   2999,
      3196,   3393,   3590,   3786,   3981,   4176,   4370,   4563,
      4756,   4948,   5139,   5330,   5520,   5708,   5897,   6084,
      6270,   6455,   6639,   6823,   7005,   7186,   7366,   7545,
      7723,   7900,   8076,   8250,   8423,   8595,   8765,   8935,
      9102,   9269,   9434,   9598,   9760,   9921,  10080,  10238,
     10394,  10549,  10702,  10853,  11003,  11151,  11297,  11442,
     11585,  11727,  11866,  12004,  12140,  12274,  12406,  12537,
     12665,  12792,  12916,  13039,  13160,  13279,  13395,  13510,
     13623,  13733,  13842,  13949,  14053,  14155,  14256,  14354,
     14449,  14543,  14635,  14724,  14811,  14896,  14978,  15059,
     15137,  15213,  15286,  15357,  15426,  15493,  15557,  15619,
     15679,  15736,  15791,  15843,  15893,  15941,  15986,  16029,
     16069,  16107,  16143,  16176,  16207,  16235,  16261,  16284,
     16305,  16324,  16340,  16353,  16364,  16373,  16379,  16383,
     16384,
};

int32_t sin_quarter(int32_t a) {          // a in [0, k_quarter]
    const int32_t i = a >> 3;
    const int32_t f = a & 7;
    if (i >= 128) return k_fp14;
    const int32_t lo = k_sin_quarter[i];
    return lo + (((k_sin_quarter[i + 1] - lo) * f) >> 3);
}

int32_t clamp_i(int32_t v, int32_t lo, int32_t hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

int64_t clamp_l(int64_t v, int64_t lo, int64_t hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

int32_t abs_i(int32_t v) { return v < 0 ? -v : v; }
int64_t abs_l(int64_t v) { return v < 0 ? -v : v; }

// Wrap an angle to (-half turn, +half turn]. What an error signal wants: the
// autopilot should turn 10 degrees the short way, not 350 the long way.
int32_t wrap_signed(int32_t a) {
    a %= k_turn;
    if (a > k_turn / 2) a -= k_turn;
    if (a <= -k_turn / 2) a += k_turn;
    return a;
}

int32_t wrap_turn(int32_t a) {
    a %= k_turn;
    return a < 0 ? a + k_turn : a;
}

// Gravity is accumulated with four extra fractional bits and shifted down
// once, so a moon's pull at three hundred kilometres (a couple of hundredths
// of a fp16 count per tick at x1) does not truncate to nothing. It is a real
// term: it is the entire reason a transfer arrives anywhere.
constexpr int k_grav_extra = 4;

// The pad is at the top of Picopiter, and the ground around it is flattened so
// a rocket stands on a level surface. The blend runs out over k_pad_blend
// angle units, which at 60 km of radius is about a kilometre and a half of
// apron, so the flat does not end in a cliff.
constexpr int32_t k_pad_bearing = k_turn / 4;         // 1024, straight up
constexpr int32_t k_pad_flat = 26;
constexpr int32_t k_pad_blend = 70;

// Air density, 0..256, sampled every fifth of the atmosphere and interpolated.
// exp(-h/H) with H one fifth of the top, so the last entry is not zero and the
// air thins out rather than ending.
const int16_t k_density[6] = {256, 94, 35, 13, 5, 2};

int32_t density256(int32_t alt_m) {
    if (alt_m <= 0) return 256;
    if (alt_m >= k_home_atmo_m) return 0;
    const int32_t step = k_home_atmo_m / 5;
    const int32_t i = alt_m / step;
    const int32_t f = alt_m - i * step;
    const int32_t lo = k_density[i], hi = k_density[i + 1];
    return lo + ((hi - lo) * f) / step;
}

}  // namespace

// ------------------------------------------------------------------ maths

int32_t sin_fp(int32_t angle) {
    int32_t a = angle % k_turn;
    if (a < 0) a += k_turn;
    if (a <= k_quarter) return sin_quarter(a);
    if (a <= 2 * k_quarter) return sin_quarter(2 * k_quarter - a);
    if (a <= 3 * k_quarter) return -sin_quarter(a - 2 * k_quarter);
    return -sin_quarter(4 * k_quarter - a);
}

int32_t cos_fp(int32_t angle) { return sin_fp(angle + k_quarter); }

int64_t isqrt64(int64_t value) {
    if (value <= 0) return 0;
    // Newton from a shift based seed. Converges in a handful of iterations for
    // anything this sim produces, and unlike a bit by bit method it costs the
    // same whether the input is a metre or a megametre.
    int64_t guess = 1;
    int64_t v = value;
    while (v > 1) { v >>= 2; guess <<= 1; }
    for (int i = 0; i < 12; i++) {
        const int64_t next = (guess + value / guess) >> 1;
        if (next == guess) break;
        guess = next;
    }
    while (guess * guess > value) guess--;
    while ((guess + 1) * (guess + 1) <= value) guess++;
    return guess;
}

namespace {

// Angle of a vector, in angle units, by bisection against the same sine table
// everything else here uses.
//
// Bisection rather than an arctangent table because it needs no second table
// and no octant algebra to get wrong: eleven steps of "is the angle above or
// below this" lands on the exact unit, and the only thing it can disagree with
// is sin_fp, which is the function whose answer is wanted in the first place.
int32_t atan2_units(int64_t y, int64_t x) {
    if (x == 0 && y == 0) return 0;
    const bool neg_x = x < 0, neg_y = y < 0;
    const int64_t ax = abs_l(x), ay = abs_l(y);
    int32_t lo = 0, hi = k_quarter;
    for (int i = 0; i < 11; i++) {
        const int32_t mid = (lo + hi) / 2;
        // tan(mid) against ay/ax, cross multiplied so there is no divide.
        if (ay * cos_fp(mid) - ax * sin_fp(mid) > 0) lo = mid; else hi = mid;
    }
    // lo and hi are now adjacent. Take whichever actually fits better rather
    // than always the lower: bisection can only ever narrow onto an interval,
    // and taking its floor put a vector pointing straight up at 1023 instead
    // of 1024, which is a quarter turn that is a whole angle unit short.
    const int64_t miss_lo = abs_l(ay * cos_fp(lo) - ax * sin_fp(lo));
    const int64_t miss_hi = abs_l(ay * cos_fp(hi) - ax * sin_fp(hi));
    const int32_t a = miss_hi < miss_lo ? hi : lo;
    if (neg_x) return neg_y ? 2 * k_quarter + a : 2 * k_quarter - a;
    return neg_y ? k_turn - a : a;
}

}  // namespace

// ------------------------------------------------------------------ bodies

// Picopiter and its two moons.
//
// The whole system is small on purpose. A 90 km planet pulling 8 m/s^2 puts a
// circular orbit at 767 m/s and fifteen minutes, which is a rocket a 2050 kg
// stack can reach in two stages and an orbit time warp can actually cover.
// Scale it up to anything earthlike and either the rocket cannot get there or
// the player watches a progress bar.
//
// mu is g0 * R^2 exactly, so the surface gravity quoted in each comment is the
// gravity the sim applies rather than a number someone wrote down once.
const Body k_bodies[kBodyCount] = {
    // Picopiter: 8.0 m/s^2, air to 12 km.
    {"PICOPITER", k_home_radius_m, 64800000000LL, k_home_atmo_m, 0, 0, 0, 0,
     200, 74, 122, 96},
    // Pip: 1.5 m/s^2, airless, 400 km out, round in 1 h 44 m. The reference
    // radius is a little wider than the textbook sphere of influence (61 km)
    // because it is a readout boundary rather than a physical one, and a map
    // that switches to the moon's frame slightly early is a map that tells you
    // you have arrived slightly early.
    {"PIP", 20000, 600000000LL, 0, 70000, 400000, 42938, 1500,
     400, 156, 160, 150},
    // Pom: 1.2 m/s^2, airless, 800 km out, round in 4 h 54 m.
    {"POM", 28000, 940800000LL, 0, 130000, 800000, 15201, 2900,
     700, 128, 112, 148},
};

const Stage k_stages[k_stage_count] = {
    // The booster. 33 kN under 2050 kg is 2.01 g of thrust to weight, and
    // 750 kg of propellant at 2400 m/s of exhaust is 1093 m/s of delta v,
    // against an ascent that costs about 1100.
    //
    // That is deliberately NOT enough to reach orbit on. An earlier booster
    // carried 1000 kg and flew the whole first mission by itself with 199 kg
    // to spare, which made the stage button a control with nothing to do:
    // there was no moment in the game where dropping the empty half of the
    // rocket was the thing that got you there. Now the booster runs dry with
    // the ship still climbing and the upper stage finishes the job.
    {500, 750, 33000, 2400},
    // The upper stage, and the lander. 2149 m/s, which covers the rest of the
    // ascent, circularising, the transfer, the capture and the descent with a
    // reserve for flying it badly.
    //
    // 12 kN on 800 kg is 15 m/s^2, and the thrust is set by the
    // circularisation burn rather than by the ascent. Reaching orbit means
    // adding about 500 m/s at apoapsis, and the ship is falling the whole
    // time it does: at 6 kN that burn took 90 seconds, the apoapsis ran away
    // faster than the periapsis came up, and the orbit simply never closed.
    // At 15 m/s^2 it is a 35 second burn, the ship barely drops during it, and
    // the periapsis rises to meet the apoapsis the way the picture in every
    // player's head says it should.
    //
    // It gives Pip a thrust to weight of ten, which sounds like too much for a
    // landing and is not: the throttle is continuous, a descent is flown at a
    // sixth of it, and having the authority to stop a fall late is worth more
    // on four buttons than a stage that has to be flown perfectly early.
    {400, 400, 12000, 3100},
};

// -------------------------------------------------------------- readouts

namespace {

// How much relief this stretch of ground is allowed, fp14: zero at the launch
// pad, full everywhere else.
//
// A rocket standing on a slope topples the moment the gear touches, so the
// launch site is levelled, and the blend runs out over k_pad_blend angle units
// so the flat does not end in a cliff. It is a factor rather than an early
// return because the cross plane relief has to be flattened by exactly the
// same amount: a pad that is level along the ship's track and corrugated
// across it is not a pad.
int32_t relief_scale(uint8_t body, int32_t along) {
    if (body != kPicopiter) return k_fp14;
    const int32_t off = abs_i(wrap_signed(along - k_pad_bearing));
    if (off <= k_pad_flat) return 0;
    if (off >= k_pad_blend) return k_fp14;
    return (k_fp14 * (off - k_pad_flat)) / (k_pad_blend - k_pad_flat);
}

}  // namespace

int32_t terrain_m(uint8_t body, int32_t along) {
    const Body& b = k_bodies[body];
    // Three harmonics that do not share a period, so the ground never repeats
    // over a lap. Rule 11 is about models, not about landscapes: this is a
    // function, not a mesh, and it has to be one because the collision and the
    // picture must be the same ground and a 90 km sphere is not a model.
    const int32_t h = (sin_fp(along * 3) * 3 +
                       sin_fp(along * 7 + 700) * 2 +
                       sin_fp(along * 17 + 1900)) / 6;
    // int64 for the product and for the divisor both. Three fp14 terms
    // multiplied together is 2^42 before the shift back, and the divisor here
    // is only just inside an int32 while terrain_at's is well past it: written
    // as plain ints the whole expression evaluates to a division by zero, and
    // it did, because the divisor wrapped rather than the product.
    return static_cast<int32_t>(
        (static_cast<int64_t>(h) * b.relief_m * relief_scale(body, along)) /
        (2LL * k_fp14 * k_fp14));
}

int32_t terrain_at(uint8_t body, int32_t along, int32_t across) {
    const int32_t base = terrain_m(body, along);
    if (across == 0) return base;
    const Body& b = k_bodies[body];
    // Two more harmonics, in BOTH angles so the ground is not a set of ridges
    // running parallel to the flight path, multiplied by a ramp that is zero
    // in the plane. The ramp is what keeps terrain_at(b, a, 0) exactly
    // terrain_m(b, a), and the identity has to be exact: the renderer draws
    // this and the collision uses the other one, so a ship that lands half a
    // metre above the ground it can see is a ship floating.
    const int32_t w = (sin_fp(across * 11 + along * 5) * 2 +
                       sin_fp(across * 29 - along * 13)) / 3;
    constexpr int32_t k_reach = 220;
    const int32_t reach = abs_i(across) < k_reach ? abs_i(across) : k_reach;
    return base + static_cast<int32_t>(
        (static_cast<int64_t>(w) * b.relief_m * relief_scale(body, along) *
         reach) / (4LL * k_fp14 * k_fp14 * k_reach));
}

void body_position(const World& world, uint8_t body, int32_t& x, int32_t& y) {
    const Body& b = k_bodies[body];
    if (b.orbit_m == 0) { x = 0; y = 0; return; }
    const int32_t a = world.moon_phase[body] / k_fp16;
    x = static_cast<int32_t>((static_cast<int64_t>(cos_fp(a)) * b.orbit_m) /
                             k_fp14);
    y = static_cast<int32_t>((static_cast<int64_t>(sin_fp(a)) * b.orbit_m) /
                             k_fp14);
}

void body_velocity(const World& world, uint8_t body, int32_t& vx, int32_t& vy) {
    const Body& b = k_bodies[body];
    if (b.orbit_m == 0) { vx = 0; vy = 0; return; }
    const int32_t a = world.moon_phase[body] / k_fp16;
    // v = omega * r, tangential. omega is carried as fp16 ANGLE units per
    // second, so it becomes radians per second by 2 pi / k_turn, and the
    // constant below is k_fp16 * k_turn / (2 pi) rounded: 42,724,000. Dividing
    // by it converts an angle rate straight into a speed at unit radius.
    constexpr int64_t k_rate_to_speed = 42724000LL;
    const int64_t speed = (static_cast<int64_t>(b.rate_fp16) * b.orbit_m *
                           k_fp16) / k_rate_to_speed;
    vx = static_cast<int32_t>((-speed * sin_fp(a)) / k_fp14);
    vy = static_cast<int32_t>((speed * cos_fp(a)) / k_fp14);
}

namespace {

// The ship relative to a body: offset in metres, velocity in fp16 m/s.
void relative_to(const World& w, uint8_t body,
                 int32_t& rx, int32_t& ry, int32_t& vx, int32_t& vy) {
    int32_t bx, by, bvx, bvy;
    body_position(w, body, bx, by);
    body_velocity(w, body, bvx, bvy);
    rx = static_cast<int32_t>((w.x >> 16) - bx);
    ry = static_cast<int32_t>((w.y >> 16) - by);
    vx = w.vx - bvx;
    vy = w.vy - bvy;
}

int32_t range_to(const World& w, uint8_t body) {
    int32_t rx, ry, vx, vy;
    relative_to(w, body, rx, ry, vx, vy);
    return static_cast<int32_t>(isqrt64(static_cast<int64_t>(rx) * rx +
                                        static_cast<int64_t>(ry) * ry));
}

}  // namespace

uint8_t reference_body(const World& world) {
    for (uint8_t b = 1; b < kBodyCount; b++) {
        if (range_to(world, b) < k_bodies[b].ref_m) return b;
    }
    return kPicopiter;
}

int32_t bearing_from(const World& world, uint8_t body) {
    int32_t rx, ry, vx, vy;
    relative_to(world, body, rx, ry, vx, vy);
    return atan2_units(ry, rx);
}

int32_t stand_m(const World& world) {
    return world.stage == 0 ? k_stack_m : k_gear_m;
}

int32_t clearance_m(const World& world) {
    const int32_t alt = altitude_m(world);
    const int32_t stand = stand_m(world);
    return alt > stand ? alt - stand : 0;
}

int32_t altitude_m(const World& world) {
    const uint8_t ref = world.ref_body;
    const int32_t r = range_to(world, ref);
    const int32_t floor_m =
        k_bodies[ref].radius_m + terrain_m(ref, bearing_from(world, ref));
    return r > floor_m ? r - floor_m : 0;
}

int32_t speed_fp16(const World& world) {
    int32_t rx, ry, vx, vy;
    relative_to(world, world.ref_body, rx, ry, vx, vy);
    return static_cast<int32_t>(isqrt64(static_cast<int64_t>(vx) * vx +
                                        static_cast<int64_t>(vy) * vy));
}

void prograde(const World& world, int32_t& ux, int32_t& uy) {
    int32_t rx, ry, vx, vy;
    relative_to(world, world.ref_body, rx, ry, vx, vy);
    const int64_t mag = isqrt64(static_cast<int64_t>(vx) * vx +
                                static_cast<int64_t>(vy) * vy);
    if (mag == 0) { ux = 0; uy = 0; return; }
    ux = static_cast<int32_t>((static_cast<int64_t>(vx) * k_fp14) / mag);
    uy = static_cast<int32_t>((static_cast<int64_t>(vy) * k_fp14) / mag);
}

int32_t prograde_angle(const World& world) {
    int32_t rx, ry, vx, vy;
    relative_to(world, world.ref_body, rx, ry, vx, vy);
    return atan2_units(vy, vx);
}

int32_t mass_fp8(const World& world) {
    int32_t kg = world.fuel_kg;
    for (int i = world.stage; i < k_stage_count; i++) {
        kg += k_stages[i].dry_kg * k_fp8;
        if (i > world.stage) kg += k_stages[i].fuel_kg * k_fp8;
    }
    return kg;
}

int32_t thrust_n(const World& world) {
    if (world.fuel_kg <= 0 || world.throttle == 0) return 0;
    return static_cast<int32_t>(
        (static_cast<int64_t>(k_stages[world.stage].thrust_n) *
         world.throttle) / 255);
}

bool out_of_fuel(const World& world) {
    return world.stage + 1 >= k_stage_count && world.fuel_kg <= 0;
}

bool warp_allowed(const World& world) {
    if (!flying(world) || world.grounded || world.throttle > 0) return false;
    const uint8_t ref = world.ref_body;
    const int32_t alt = altitude_m(world);
    const int32_t floor_m = k_bodies[ref].atmo_m > 0 ? k_bodies[ref].atmo_m
                                                     : k_warp_floor_m;
    return alt > floor_m;
}

Elements elements(const World& world) {
    Elements out{};
    const uint8_t ref = world.ref_body;
    out.ref = ref;

    int32_t rx, ry, vx, vy;
    relative_to(world, ref, rx, ry, vx, vy);
    const int64_t r = isqrt64(static_cast<int64_t>(rx) * rx +
                              static_cast<int64_t>(ry) * ry);
    if (r <= 0) { out.closed = false; return out; }

    const int64_t mu = k_bodies[ref].mu;

    // Specific energy, carried in fp8 of (m/s)^2. The extra eight bits matter:
    // a circular orbit sits where the kinetic and potential terms very nearly
    // cancel, so at whole units the difference between them is a few hundred
    // parts in a hundred thousand and the semi major axis it implies wobbles
    // by kilometres frame to frame.
    const int64_t v2_fp8 = (static_cast<int64_t>(vx) * vx +
                            static_cast<int64_t>(vy) * vy) >> 24;
    const int64_t eps_fp8 = v2_fp8 / 2 - (mu * k_fp8) / r;

    // Angular momentum, m^2/s, and from it the semi latus rectum. This half is
    // computed whether the orbit closes or not, because the shape of an escape
    // trajectory is exactly as drawable as the shape of an orbit and the map
    // has to draw both.
    const int64_t h = ((static_cast<int64_t>(rx) * vy -
                        static_cast<int64_t>(ry) * vx)) >> 16;
    const int64_t p = (h * h) / mu;
    out.semi_latus_m = static_cast<int32_t>(clamp_l(p, 0, 0x7FFFFFFF));

    // Which way periapsis lies: the direction of the eccentricity vector,
    // e = ((v^2 - mu/r) r - (r.v) v) / mu. The 1/mu is dropped because only
    // the direction is wanted, which also keeps the whole thing in integers.
    {
        const int64_t v2 = v2_fp8 / k_fp8;
        const int64_t rv = (static_cast<int64_t>(rx) * vx +
                            static_cast<int64_t>(ry) * vy) >> 16;
        const int64_t k = v2 - mu / r;
        out.peri_angle = atan2_units(
            k * ry - rv * (static_cast<int64_t>(vy) >> 16),
            k * rx - rv * (static_cast<int64_t>(vx) >> 16));
    }

    if (eps_fp8 >= 0 || (mu * k_fp8) / (-2 * eps_fp8) > 4LL * k_lost_m) {
        // Open, or so nearly open that calling it an ellipse would be a
        // number the HUD made up. e still has a meaning and the map still
        // draws it: for a hyperbola a is negative, so e comes off the energy
        // rather than off a.
        out.closed = false;
        // e^2 = 1 + 2 eps h^2 / mu^2, formed with the h^2 that is already p*mu
        // so nothing has to hold h^2 and mu^2 at once.
        const int64_t term = (2 * eps_fp8 * p) / (mu / k_fp8 * 2);
        const int64_t e2_fp16 = static_cast<int64_t>(k_fp16) +
                                (term * k_fp16) / (k_fp8 * k_fp8 / 2);
        out.ecc_fp16 = static_cast<int32_t>(
            isqrt64(clamp_l(e2_fp16, 0, 0x7FFFFFFF) *
                    static_cast<int64_t>(k_fp16)));
        return out;
    }

    const int64_t a = (mu * k_fp8) / (-2 * eps_fp8);

    // Apoapsis and periapsis are the roots of t^2 - 2at + ap = 0, which is
    // eccentricity without ever forming eccentricity: e is a ratio near 1 for
    // a circular orbit and squaring it loses exactly the digits that matter.
    int64_t disc = a * (a - p);
    if (disc < 0) disc = 0;
    const int64_t root = isqrt64(disc);

    out.apoapsis_m = static_cast<int32_t>(clamp_l(a + root, 0, k_lost_m));
    out.periapsis_m = static_cast<int32_t>(clamp_l(a - root, 0, k_lost_m));
    // e = (ap - pe) / (ap + pe), from the two ends rather than from the
    // energy: the same subtraction that already survived being turned into a
    // radius, so the ellipse the map draws passes through the apsides the HUD
    // is printing.
    const int64_t sum = static_cast<int64_t>(out.apoapsis_m) + out.periapsis_m;
    out.ecc_fp16 = sum > 0 ? static_cast<int32_t>(
        ((static_cast<int64_t>(out.apoapsis_m) - out.periapsis_m) * k_fp16) /
        sum) : 0;
    out.closed = true;
    return out;
}

bool burn_window(const World& world) {
    const uint8_t target = target_body(world);
    if (target == kPicopiter) return false;
    if (world.ref_body != kPicopiter) return false;

    const Elements el = elements(world);
    if (!el.closed) return false;

    const int32_t r1 = range_to(world, kPicopiter);
    const int32_t r2 = k_bodies[target].orbit_m;
    if (r1 >= r2) return false;

    // How long a Hohmann transfer to that orbit takes, in seconds: half the
    // period of an ellipse whose semi major axis is the mean of the two radii.
    const int64_t at = (static_cast<int64_t>(r1) + r2) / 2;
    const int64_t t_half = (isqrt64((at * at * at) / k_bodies[kPicopiter].mu) *
                            355) / 113;

    // Where the moon has to be when the burn starts: half a turn ahead of the
    // ship, less however far the moon travels while the ship is on its way.
    const int64_t moon_travel =
        (static_cast<int64_t>(k_bodies[target].rate_fp16) * t_half) / k_fp16;
    const int32_t want = wrap_turn(static_cast<int32_t>(k_turn / 2 -
                                                        moon_travel));

    const int32_t moon_bearing = world.moon_phase[target] / k_fp16;
    const int32_t ship_bearing = bearing_from(world, kPicopiter);
    const int32_t lead = wrap_turn(moon_bearing - ship_bearing);
    return abs_i(wrap_signed(lead - want)) <= k_window_tol;
}

// ------------------------------------------------------------------- init

void world_init(World& world, Mission mission) {
    world = World{};
    world.mission = mission;
    world.state = Flight::Flying;
    world.fault = Fault::None;
    world.stage = 0;
    world.fuel_kg = k_stages[0].fuel_kg * k_fp8;
    world.hold = Hold::Off;
    world.landed_on = kPicopiter;
    world.grounded = true;
    world.ref_body = kPicopiter;

    // On the pad: at the top of the planet, nose straight up. The pad apron is
    // levelled by terrain_m, so this is the exact surface height there, and
    // the rocket stands on its engine bell rather than on legs it has not
    // deployed yet.
    const int64_t stand =
        static_cast<int64_t>(k_home_radius_m + terrain_m(kPicopiter,
                                                          k_pad_bearing) +
                             k_stack_m);
    world.x = 0;
    world.y = stand << 16;
    world.angle = k_pad_bearing * k_fp16;

    for (uint8_t b = 1; b < kBodyCount; b++) {
        world.moon_phase[b] = k_bodies[b].start_fp16 * k_fp16;
    }
}

// ------------------------------------------------------------------- tick

namespace {

// One body's pull on a point, in fp16 m/s of velocity change over dt,
// accumulated with k_grav_extra spare bits by the caller.
void gravity_from(const World& w, uint8_t body, uint32_t dt_ms,
                  int64_t& out_x, int64_t& out_y) {
    int32_t bx, by;
    body_position(w, body, bx, by);
    const int32_t rx = static_cast<int32_t>((w.x >> 16) - bx);
    const int32_t ry = static_cast<int32_t>((w.y >> 16) - by);
    const int64_t r2 = static_cast<int64_t>(rx) * rx +
                       static_cast<int64_t>(ry) * ry;
    if (r2 <= 0) return;
    const int64_t r = isqrt64(r2);
    if (r < 1) return;

    // mu * dt / r^3, split so nothing overflows: the first divide brings the
    // numerator back under control before it meets the offset.
    const int64_t scale =
        ((k_bodies[body].mu * dt_ms) / 1000) * (k_fp16 << k_grav_extra) / r;
    out_x -= (scale * rx) / r2;
    out_y -= (scale * ry) / r2;
}

// Where the ship rests on a body: mean radius, plus the terrain under it, plus
// whatever the ship is standing on. One function, so the spawn, the collision
// and the altitude readout cannot disagree about where the floor is.
int32_t floor_at(const World& w, uint8_t body, int32_t bearing) {
    return k_bodies[body].radius_m + terrain_m(body, bearing) + stand_m(w);
}

// Put the ship exactly on a body's floor, keeping the direction it is already
// in rather than rebuilding it from an angle.
//
// Scaling the offset vector is not a micro optimisation over cos and sin of a
// re derived bearing, it is the difference between resting and sinking: the
// bearing comes back quantised to an angle unit, and a rocket standing at the
// pole came out at sin(1023) rather than sin(1024), which put it 3.7 m under
// its own pad and left it there. Direction is what the ship has; there is no
// reason to turn it into an angle and back.
void snap_to_floor(World& w, uint8_t body, int32_t rx, int32_t ry,
                   int64_t r, int64_t rest) {
    if (r <= 0) return;
    int32_t bx, by;
    body_position(w, body, bx, by);
    w.x = (static_cast<int64_t>(bx) << 16) +
          (static_cast<int64_t>(rx) * k_fp16 * rest) / r;
    w.y = (static_cast<int64_t>(by) << 16) +
          (static_cast<int64_t>(ry) * k_fp16 * rest) / r;
}

void end_flight(World& w, Flight state, Fault fault) {
    w.state = state;
    w.fault = fault;
    w.ticks_in_state = 0;
    w.throttle = 0;
    w.warp_step = 0;
    w.spin = 0;
}

// Contact with a body, once the ship has reached its floor. Judges by relative
// speed and by lean, and both edges are the ones the HUD colours by.
void touch_down(World& w, uint8_t body) {
    int32_t rx, ry, vx, vy;
    relative_to(w, body, rx, ry, vx, vy);
    const int64_t r = isqrt64(static_cast<int64_t>(rx) * rx +
                              static_cast<int64_t>(ry) * ry);
    if (r <= 0) return;

    const int32_t speed =
        static_cast<int32_t>(isqrt64(static_cast<int64_t>(vx) * vx +
                                     static_cast<int64_t>(vy) * vy));
    w.touch_speed = speed;

    // The local vertical, and how far the nose is from it. A ship that comes
    // down fast enough is a wreck whatever its attitude; a ship that comes
    // down gently on its side is a wreck too, and it is worth saying which.
    const int32_t up_x = static_cast<int32_t>((rx * static_cast<int64_t>(k_fp14)) / r);
    const int32_t up_y = static_cast<int32_t>((ry * static_cast<int64_t>(k_fp14)) / r);
    const int32_t a = w.angle / k_fp16;
    const int32_t lean = k_fp14 - static_cast<int32_t>(
        (static_cast<int64_t>(cos_fp(a)) * up_x +
         static_cast<int64_t>(sin_fp(a)) * up_y) / k_fp14);

    if (touchdown_band(speed) == Touchdown::Fatal) {
        end_flight(w, Flight::Crashed, Fault::Impact);
        return;
    }
    if (lean > k_safe_tilt) {
        end_flight(w, Flight::Crashed, Fault::Toppled);
        return;
    }

    // Down safe. Park the ship exactly on its floor with the relative motion
    // taken out, so it sits rather than creeping.
    int32_t bvx, bvy;
    body_velocity(w, body, bvx, bvy);
    snap_to_floor(w, body, rx, ry, r, floor_at(w, body, atan2_units(ry, rx)));
    w.vx = bvx;
    w.vy = bvy;
    w.grounded = true;
    w.landed_on = body;
    w.spin = 0;

    if (body == target_body(w) && w.mission != Mission::Orbit) {
        end_flight(w, Flight::Complete, Fault::None);
    } else {
        // Down safe, and not where the mission asked. Nothing here is a
        // failure to fly: it is a flight that ended somewhere else, and the
        // card says so rather than calling it a crash.
        end_flight(w, Flight::Landed, Fault::None);
    }
}

void step_debris(World& w, uint32_t dt_ms) {
    if (!w.debris) return;
    const int32_t rx = static_cast<int32_t>(w.dx >> 16);
    const int32_t ry = static_cast<int32_t>(w.dy >> 16);
    const int64_t r2 = static_cast<int64_t>(rx) * rx +
                       static_cast<int64_t>(ry) * ry;
    const int64_t r = isqrt64(r2);
    if (r > 1) {
        const int64_t scale =
            ((k_bodies[kPicopiter].mu * dt_ms) / 1000) * k_fp16 / r;
        w.dvx -= static_cast<int32_t>((scale * rx) / r2);
        w.dvy -= static_cast<int32_t>((scale * ry) / r2);
    }
    w.dx += (static_cast<int64_t>(w.dvx) * dt_ms) / 1000;
    w.dy += (static_cast<int64_t>(w.dvy) * dt_ms) / 1000;
    w.debris_angle = wrap_turn((w.debris_angle +
                                static_cast<int32_t>(
                                    (static_cast<int64_t>(w.debris_spin) *
                                     dt_ms) / 1000)) % k_turn);
    // It stops being scenery when it is inside the planet or a long way off.
    if (r < k_home_radius_m || r > k_lost_m) w.debris = false;
}

void do_stage(World& w) {
    if (w.stage + 1 >= k_stage_count) return;

    // The booster keeps whatever the ship had and takes a shove backward, so
    // it falls away down screen instead of sitting inside the upper stage.
    w.debris = true;
    w.dx = w.x;
    w.dy = w.y;
    const int32_t a = w.angle / k_fp16;
    constexpr int64_t k_kick = 3 * k_fp16;      // 3 m/s of separation
    w.dvx = w.vx - static_cast<int32_t>((cos_fp(a) * k_kick) / k_fp14);
    w.dvy = w.vy - static_cast<int32_t>((sin_fp(a) * k_kick) / k_fp14);
    w.debris_angle = a;
    w.debris_spin = 40;                          // angle units per second

    w.stage++;
    w.fuel_kg = k_stages[w.stage].fuel_kg * k_fp8;
    w.dry_ms = 0;
    if (w.stage > w.peak_stage) w.peak_stage = w.stage;
}

}  // namespace

void world_tick(World& world, const Input& input) {
    World& w = world;
    if (w.state != Flight::Flying) {
        w.ticks_in_state++;
        return;
    }
    w.ticks_in_state++;

    // ---- edges ----
    if (input.stage) do_stage(w);
    if (input.hold) {
        w.hold = static_cast<Hold>((static_cast<uint8_t>(w.hold) + 1) %
                                   static_cast<uint8_t>(Hold::kCount));
    }
    if (input.warp) {
        w.warp_step = warp_allowed(w)
                          ? static_cast<uint8_t>((w.warp_step + 1) %
                                                 k_warp_count)
                          : 0;
    }
    // Warp survives only while it is legal. Touching the throttle, entering
    // the air or touching down all drop it, which is the only sane rule: a
    // two second step is not something a burn or an atmosphere can be flown
    // through.
    if (w.warp_step != 0 && (!warp_allowed(w) || input.up || input.down)) {
        w.warp_step = 0;
    }

    const uint32_t dt_ms = k_tick_ms * warp_factor(w);
    w.tick++;
    w.mission_ms += dt_ms;

    // ---- moons on rails ----
    for (uint8_t b = 1; b < kBodyCount; b++) {
        w.moon_phase[b] = static_cast<int32_t>(
            (w.moon_phase[b] +
             (static_cast<int64_t>(k_bodies[b].rate_fp16) * dt_ms) / 1000) %
            (static_cast<int64_t>(k_turn) * k_fp16));
    }

    // ---- throttle ----
    if (w.warp_step == 0) {
        // Full travel in half a second, which is quick enough to answer a
        // landing and slow enough to set a number by eye.
        const int32_t move = static_cast<int32_t>((255 * dt_ms) / 500);
        int32_t t = w.throttle;
        if (input.up) t += move > 0 ? move : 1;
        if (input.down) t -= move > 0 ? move : 1;
        w.throttle = static_cast<uint8_t>(clamp_i(t, 0, 255));
    } else {
        w.throttle = 0;
    }

    // ---- attitude ----
    // Peak turn rate in fp16 angle units per second. Formed in an int64
    // because the obvious spelling, k_turn * k_turn_rate * k_fp16, is 1.3e10
    // and overflows an int32 before the divide ever runs.
    const int32_t peak = static_cast<int32_t>(
        (static_cast<int64_t>(k_turn) * k_turn_rate * k_fp16) / 360);
    int32_t want_spin = 0;
    if (input.left) want_spin += peak;
    if (input.right) want_spin -= peak;
    if (want_spin == 0 && w.hold != Hold::Off && !w.grounded) {
        // Point at the velocity, or the other way. The error closes over about
        // a third of a second, capped at the same rate a thumb gets.
        int32_t want_angle = prograde_angle(w);
        if (w.hold == Hold::Retrograde) want_angle += k_turn / 2;
        const int32_t err = wrap_signed(want_angle - w.angle / k_fp16);
        want_spin = clamp_i(err * k_fp16 * 3, -peak, peak);
    }
    const int32_t ramp = static_cast<int32_t>(
        (static_cast<int64_t>(peak) * dt_ms) / k_turn_ramp_ms);
    if (w.spin < want_spin) w.spin = clamp_i(w.spin + ramp, w.spin, want_spin);
    if (w.spin > want_spin) w.spin = clamp_i(w.spin - ramp, want_spin, w.spin);
    w.angle = static_cast<int32_t>(
        (w.angle + (static_cast<int64_t>(w.spin) * dt_ms) / 1000) %
        (static_cast<int64_t>(k_turn) * k_fp16));
    if (w.angle < 0) w.angle += k_turn * k_fp16;

    // ---- thrust ----
    const int32_t force = thrust_n(w);
    const int32_t mass = mass_fp8(w);
    if (force > 0) {
        const int32_t a = w.angle / k_fp16;
        const int64_t dv = (static_cast<int64_t>(force) * dt_ms * k_fp8 *
                            k_fp16) / (1000LL * mass);
        w.vx += static_cast<int32_t>((dv * cos_fp(a)) / k_fp14);
        w.vy += static_cast<int32_t>((dv * sin_fp(a)) / k_fp14);

        const int32_t burn = static_cast<int32_t>(
            (static_cast<int64_t>(force) * dt_ms * k_fp8) /
            (1000LL * k_stages[w.stage].exhaust_ms));
        const int32_t spent = burn < w.fuel_kg ? burn : w.fuel_kg;
        w.fuel_kg -= spent;
        w.fuel_used += spent;
        if (w.fuel_kg <= 0) w.fuel_kg = 0;
    }

    // ---- gravity, from every body, every tick ----
    int64_t gx = 0, gy = 0;
    for (uint8_t b = 0; b < kBodyCount; b++) gravity_from(w, b, dt_ms, gx, gy);
    w.vx += static_cast<int32_t>(gx >> k_grav_extra);
    w.vy += static_cast<int32_t>(gy >> k_grav_extra);

    // ---- air ----
    const int32_t home_r = range_to(w, kPicopiter);
    const int32_t home_alt = home_r - k_home_radius_m;
    if (home_alt < k_home_atmo_m) {
        const int32_t rho = density256(home_alt);
        if (rho > 0) {
            const int64_t v_mag = isqrt64(static_cast<int64_t>(w.vx) * w.vx +
                                          static_cast<int64_t>(w.vy) * w.vy);
            const int64_t v2 = (v_mag * v_mag) >> 32;      // (m/s)^2
            if (v_mag > 0 && v2 > 0) {
                const int64_t accel =
                    (static_cast<int64_t>(rho) * v2 * k_fp16) /
                    (static_cast<int64_t>(mass / k_fp8) * k_drag_div);
                int64_t dv = (accel * dt_ms) / 1000;
                // Drag removes speed. It must never add any in the other
                // direction, which one long step at high speed would do.
                if (dv > v_mag) dv = v_mag;
                w.vx -= static_cast<int32_t>((dv * w.vx) / v_mag);
                w.vy -= static_cast<int32_t>((dv * w.vy) / v_mag);
            }
        }
    }

    // ---- move ----
    w.x += (static_cast<int64_t>(w.vx) * dt_ms) / 1000;
    w.y += (static_cast<int64_t>(w.vy) * dt_ms) / 1000;
    step_debris(w, dt_ms);

    w.ref_body = reference_body(w);

    // ---- the ground ----
    //
    // Checked against every body rather than just the reference one. The
    // reference radius is a readout boundary and nothing more, so a ship on a
    // shallow trajectory can be outside a moon's reference radius and still on
    // course to hit the moon, and a collision test that only looked at the
    // reference body would let it pass straight through.
    bool touching = false;
    for (uint8_t b = 0; b < kBodyCount; b++) {
        int32_t rx, ry, vx, vy;
        relative_to(w, b, rx, ry, vx, vy);
        const int64_t r = isqrt64(static_cast<int64_t>(rx) * rx +
                                  static_cast<int64_t>(ry) * ry);
        // The cheap test first. Working out WHICH bit of ground is under the
        // ship costs an arctangent, and doing that for three bodies every tick
        // when two of them are hundreds of kilometres away is the sort of cost
        // that only shows up on the device.
        if (r > k_bodies[b].radius_m + k_bodies[b].relief_m + stand_m(w)) {
            continue;
        }
        const int32_t rest = floor_at(w, b, atan2_units(ry, rx));
        if (r > rest) continue;
        touching = true;

        // Radial motion decides what this is, and it has to be asked FIRST.
        // On the tick a rocket leaves its pad it is still below its own resting
        // height and already moving up, and a test that only asked "is the ship
        // grounded" read that as an arrival: the launch was scored as a landing
        // on the pad it had just left, one tick after the engine lit.
        const int64_t out = static_cast<int64_t>(vx) * rx +
                            static_cast<int64_t>(vy) * ry;
        if (out > 0) {
            // Leaving. The position is left exactly where the step put it and
            // is NOT pulled back to the floor: the offsets here are whole
            // metres, a rocket's first tick off the pad lifts it well under a
            // millimetre, and snapping it back to the surface every tick threw
            // that away as fast as it accumulated. The ship burned its whole
            // booster sitting at zero altitude.
            w.grounded = false;
            break;
        }

        if (w.grounded && w.landed_on == b) {
            // Sitting on it, with the ground holding it up.
            int32_t bvx, bvy;
            body_velocity(w, b, bvx, bvy);
            snap_to_floor(w, b, rx, ry, r, rest);
            w.vx = bvx;
            w.vy = bvy;
            break;
        }
        touch_down(w, b);
        break;
    }

    // Nothing is under it any more, so it is not sitting on anything. This is
    // not the same as the lift off case above and it is why the flag needs
    // clearing here as well: staging CHANGES what the ship stands on, from an
    // engine bell to a set of legs six metres shorter, and a rocket that
    // separates on the pad is instantly above its own resting height without
    // ever having moved. It stayed "grounded" through the whole flight that
    // followed, which kept the throttle from ever counting as a lift off.
    if (w.grounded && !touching) {
        w.grounded = false;
    }

    if (w.state != Flight::Flying) return;

    // ---- did the mission just finish? ----
    if (w.mission == Mission::Orbit && w.ref_body == kPicopiter) {
        const Elements el = elements(w);
        const int32_t clear = k_home_radius_m + k_home_atmo_m +
                              k_orbit_margin_m;
        if (el.closed && el.periapsis_m >= clear && el.apoapsis_m >= clear) {
            end_flight(w, Flight::Complete, Fault::None);
            return;
        }
    }

    // ---- is it over? ----
    if (home_r > k_lost_m) {
        end_flight(w, Flight::Stranded, Fault::Lost);
        return;
    }
    if (out_of_fuel(w)) {
        w.dry_ms += dt_ms;
        // An empty tank ends a flight only once the trajectory has stopped
        // going anywhere: a ship falling toward a landing with nothing left
        // still gets to try, and one on an orbit that never touches anything
        // is finished whatever it does next.
        const Elements el = elements(w);
        const int32_t surface = k_bodies[w.ref_body].radius_m;
        const bool coming_down = !el.closed || el.periapsis_m <= surface;
        if (!coming_down && w.dry_ms > k_dry_grace_ms) {
            end_flight(w, Flight::Stranded, Fault::Dry);
            return;
        }
    } else {
        w.dry_ms = 0;
    }
}

}  // namespace ps
