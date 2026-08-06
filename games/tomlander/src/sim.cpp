#include "sim.hpp"

namespace tl {
namespace {

// A quarter of a sine, every 8 angle units of the 4096 unit turn, in fp14.
// 129 entries so the last one is exactly k_trig_one and the interpolation
// below never has to special case the top of the range. 258 bytes of flash.
// Linear interpolation between entries is worth 9e-05 at its worst, which is
// four orders of magnitude finer than anything here can see.
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
    if (i >= 128) return k_trig_one;
    const int32_t lo = k_sin_quarter[i];
    return lo + (((k_sin_quarter[i + 1] - lo) * f) >> 3);
}

int32_t clamp_i(int32_t v, int32_t lo, int32_t hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

// v -= v >> shift, done so it works for negative v too. A plain arithmetic
// shift of a negative rounds toward negative infinity, so the naive version
// never quite lets a leftward drift die and the ship creeps forever.
int32_t damp(int32_t v) {
    return v - (v >= 0 ? (v >> k_drag_shift) : -((-v) >> k_drag_shift));
}

}  // namespace

int32_t sin_fp(int32_t angle) {
    int32_t a = angle % k_turn;
    if (a < 0) a += k_turn;
    if (a <= k_quarter) return sin_quarter(a);
    if (a <= 2 * k_quarter) return sin_quarter(2 * k_quarter - a);
    if (a <= 3 * k_quarter) return -sin_quarter(a - 2 * k_quarter);
    return -sin_quarter(4 * k_quarter - a);
}

int32_t cos_fp(int32_t angle) { return sin_fp(angle + k_quarter); }

// ---------------------------------------------------------------- terrain

namespace {

// One wave of the landscape. `shift` sets the wavelength: the coordinate is
// fp16, so shifting it down by 10 gives one full cycle every 64 world units.
int32_t wave(int32_t coord, int32_t amp, int32_t shift) {
    // fp14 sine, times whole units of amplitude, into fp16: 65536 >> 14 is 4.
    return sin_fp(coord >> shift) * amp * 4;
}

int32_t terrain_raw(int32_t x, int32_t z) {
    return wave(x, k_hill_a1, k_hill_s1) +
           wave(z + (1 << 20), k_hill_a2, k_hill_s2) +
           wave(x + z, k_hill_a3, k_hill_s3);
}

int32_t iabs(int32_t v) { return v < 0 ? -v : v; }

}  // namespace

int32_t terrain_height(const World& world, int32_t x, int32_t z) {
    int32_t h = terrain_raw(x, z);
    for (int i = 0; i < k_pad_count; i++) {
        const Pad& pad = world.pads[i];
        // Chebyshev distance, not the hypotenuse. No square root, and the pad
        // is a square, so a square apron is the honest shape for it anyway.
        const int32_t dx = iabs(x - pad.x);
        const int32_t dz = iabs(z - pad.z);
        const int32_t d = dx > dz ? dx : dz;
        if (d >= 2 * k_pad_flat) continue;

        // Dead flat out to k_pad_flat, then a smoothstep back to the hills
        // over the same distance again, so the plateau has a skirt.
        int32_t t = 0;
        if (d > k_pad_flat) {
            t = static_cast<int32_t>(
                (static_cast<int64_t>(d - k_pad_flat) << 8) / k_pad_flat);
            t = clamp_i(t, 0, 256);
        }
        const int32_t s = (t * t * (768 - 2 * t)) >> 16;    // fp8 smoothstep
        h = static_cast<int32_t>(
            (static_cast<int64_t>(h) * s +
             static_cast<int64_t>(pad.y) * (256 - s)) >> 8);
    }
    return h;
}

int pad_at(const World& world, int32_t x, int32_t z) {
    for (int i = 0; i < k_pad_count; i++) {
        if (iabs(x - world.pads[i].x) <= k_pad_half &&
            iabs(z - world.pads[i].z) <= k_pad_half) {
            return i;
        }
    }
    return -1;
}

int32_t ground_at(const World& world, int32_t x, int32_t z) {
    const int pad = pad_at(world, x, z);
    const int32_t floor_y = pad >= 0 ? world.pads[pad].y + k_pad_rise
                                     : terrain_height(world, x, z);
    return floor_y + k_rest_height;
}

int32_t range_to_target(const World& world) {
    const Pad& pad = world.pads[world.target];
    const int32_t dx = iabs(pad.x - world.x) >> 16;
    const int32_t dz = iabs(pad.z - world.z) >> 16;
    // Octagonal approximation of the hypotenuse, good to about 4%, which is
    // plenty for a two digit readout and costs no square root.
    const int32_t big = dx > dz ? dx : dz;
    const int32_t small = dx > dz ? dz : dx;
    return big + (small >> 1) - (big >> 3);
}

void hull_up(const World& world, int32_t& ux, int32_t& uy, int32_t& uz) {
    // The hull's own +y, turned into the world. One line, and correct at
    // every attitude, because the attitude is an orientation rather than a
    // recipe for reaching one. The two angle version this replaced had to
    // spell out which way each angle tipped the vector, and got the sign of
    // one of them wrong against the renderer for as long as it existed.
    pse::quat_rotate(world.q, 0, pse::k_quat_one, 0, ux, uy, uz);
}

void up_in_hull(const World& world, int32_t& bx, int32_t& by, int32_t& bz) {
    pse::quat_unrotate(world.q, 0, pse::k_quat_one, 0, bx, by, bz);
}

// ------------------------------------------------------------------ world

void world_init(World& world) {
    world = World{};
    world.q = pse::quat_identity();

    // Left shift of a negative is undefined, so these multiply. The compiler
    // folds them either way; the warning is the only difference that matters.
    world.pads[0] = Pad{-34 * 65536, -8 * 65536, 0};
    world.pads[1] = Pad{ 32 * 65536, 22 * 65536, 0};
    // The apron sits at whatever the untouched landscape does there, so a pad
    // looks like it was built on the hill rather than punched through it.
    for (int i = 0; i < k_pad_count; i++) {
        world.pads[i].y = terrain_raw(world.pads[i].x, world.pads[i].z);
    }

    world.target = 1;
    world.landed_on = 0;
    world.grounded = true;
    world.state = Flight::Flying;
    world.fault = Fault::None;
    world.fuel = k_fuel_full;

    // Mission one starts ON pad A, not falling toward it. The first thing a
    // player does is take off, which is also the first thing they need to
    // learn.
    world.x = world.pads[0].x;
    world.z = world.pads[0].z;
    world.y = ground_at(world, world.x, world.z);
}

namespace {

// What each pod is doing this tick, 0..255.
void resolve_throttle(const World& world, const Input& input,
                      uint8_t out[kPodCount]) {
    if (input.level) {
        // A PD controller run through the pods, reading the world's up vector
        // in the hull's own frame. Level and it is (0, 1, 0); tip the hull and
        // it leans, and which way it leans names the pod that will fix it.
        //
        //   bz > 0  the nose is up      pull it down with the BACK pod
        //   bx > 0  the right side is up   pull it down with the LEFT pod
        //
        // The rate terms carry opposite signs, and that is real rather than a
        // slip: nose up is a NEGATIVE rotation about the hull's x, while right
        // side up is a POSITIVE one about its z, so damping them means
        // subtracting one rate and adding the other. test_sim checks both
        // halves converge, because getting one wrong makes that axis run away
        // and the hull levels itself over onto its back.
        int32_t bx, by, bz;
        up_in_hull(world, bx, by, bz);
        const int32_t cp = (k_level_kp * bz - k_level_kd * world.wx) >> 12;
        const int32_t cr = (k_level_kp * bx + k_level_kd * world.wz) >> 12;
        out[kPodBack]  = static_cast<uint8_t>(clamp_i(k_level_base + cp, 0, 255));
        out[kPodFront] = static_cast<uint8_t>(clamp_i(k_level_base - cp, 0, 255));
        out[kPodLeft]  = static_cast<uint8_t>(clamp_i(k_level_base + cr, 0, 255));
        out[kPodRight] = static_cast<uint8_t>(clamp_i(k_level_base - cr, 0, 255));
        return;
    }
    for (int i = 0; i < kPodCount; i++) out[i] = input.pod[i] ? 255 : 0;
}

}  // namespace

void world_tick(World& world, const Input& input) {
    world.tick++;
    if (world.state != Flight::Flying) {
        world.ticks_in_state++;
        for (int i = 0; i < kPodCount; i++) world.throttle[i] = 0;
        return;
    }

    resolve_throttle(world, input, world.throttle);
    if (world.fuel <= 0) {
        for (int i = 0; i < kPodCount; i++) world.throttle[i] = 0;
    }

    int32_t total = 0;
    for (int i = 0; i < kPodCount; i++) total += world.throttle[i];

    // ---- fuel ----
    // The original drains a flat 13/s the moment ANY thruster has power,
    // throttle ignored. That suits a one engine moon lander and is wrong for
    // four independent pods, so this is proportional: all four at full costs
    // the original's rate and one pod costs a quarter of it.
    if (total > 0) {
        const int32_t burn = (k_fuel_burn * total) / (255 * kPodCount);
        const int32_t spent = burn > 0 ? burn : 1;
        world.fuel -= spent;
        world.fuel_used += static_cast<uint32_t>(spent);
        if (world.fuel < 0) world.fuel = 0;
    }

    // ---- linear ----
    // All four pods point along the hull's own up vector, so the sum is one
    // force in that direction. Tilt the hull and you go sideways: that is the
    // entire flight model.
    int32_t ux, uy, uz;
    hull_up(world, ux, uy, uz);
    const int32_t push = (k_pod_thrust * total) / 255;
    world.vx += (ux * push) >> 14;
    world.vy += ((uy * push) >> 14) - k_gravity;
    world.vz += (uz * push) >> 14;

    world.vx = damp(world.vx);
    world.vy = damp(world.vy);
    world.vz = damp(world.vz);

    world.x += world.vx;
    world.y += world.vy;
    world.z += world.vz;

    // ---- angular ----
    // A pod lifts its OWN corner. The torque is the plain cross product of
    // the arm with the thrust, r x F with F along the hull's +y, which for an
    // arm (ox, 0, oz) is (-oz, 0, ox) times the lift. Nothing here needs to
    // know the attitude: these are the hull's own axes, which is exactly what
    // makes them safe to integrate at any attitude at all.
    //
    // The y component is identically zero and is not an omission. Every pod
    // pushes along the centre line, and a force through the centre line has
    // no moment about it, so no pod can yaw this hull. That is the physics
    // agreeing with the design rather than the design being imposed on it.
    int32_t tx = 0, tz = 0;
    for (int i = 0; i < kPodCount; i++) {
        const int32_t lift = (k_pod_torque * world.throttle[i]) / 255;
        tx -= k_pods[i].oz * lift;
        tz += k_pods[i].ox * lift;
    }
    world.wx = damp(world.wx + tx);
    world.wy = damp(world.wy);
    world.wz = damp(world.wz + tz);

    // Compose this tick's turn onto the attitude, in the hull's own frame.
    // The rates are fp8 angle units where a turn is 4096 << 8; quat_integrate
    // wants Q14 radians, and 6434 / 65536 is 2 pi / (4096 * 256) * 16384.
    // Rounded rather than truncated so a slow turn still turns: truncation
    // floors anything under about a third of a degree a second to nothing.
    world.q = pse::quat_integrate(world.q,
                                  (world.wx * 6434 + 32768) >> 16,
                                  (world.wy * 6434 + 32768) >> 16,
                                  (world.wz * 6434 + 32768) >> 16);

    // ---- the ground ----
    // Landing well anywhere just parks the ship and it can lift off again.
    // The flight only ends on the deck it was sent to.
    const int32_t floor_y = ground_at(world, world.x, world.z);
    if (world.y <= floor_y) {
        world.y = floor_y;
        const int32_t fall = descent(world);
        const int32_t slide_x = world.vx < 0 ? -world.vx : world.vx;
        const int32_t slide_z = world.vz < 0 ? -world.vz : world.vz;
        const int32_t slide = slide_x > slide_z ? slide_x : slide_z;
        const int pad = pad_at(world, world.x, world.z);

        world.vx = world.vy = world.vz = 0;
        world.wx = world.wy = world.wz = 0;

        if (fall > k_safe_descent) {
            world.state = Flight::Crashed;
            world.fault = Fault::TooFast;
        } else if (tilt(world) > k_safe_tilt) {
            world.state = Flight::Crashed;
            world.fault = Fault::TooSteep;
        } else if (slide > k_safe_slide) {
            world.state = Flight::Crashed;
            world.fault = Fault::Scraped;
        } else if (pad >= 0 && pad == world.target) {
            world.state = Flight::Landed;
            world.landed_on = static_cast<uint8_t>(pad);
        } else {
            world.grounded = true;
            world.landed_on = pad >= 0 ? static_cast<uint8_t>(pad) : 0xFF;
        }
        if (world.state != Flight::Flying) world.ticks_in_state = 0;
    } else {
        world.grounded = false;
        world.landed_on = 0xFF;
        if (world.y > k_max_altitude) {
            world.y = k_max_altitude;
            if (world.vy > 0) world.vy = 0;
        }
    }

    if (world.state == Flight::Flying && tilt(world) > k_tumble_tilt) {
        world.state = Flight::Tumbled;
        world.ticks_in_state = 0;
    }
}

}  // namespace tl
