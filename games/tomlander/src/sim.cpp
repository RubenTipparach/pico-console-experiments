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

// The same thing at an arbitrary ratio, for the loaded hull's slower settle.
// Rounds toward zero on both signs for the same reason damp does: a decay
// that rounds the wrong way on negatives never lets a leftward drift die.
int32_t damp_ratio(int32_t v, int32_t num, int32_t den) {
    return v >= 0 ? (v * num) / den : -((-v) * num) / den;
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

// How far the sea floor has fallen away under this point. See k_shore_edge
// for why the ocean needs one at all.
int32_t seabed_drop(const World& world, int32_t x, int32_t z) {
    if (world.sea <= k_no_sea) return 0;

    // Seaward is the way the salvage deck lies from the shore deck. Taken
    // from the pads rather than stored, so a later sea mission laid out along
    // another axis gets its coast for free and there is no second copy of the
    // layout to disagree with the first.
    const int32_t legx = world.pads[1].x - world.pads[0].x;
    const int32_t legz = world.pads[1].z - world.pads[0].z;
    int32_t out;
    if (iabs(legx) > iabs(legz)) {
        out = legx > 0 ? x - world.pads[0].x : world.pads[0].x - x;
    } else {
        out = legz > 0 ? z - world.pads[0].z : world.pads[0].z - z;
    }
    if (out <= k_shore_edge) return 0;

    int32_t drop = static_cast<int32_t>(
        (static_cast<int64_t>(out - k_shore_edge) * k_seabed_grade) >> 8);
    return drop > k_seabed_floor ? k_seabed_floor : drop;
}

}  // namespace

int32_t terrain_height(const World& world, int32_t x, int32_t z) {
    int32_t h = terrain_raw(x, z) - seabed_drop(world, x, z);
    for (int i = 0; i < k_pad_count; i++) {
        // A floating deck has no apron. Flattening the sea floor to the
        // waterline around the wreck would have raised a plateau out of the
        // ocean, and the ocean is the mission.
        if (pad_floats(world, i)) continue;
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

namespace {

// Where the fuel crates stand, per mission.
//
// PLACEMENT, not geometry: a crate is a cube the renderer draws parametrically,
// and this table only says where. Kept off the straight line between decks on
// purpose. A crate sitting exactly on the course is fuel you collect by not
// steering, and the point of moving fuel off the pads was to make it a
// decision. Five or six units to one side is a lean and a lean back, which is
// the whole of this game's vocabulary.
//
// Also kept clear of the buildings in k_buildings: a crate inside a tower is
// one you cannot reach without hitting the tower.
struct CratePlacement { int16_t x, z; bool waits; };

// The delivery. One out on the A to B leg, the teaching crate beside deck B,
// and one on the B to C leg.
//
// The teaching one WAITS. The player picks up the cargo on deck B, and a fuel
// crate is sitting eleven units off the deck when they lift again: close
// enough that it is the next thing they fly through, far enough that they had
// to fly through it. That is the whole tutorial, and it costs no words.
// Clearance is measured against the TOWERS, not against every building. A
// crate hangs twelve units up and a block is at most 4.8 tall, so a crate can
// sit right over one; a tower is 10 to 15 and is genuinely in the way. Six
// units clear of a tower's footprint is what these are placed for, which keeps
// the hull outside it on any approach that reaches the crate.
const CratePlacement k_crates_delivery[] = {
    { -7,  12, false},   // A to B, halfway, 7 off the line
    { 26,  32, true },   // beside deck B, on the way out toward deck C
    { 10,  47, false},   // B to C, halfway, 6 off the line
};

// The salvage. Two over open water, on opposite sides of the crossing, so the
// one you steer for going out is not the one you steer for coming back.
const CratePlacement k_crates_salvage[] = {
    {-145,  20, false},
    {-172,  18, false},
};

void place_crates(World& world, Mission mission) {
    const CratePlacement* table = nullptr;
    int count = 0;
    // Mission one places none. It is one leg on one tank with room to spare,
    // and the delivery is where a crate first has to be understood: meeting
    // one before it matters would spend the lesson on a flight that did not
    // need it.
    if (mission == Mission::Delivery) {
        table = k_crates_delivery;
        count = static_cast<int>(sizeof(k_crates_delivery) /
                                 sizeof(k_crates_delivery[0]));
    } else if (mission == Mission::Salvage) {
        table = k_crates_salvage;
        count = static_cast<int>(sizeof(k_crates_salvage) /
                                 sizeof(k_crates_salvage[0]));
    }
    if (count > k_crate_max) count = k_crate_max;

    world.crate_count = static_cast<uint8_t>(count);
    world.crates_taken = 0;
    for (int i = 0; i < k_crate_max; i++) {
        if (i >= count) {
            world.crates[i] = FuelCrate{0, 0, 0, Crate::Taken};
            continue;
        }
        const int32_t x = table[i].x * 65536;
        const int32_t z = table[i].z * 65536;
        // Floats above whatever is underneath, which over the salvage's ocean
        // is the waterline rather than the sea floor: ground_at clamps to the
        // sea, so a crate out at the wreck hangs twelve units over the water
        // instead of twelve units over a seabed forty units down.
        world.crates[i] = FuelCrate{
            x, z, ground_at(world, x, z) + k_crate_hover,
            table[i].waits ? Crate::Waiting : Crate::Out};
    }
}

}  // namespace

int pad_at(const World& world, int32_t x, int32_t z) {
    for (int i = 0; i < k_pad_count; i++) {
        if (iabs(x - world.pads[i].x) <= world.pads[i].half &&
            iabs(z - world.pads[i].z) <= world.pads[i].half) {
            return i;
        }
    }
    return -1;
}

// static const, so it sits in flash rather than costing 88 bytes of the
// 264 KB of RAM permanently.
const Building k_buildings[k_building_count] = {
    // Along the A to B leg, which runs from (-34,-8) to (32,22).
    {-16,   4, 3, false, 220},
    { -6,  -6, 2, false, 255},
    {  2,   8, 3, true,  255},
    { 12,  -2, 2, false, 200},
    { 14,  16, 2, true,  235},
    // Off to the side of B's apron, so the deck itself stays clear.
    { 46,  10, 2, false, 240},
    // Along the B to C leg, (32,22) to (-20,62).
    { 18,  38, 3, true,  255},
    {  8,  30, 2, false, 215},
    {  2,  48, 3, false, 250},
    { -8,  40, 2, true,  225},
    {-30,  46, 2, false, 235},
};

// Which building's footprint covers this point, or -1. Chebyshev, same as the
// pad test: a square footprint and no square root.
// How high a building's roof stands. Read by the floor (a roof you can put
// down on) and by the wall (the height a hull has to clear to pass over).
int32_t building_roof(int index) {
    const Building& b = k_buildings[index];
    return terrain_raw(b.x * 65536, b.z * 65536) + building_height(b);
}

int building_at(int32_t x, int32_t z) {
    for (int i = 0; i < k_building_count; i++) {
        const Building& b = k_buildings[i];
        const int32_t dx = iabs(x - (b.x * 65536));
        const int32_t dz = iabs(z - (b.z * 65536));
        const int32_t half = b.half * 65536;
        if (dx <= half && dz <= half) return i;
    }
    return -1;
}

// Push a hull back out of a building it has flown INTO, and say whether the
// impact was hard enough to end the flight.
//
// A building used to be nothing but a tall floor: ground_at returned its roof
// across the whole footprint, so a hull crossing that footprint below roof
// height was instantly "below the floor" and the touchdown gates judged it as a
// LANDING. Flying level into a tower at six units a second, at the lean any
// translating hull carries, came out as TOO STEEP. A landing verdict for a wall
// strike, on a flight that never touched the ground.
//
// So a wall is a wall now. Coming in from the side pushes the hull back out
// along whichever axis it is least far into, kills the velocity into the wall,
// and leaves it flying. Only a genuinely fast impact ends anything, judged by
// the same k_safe_slide a scrape is judged by, and it says Struck rather than
// borrowing a landing's word. The roof is still a floor from above, which is
// what keeps a building something you can put down on.
bool resolve_building_walls(World& world) {
    const int index = building_at(world.x, world.z);
    if (index < 0) return false;
    const Building& b = k_buildings[index];
    // Above the roof, or only just under it, is the floor's business and not
    // the wall's: a hull settling ONTO a roof passes through the boundary and
    // must not be shoved off sideways for it. See k_wall_bite.
    if (world.y >= building_roof(index) + k_rest_height - k_wall_bite) {
        return false;
    }

    const int32_t half = b.half * 65536;
    const int32_t cx = b.x * 65536;
    const int32_t cz = b.z * 65536;
    // How far in from each face, and which is nearest: that is the way out.
    const int32_t in_x = half - iabs(world.x - cx);
    const int32_t in_z = half - iabs(world.z - cz);

    const int32_t into = in_x < in_z ? iabs(world.vx) : iabs(world.vz);
    if (into > k_safe_slide) {
        world.vx = world.vy = world.vz = 0;
        world.wx = world.wy = world.wz = 0;
        world.state = Flight::Crashed;
        world.fault = Fault::Struck;
        world.ticks_in_state = 0;
        return true;
    }

    // Just OUTSIDE the footprint, not exactly on it. building_at counts the
    // boundary as inside (dx <= half), so pushing to the edge exactly left the
    // hull still in the building as far as the floor was concerned, and the
    // ground check on the very next line found the roof and called it a
    // landing. A sixteenth of a unit is under a pixel and settles it.
    if (in_x < in_z) {
        world.x = world.x > cx ? cx + half + k_wall_clear
                               : cx - half - k_wall_clear;
        world.vx = 0;
    } else {
        world.z = world.z > cz ? cz + half + k_wall_clear
                               : cz - half - k_wall_clear;
        world.vz = 0;
    }
    return false;
}

bool over_water(const World& world, int32_t x, int32_t z) {
    if (pad_at(world, x, z) >= 0) return false;
    return terrain_height(world, x, z) < world.sea;
}

int32_t ground_at(const World& world, int32_t x, int32_t z) {
    const int pad = pad_at(world, x, z);
    if (pad >= 0) return world.pads[pad].y + k_pad_rise + k_rest_height;

    // A roof is a floor. The buildings are solid, so putting the ship down on
    // one lands it there, and flying into the side of one is a landing at
    // whatever speed you hit it, which the descent and slide gates then judge
    // exactly as they judge the ground.
    int32_t terrain = terrain_height(world, x, z);
    // The sea is a floor, not a hole. Clamping here rather than in the terrain
    // itself is what keeps the water surface and the collision surface the
    // same number: the renderer clamps the same way, so a splash happens
    // exactly where the water is drawn.
    if (terrain < world.sea) terrain = world.sea;
    const int building = building_at(x, z);
    if (building >= 0) {
        const int32_t roof = building_roof(building);
        if (roof > terrain) return roof + k_rest_height;
    }
    return terrain + k_rest_height;
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

void world_init(World& world, Mission mission) {
    world = World{};
    world.q = pse::quat_identity();
    world.mission = mission;

    // Left shift of a negative is undefined, so these multiply. The compiler
    // folds them either way; the warning is the only difference that matters.
    //
    // Three decks, spaced so each leg is a similar flight: A to B is 72 units
    // and B to C is 66. Mission one only ever uses A and B; C is built either
    // way, because a deck that appears when a mission starts would be a deck
    // the player has not been able to see coming.
    if (mission == Mission::Salvage) {
        // The coast. Not guessed: a probe walked the terrain for dry ground
        // with deep water a long way off it, and this is what it found. The
        // shore stands 11.2 above datum, the sea floor at the wreck is 11.3
        // below, and the crossing is 99 units.
        //
        // Long on purpose. The first placement put the wreck 45 units out and
        // there was barely any sea in between: the shore pad's apron flattens
        // the ground for 32 units around it, and the wreck's own landing
        // square takes 7 more, so the actual open water was about six units
        // wide. That is a puddle, and the mission is supposed to be a crossing.
        world.pads[0] = Pad{-112 * 65536,  4 * 65536, 0, k_pad_half};
        world.pads[1] = Pad{-206 * 65536, 34 * 65536, 0, k_salvage_half};
        world.pads[2] = Pad{-112 * 65536,  4 * 65536, 0, k_pad_half};
        world.sea = k_sea_level;
    } else {
        world.pads[0] = Pad{-34 * 65536,  -8 * 65536, 0, k_pad_half};
        world.pads[1] = Pad{ 32 * 65536,  22 * 65536, 0, k_pad_half};
        world.pads[2] = Pad{-20 * 65536,  62 * 65536, 0, k_pad_half};
        world.sea = k_no_sea;
    }
    // The apron sits at whatever the untouched landscape does there, so a pad
    // looks like it was built on the hill rather than punched through it. A
    // deck out past the waterline is the exception: it is a rocket section
    // floating in the swell, so it rides the sea rather than the sea floor.
    for (int i = 0; i < k_pad_count; i++) {
        // terrain_raw plus the seabed, not terrain_height: the apron a deck
        // lays down is read out of pad.y, which is the value being worked out
        // here, so asking for the finished ground would be asking a question
        // that depends on its own answer.
        const int32_t ground =
            terrain_raw(world.pads[i].x, world.pads[i].z) -
            seabed_drop(world, world.pads[i].x, world.pads[i].z);
        world.pads[i].y = ground < world.sea ? world.sea + k_float_rise
                                             : ground;
    }

    // Both missions fly to B first. Mission one lands there and is done;
    // mission two finds the crate there and carries it on to C, so the first
    // leg is a flight the player has already made and the new thing is what
    // happens after it.
    world.target = 1;
    world.cargo = mission == Mission::Hop ? kCargoNone : 1;
    // The delivery carries its crate ONWARD to a third deck; the salvage
    // brings the rocket section BACK to the shore it launched from. Naming
    // the destination is what lets one pickup serve both.
    world.deliver_to = mission == Mission::Salvage ? 0 : 2;
    world.landed_on = 0;
    world.grounded = true;
    world.state = Flight::Flying;
    world.fault = Fault::None;
    world.fuel = k_fuel_full;
    world.damage = 0;
    world.dry_ticks = 0;
    place_crates(world, mission);

    // Every mission starts ON pad A, not falling toward it. The first thing a
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
    int32_t push = (k_pod_thrust * total) / 255;
    // The crate's weight. Thrust is scaled and gravity is not, because gravity
    // is an acceleration and does not care what the ship masses.
    if (carrying(world)) {
        push = (push * k_cargo_thrust_num) / k_cargo_thrust_den;
    }
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
    // The crate's sway: it does not settle when the hull does, so the hull
    // keeps moving. Less damping, same torque, which reads as heavy rather
    // than as sluggish.
    if (carrying(world)) {
        world.wx = damp_ratio(world.wx + tx, k_cargo_swing_num,
                              k_cargo_swing_den);
        world.wy = damp_ratio(world.wy, k_cargo_swing_num, k_cargo_swing_den);
        world.wz = damp_ratio(world.wz + tz, k_cargo_swing_num,
                              k_cargo_swing_den);
    } else {
        world.wx = damp(world.wx + tx);
        world.wy = damp(world.wy);
        world.wz = damp(world.wz + tz);
    }

    // Compose this tick's turn onto the attitude, in the hull's own frame.
    // The rates are fp8 angle units where a turn is 4096 << 8; quat_integrate
    // wants Q14 radians, and 6434 / 65536 is 2 pi / (4096 * 256) * 16384.
    // Rounded rather than truncated so a slow turn still turns: truncation
    // floors anything under about a third of a degree a second to nothing.
    world.q = pse::quat_integrate(world.q,
                                  (world.wx * 6434 + 32768) >> 16,
                                  (world.wy * 6434 + 32768) >> 16,
                                  (world.wz * 6434 + 32768) >> 16);

    // ---- fuel crates ----
    // Checked after the move and before the ground, so a crate low over a
    // hillside is still collected on the tick the hull passes through it
    // rather than on the tick after it has already put down.
    for (int i = 0; i < world.crate_count; i++) {
        FuelCrate& c = world.crates[i];
        if (c.state != Crate::Out) continue;
        if (iabs(world.x - c.x) > k_crate_reach) continue;
        if (iabs(world.y - c.y) > k_crate_reach) continue;
        if (iabs(world.z - c.z) > k_crate_reach) continue;
        c.state = Crate::Taken;
        world.crates_taken++;
        world.fuel += k_crate_fuel;
        // A tank does not overfill. Two crates in a row is one crate's worth
        // of fuel and one crate wasted, which is what makes taking them in the
        // right order worth thinking about.
        if (world.fuel > k_fuel_full) world.fuel = k_fuel_full;
        // Fuel back in the tank is control back in the player's hands, so the
        // countdown that was going to call the flight starts again from zero.
        world.dry_ticks = 0;
    }

    // ---- walls ----
    // Before the ground, because a hull that has flown into a building has to
    // be back outside it before anything asks where the floor under it is.
    if (resolve_building_walls(world)) return;

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

        if (over_water(world, world.x, world.z)) {
            // Anything that touches the sea is lost, however gently. A lander
            // is not a boat, and a soft ditching that merely parked the ship
            // would make the ocean scenery rather than the hazard the mission
            // is built around.
            world.state = Flight::Crashed;
            world.fault = Fault::Ditched;
        } else if (descent_band(fall) == Touchdown::Fatal) {
            world.state = Flight::Crashed;
            world.fault = Fault::TooFast;
        } else if (tilt(world) > k_safe_tilt) {
            world.state = Flight::Crashed;
            world.fault = Fault::TooSteep;
        } else if (slide > k_safe_slide) {
            world.state = Flight::Crashed;
            world.fault = Fault::Scraped;
        } else if (descent_band(fall) == Touchdown::Hard &&
                   ++world.damage >= k_damage_max) {
            // The hull survives a hard arrival, and it does not survive two.
            // Counted rather than gated so the cost carries across the whole
            // flight: bang it down on the pickup and the delivery has to be
            // clean, which is a consequence a player can feel coming.
            world.state = Flight::Crashed;
            world.fault = Fault::Broke;
        } else if (pad >= 0 && pad == world.target) {
            // The crate is a LEG, not an ending. Setting down on the deck it
            // is sitting on loads it and re-aims at the next deck, and the
            // flight carries straight on from a standing start, which is
            // exactly the take off the player already knows how to do.
            if (world.cargo == static_cast<uint8_t>(pad)) {
                world.cargo = kCargoHeld;
                world.target = world.deliver_to;
                world.grounded = true;
                world.landed_on = static_cast<uint8_t>(pad);
                // The deck does NOT refuel. It used to fill the tank, and that
                // made the decks free: arrive on fumes every time and fuel
                // stopped being a thing you thought about. The fuel is out on
                // the map now, in crates you have to fly through.
                //
                // Picking the load up is what puts the delivery's teaching
                // crate on the map, eleven units off this deck. The player
                // meets their first crate on the way out with a full load
                // aboard, which is the moment the fuel starts to matter, and
                // nothing had to be written on the screen to explain it.
                for (int i = 0; i < world.crate_count; i++) {
                    if (world.crates[i].state == Crate::Waiting) {
                        world.crates[i].state = Crate::Out;
                    }
                }
            } else {
                if (carrying(world)) world.cargo = kCargoDone;
                world.state = Flight::Landed;
                world.landed_on = static_cast<uint8_t>(pad);
            }
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

    // Being upside down used to end the flight here, at a quarter turn. It
    // does not any more: a tumble is a situation, not an outcome. The hull
    // keeps its pods and the leveller can right it, so rolling past ninety
    // degrees is something to fly out of rather than a verdict handed down
    // while the ship is still in the air and still under control.

    // An empty tank ends a mission, five seconds later, once the hull has
    // stopped moving.
    //
    // The gauge reaching zero is the end of the CONTROL, not the end of the
    // flight. A hull that runs dry at altitude still has everything it had a
    // moment ago except thrust: its speed, its lean, and a long way to fall.
    // Calling it at zero threw away the most interesting seconds the game has,
    // and threw away a real outcome with them, because a dead stick glide onto
    // the deck you were sent to is a landing and it counts.
    //
    // Both halves of the condition earn their place. The timer alone would cut
    // a fall short, since terminal descent is 17.7 units a second and the
    // ceiling is a good deal more than five of those. The stop test alone
    // would call the flight the instant a hull settled, with no beat to watch
    // it happen. So: at least five seconds, and then stopped.
    //
    // Whatever happens on the way down is judged by the touchdown exactly as
    // it always was. Arrive too fast and the fault is TOO FAST, because that
    // is what actually broke the ship. NO FUEL is the other ending, the one
    // with no other name: down safe, in one piece, somewhere that is not the
    // deck, with nothing left to lift off on again. That case used to sit
    // there forever waiting for a launch that could never happen.
    //
    // Judged LAST in the tick on purpose. A touchdown resolved above may have
    // ended the flight, or may have been a leg that filled the tank back up,
    // and both of those should beat the empty gauge: arriving on the pickup
    // deck as the last of the fuel goes is a save, not a loss.
    if (world.fuel > 0) {
        world.dry_ticks = 0;
    } else if (mission_open(world)) {
        if (world.dry_ticks < 0xFFFF) world.dry_ticks++;
        if (world.dry_ticks >= k_dry_grace && at_rest(world)) {
            world.state = Flight::Crashed;
            world.fault = Fault::Dry;
            world.ticks_in_state = 0;
        }
    }
}

}  // namespace tl
