#include "sim.hpp"

namespace sd {
namespace {

constexpr int32_t k_q14 = pse::k_quat_one;   // 16384

// ---- small integer maths ----

int32_t clamp32(int32_t v, int32_t lo, int32_t hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

int32_t abs32(int32_t v) { return v < 0 ? -v : v; }

// Integer square root of a 64 bit value. No floats: this runs every tick on a
// chip with no FPU, and sqrtf would be a software call each time.
uint32_t isqrt64(uint64_t value) {
    if (value == 0) return 0;
    uint64_t x = value;
    uint64_t bit = 1ULL << 62;
    while (bit > x) bit >>= 2;
    uint64_t root = 0;
    while (bit != 0) {
        if (x >= root + bit) {
            x -= root + bit;
            root = (root >> 1) + bit;
        } else {
            root >>= 1;
        }
        bit >>= 2;
    }
    return static_cast<uint32_t>(root);
}

// A Q14 unit vector along (dx, dy, dz), whatever scale those came in at.
// Returns false when the input is too short to have a direction, which the
// callers treat as "leave the heading alone" rather than as an error.
bool normalize_q14(int32_t dx, int32_t dy, int32_t dz,
                   int32_t& ox, int32_t& oy, int32_t& oz) {
    const uint64_t sq = static_cast<uint64_t>(static_cast<int64_t>(dx) * dx) +
                        static_cast<uint64_t>(static_cast<int64_t>(dy) * dy) +
                        static_cast<uint64_t>(static_cast<int64_t>(dz) * dz);
    const uint32_t len = isqrt64(sq);
    if (len == 0) return false;
    ox = static_cast<int32_t>((static_cast<int64_t>(dx) * k_q14) / len);
    oy = static_cast<int32_t>((static_cast<int64_t>(dy) * k_q14) / len);
    oz = static_cast<int32_t>((static_cast<int64_t>(dz) * k_q14) / len);
    return true;
}

// Scale a Q14 direction by an fp16 length, giving fp16.
inline int32_t along(int32_t dir_q14, int32_t length_fp) {
    return static_cast<int32_t>(
        (static_cast<int64_t>(dir_q14) * length_fp) >> 14);
}

uint32_t next_random(World& world) {
    world.rng ^= world.rng << 13;
    world.rng ^= world.rng >> 17;
    world.rng ^= world.rng << 5;
    return world.rng;
}

// ---- the ship classes ----

struct HullStats {
    int16_t hull;
    int16_t shield;
    int32_t speed;        // fp16 per tick
    int32_t radius;       // fp16
    int32_t length;       // fp16, for the renderer's scale
    uint16_t fire_period; // ticks between shots from the ship's own guns
    int32_t fire_range;   // fp16
    int32_t score;
};

// One table, read by the spawner, the collision, the renderer and the HUD.
// `radius` is the BOUNDING sphere, big enough to contain the whole hull, and
// it is a first pass reject rather than the hit volume: see k_hull_box.
// `length` is the one number that says how big a class of ship is. The models
// are all a unit long, the hardpoint seats are in thousandths of a model, and
// the renderer draws at this scale, so changing a number in this column moves
// the picture, the turrets and the hit volume together.
//
// They are as big as they are because of the screen. At 120 pixels a hull is
// only a shape while it covers about eight of them, and below that every one
// of its triangles rounds to zero area and the rasterizer culls the lot: a
// fighter at thirty units did not get smaller, it vanished. The sizes here put
// a dogfight's worth of range inside the band where a ship is still a ship.
const HullStats k_stats[static_cast<int>(Hull::HullCount)] = {
    // hull shield  speed        radius       length      period  range        score
    {  26,   14,    centi(26),   centi(192),  centi(310),   26,   units(80),    100},  // Fighter
    {  60,   30,    centi(15),   centi(314),  units(5),     58,   units(105),   220},  // Bomber
    { 150,   60,    centi(9),    centi(705),  units(12),    40,   units(130),   600},  // Gunship
    { 420,  140,    centi(5),    units(18),   units(34),    34,   units(150),  2200},  // Frigate
};

inline const HullStats& stats(Hull cls) {
    return k_stats[static_cast<int>(cls)];
}

// The plating, as a box in the ship's own frame, for the hulls big enough that
// a sphere is a lie.
//
// A frigate is 26 units long and 5 wide, so a sphere containing it is 14 units
// across and nine tenths of it is empty space. That was not a cosmetic problem:
// every shot aimed at a turret sponson entered the sphere twelve units before
// it reached the turret, was counted against the plating, and stopped. The
// hardpoints were unreachable, which is to say the entire point of the capital
// ships did not work. A box the size of the actual spine lets a bolt aimed
// outboard of the hull fly past the plating and reach what it was aimed at.
//
// Zero means "no box, use the sphere", which is what a fighter gets: at 2.2
// units long the sphere IS the ship, and there are four of them on the field
// to the capitals' one.
// Half extents in thousandths of the model, same as the hardpoint seats, so
// the box follows the hull when a class is resized. Deliberately smaller than
// the model's full bounding box: it is the SPINE, not the silhouette, and the
// turret sponsons stick out past it. That is the whole point.
struct HullBox {
    int32_t hx, hy, hz;
};

const HullBox k_hull_box[static_cast<int>(Hull::HullCount)] = {
    {0, 0, 0},                 // Fighter: the sphere is the ship
    {0, 0, 0},                 // Bomber
    {145, 175, 500},           // Gunship, sponsons at 167 sit outside it
    {92, 66, 460},             // Frigate, sponsons at 104 sit outside it
};

// ---- hardpoint layouts ----
//
// Where each class carries its subsystems, in hundredths of a unit in the
// ship's own frame, nose to tail. These are seats on the model: keep them in
// step with the .obj or a turret ends up floating beside its sponson.

struct SubSpec {
    Sub kind;
    int16_t ox, oy, oz;
    int16_t hull;
    int16_t radius;
};

// Fighters and bombers carry none, and that is a decision rather than an
// omission. A bomber is 3.6 units long, so an engine hardpoint on it is a
// sphere filling most of the hull, and every shot down the centre line takes
// its engines off whether or not anyone was aiming for them. A bomber that
// loses its engines to a head-on pass nobody aimed is a bomber sitting still
// for the rest of the battle, and it makes disabling a ship feel like
// something that happens to you rather than something you did. Hardpoints
// belong on the ships big enough for them to be places: the gunship and the
// frigate, where a turret really is out on a sponson and the engines really
// are at the back.

// Seats and radii in thousandths of the model, nose to tail. Read them
// straight off models/gunship.obj and models/frigate.obj: a turret at 167 sits
// on the sponson whose blocks run from 120 to 230.
const SubSpec k_gunship_subs[] = {
    {Sub::Weapons,  -167,   78,   122,  40, 111},
    {Sub::Weapons,   167,   78,   122,  40, 111},
    {Sub::Shields,     0,  156,   -67,  55, 122},
    {Sub::Engines,     0,    0,  -444,  50, 133},
};

const SubSpec k_frigate_subs[] = {
    {Sub::Navigation,    0,   27,   388,  90,  73},
    {Sub::Weapons,    -104,   31,   138,  70,  73},
    {Sub::Weapons,     104,   31,   138,  70,  73},
    {Sub::Shields,       0,   77,   -23, 110,  77},
    {Sub::LifeSupport,   0,  -38,  -127,  80,  73},
    {Sub::Engines,       0,    0,  -446, 120,  88},
};

struct SubLayout {
    const SubSpec* specs;
    uint8_t count;
};

const SubLayout k_layouts[static_cast<int>(Hull::HullCount)] = {
    {nullptr, 0},          // Fighter
    {nullptr, 0},          // Bomber
    {k_gunship_subs, 4},
    {k_frigate_subs, 6},
};

// ---- the waves ----
//
// Where a wave arrives, relative to the player at the moment it is called in:
// `ahead` down the nose, `right` and `up` across it. Written that way rather
// than as arena coordinates so a wave always warps in somewhere the player
// can see, whatever heading they happened to be on when the last one died.

struct Spawn {
    Hull cls;
    int16_t ahead, right, up;   // whole world units
};

const Spawn k_wave1[] = {
    {Hull::Fighter, 120, -14,   5},
    {Hull::Fighter, 132,  16,  -6},
};

const Spawn k_wave2[] = {
    {Hull::Fighter, 118, -22,  -8},
    {Hull::Fighter, 130,   0,  12},
    {Hull::Fighter, 124,  24,  -4},
};

const Spawn k_wave3[] = {
    {Hull::Bomber,  140, -10,   6},
    {Hull::Bomber,  146,  12,   4},
    {Hull::Fighter, 116, -28,  -6},
    {Hull::Fighter, 120,  28,  -6},
};

const Spawn k_wave4[] = {
    {Hull::Gunship, 150,   6,   2},
    {Hull::Fighter, 112, -26,   8},
    {Hull::Fighter, 118,  26,   8},
    {Hull::Fighter, 128,   0, -14},
};

const Spawn k_wave5[] = {
    {Hull::Frigate, 175,   0,   0},
    {Hull::Gunship, 140, -34,  10},
    {Hull::Fighter, 115,  30,  -8},
    {Hull::Fighter, 122, -30, -10},
};

struct WaveSpec {
    const Spawn* ships;
    uint8_t count;
};

const WaveSpec k_waves[k_wave_count] = {
    {k_wave1, 2}, {k_wave2, 3}, {k_wave3, 4}, {k_wave4, 4}, {k_wave5, 4},
};

// ---- helpers over the world ----

void basis_of(const pse::Quat& q, int32_t fwd[3], int32_t right[3],
              int32_t up[3]) {
    pse::quat_rotate(q, 0, 0, k_q14, fwd[0], fwd[1], fwd[2]);
    pse::quat_rotate(q, k_q14, 0, 0, right[0], right[1], right[2]);
    pse::quat_rotate(q, 0, k_q14, 0, up[0], up[1], up[2]);
}

// Just the nose. Most of the per tick callers want only this, and asking for
// the whole frame costs three quaternion rotations where one will do, once
// per ship per tick.
void forward_of(const pse::Quat& q, int32_t fwd[3]) {
    pse::quat_rotate(q, 0, 0, k_q14, fwd[0], fwd[1], fwd[2]);
}

// Turn a body onto a world direction, at most `rate` this tick.
//
// The desired direction is taken into the body's OWN frame first, and that is
// the whole trick: in body coordinates the answer is immediate, because the
// nose is +z by definition, so how far the target sits to the side IS the yaw
// error and how far it sits above IS the pitch error. No angles are formed
// and no attitude is special.
void turn_toward(pse::Quat& q, int32_t dx, int32_t dy, int32_t dz,
                 int32_t rate) {
    int32_t nx, ny, nz;
    if (!normalize_q14(dx, dy, dz, nx, ny, nz)) return;

    int32_t bx, by, bz;
    pse::quat_unrotate(q, nx, ny, nz, bx, by, bz);

    // Behind the ship the sideways error tells you almost nothing: a target
    // dead astern has bx and by near zero and needs the hardest possible
    // turn, not the softest. Peg the command to full deflection back there
    // and let the sign pick a side.
    if (bz < 0) {
        const int32_t mag = abs32(bx) > abs32(by) ? abs32(bx) : abs32(by);
        if (mag > 0) {
            bx = (bx * k_q14) / mag;
            by = (by * k_q14) / mag;
        } else {
            by = k_q14;         // exactly astern: pull up, arbitrarily
        }
    }

    // A positive rotation about the body's +y swings the nose toward +x, and
    // a positive rotation about +x swings it toward -y. Hence the sign on the
    // pitch term, which is the one that is easy to get backwards and reads on
    // screen as a ship that flies away from what it is chasing.
    const int32_t ry = clamp32((bx * rate) / k_q14, -rate, rate);
    const int32_t rx = clamp32((-by * rate) / k_q14, -rate, rate);
    q = pse::quat_integrate(q, rx, ry, 0);
}

// Point a body straight at something, now, for placing a ship as it warps in.
//
// Repeated small turns rather than one axis and angle, because an axis and an
// angle needs an arccosine and this file has no floats in it: the flight model
// runs a hundred times a second on a chip with no FPU, and a trigonometry
// call in here to serve four spawns a minute is the wrong trade. Forty steps
// of a seventh of a radian closes any error, including the half turn a ship
// spawning behind the player starts with.
//
// quat_integrate's small angle step is a first order approximation, so the
// step size matters: 4000 in Q14 is 0.24 radians, where the approximation is
// still good to a quarter of a percent and the normalise absorbs the rest.
void face_toward(pse::Quat& q, int32_t dx, int32_t dy, int32_t dz) {
    for (int i = 0; i < 40; i++) {
        turn_toward(q, dx, dy, dz, 4000);
    }
}

// Distance from a moving point's path this tick to a sphere, as a yes or no.
//
// A swept test rather than a point test, and not for tidiness: a gun bolt
// covers two whole units in a tick and a fighter is 2.2 units across, so a
// point test at the end of the step misses shots that went straight through
// the target. That is the kind of bug that reads as "the guns feel wrong"
// and never as a bug.
bool segment_hits_sphere(int32_t px, int32_t py, int32_t pz,
                         int32_t dx, int32_t dy, int32_t dz,
                         int32_t cx, int32_t cy, int32_t cz, int32_t radius) {
    // Cheap reject first, on the box that contains the whole swept segment
    // plus the radius. Most bolt/ship pairs die here for three subtractions.
    const int32_t reach = radius + (abs32(dx) > abs32(dy)
                                        ? (abs32(dx) > abs32(dz) ? abs32(dx)
                                                                 : abs32(dz))
                                        : (abs32(dy) > abs32(dz) ? abs32(dy)
                                                                 : abs32(dz)));
    if (abs32(cx - px) > reach || abs32(cy - py) > reach ||
        abs32(cz - pz) > reach) {
        return false;
    }

    const int64_t mx = cx - px, my = cy - py, mz = cz - pz;
    const int64_t dd = static_cast<int64_t>(dx) * dx +
                       static_cast<int64_t>(dy) * dy +
                       static_cast<int64_t>(dz) * dz;

    int64_t nearest_x = 0, nearest_y = 0, nearest_z = 0;
    if (dd > 0) {
        const int64_t md = mx * dx + my * dy + mz * dz;
        // t clamped to the segment, kept as a fraction of dd rather than
        // divided out, so nothing is lost to integer division before the
        // comparison that matters.
        int64_t t_num = md, t_den = dd;
        if (t_num < 0) { t_num = 0; t_den = 1; }
        if (t_num > t_den) { t_num = 1; t_den = 1; }
        nearest_x = (static_cast<int64_t>(dx) * t_num) / t_den;
        nearest_y = (static_cast<int64_t>(dy) * t_num) / t_den;
        nearest_z = (static_cast<int64_t>(dz) * t_num) / t_den;
    }

    const int64_t ex = mx - nearest_x, ey = my - nearest_y, ez = mz - nearest_z;
    const int64_t dist2 = ex * ex + ey * ey + ez * ez;
    const int64_t r2 = static_cast<int64_t>(radius) * radius;
    return dist2 <= r2;
}

// The same swept question against an oriented box: does this tick's flight
// path pass through the ship's plating?
//
// Both ends of the segment are taken into the ship's own frame first, which
// turns an oriented box test into an axis aligned one, and then it is the
// standard slab clip: keep the interval of the segment that is inside every
// pair of faces, and if what is left is empty the segment missed.
//
// The interval is carried as a Q16 fraction of the segment rather than as a
// distance, so nothing has to be divided by the segment's length and the two
// ends can be compared directly.
bool segment_hits_box(const Ship& ship, int32_t px, int32_t py, int32_t pz,
                      int32_t dx, int32_t dy, int32_t dz, const HullBox& box) {
    int32_t lp[3], ld[3];
    pse::quat_unrotate(ship.q, px - ship.x, py - ship.y, pz - ship.z, lp[0],
                       lp[1], lp[2]);
    pse::quat_unrotate(ship.q, dx, dy, dz, ld[0], ld[1], ld[2]);

    // The box is in thousandths of the model, so it has to be grown to the
    // size this class of ship is actually drawn at before anything is compared
    // against a world space position.
    const int32_t length = stats(ship.cls).length;
    const int32_t half[3] = {
        static_cast<int32_t>((static_cast<int64_t>(box.hx) * length) / 1000),
        static_cast<int32_t>((static_cast<int64_t>(box.hy) * length) / 1000),
        static_cast<int32_t>((static_cast<int64_t>(box.hz) * length) / 1000),
    };

    int64_t enter = 0;              // Q16
    int64_t leave = 1 << 16;

    for (int axis = 0; axis < 3; axis++) {
        const int32_t p = lp[axis], d = ld[axis], h = half[axis];
        if (d == 0) {
            // Parallel to this pair of faces: either always between them or
            // never.
            if (p < -h || p > h) return false;
            continue;
        }
        int64_t t0 = (static_cast<int64_t>(-h - p) << 16) / d;
        int64_t t1 = (static_cast<int64_t>(h - p) << 16) / d;
        if (t0 > t1) {
            const int64_t swap = t0;
            t0 = t1;
            t1 = swap;
        }
        if (t0 > enter) enter = t0;
        if (t1 < leave) leave = t1;
        if (enter > leave) return false;
    }
    return true;
}

Shot* free_shot(World& world) {
    for (uint8_t i = 0; i < k_max_bolts; i++) {
        if (!world.shots[i].active) return &world.shots[i];
    }
    return nullptr;
}

void spawn_shot(World& world, Bolt kind, int32_t x, int32_t y, int32_t z,
                int32_t dirx, int32_t diry, int32_t dirz, int32_t speed,
                uint16_t life, int16_t damage) {
    Shot* shot = free_shot(world);
    if (shot == nullptr) return;
    shot->active = true;
    shot->kind = kind;
    shot->x = x; shot->y = y; shot->z = z;
    shot->vx = along(dirx, speed);
    shot->vy = along(diry, speed);
    shot->vz = along(dirz, speed);
    shot->life = life;
    shot->damage = damage;
}

// `radius` is fp16; Blast::size is hundredths of a unit. See the struct.
void spawn_blast(World& world, int32_t x, int32_t y, int32_t z,
                 int32_t radius) {
    const int32_t size = (radius / k_one) * 100 + ((radius % k_one) * 100) / k_one;
    // Oldest slot wins when they are all busy. Five at once is already more
    // than a 120 pixel screen can say anything with, and dropping the newest
    // would hide the kill the player just made in favour of one they are
    // already done looking at.
    Blast* pick = nullptr;
    for (uint8_t i = 0; i < k_max_blasts; i++) {
        Blast& b = world.blasts[i];
        if (!b.active) { pick = &b; break; }
        if (pick == nullptr || b.life < pick->life) pick = &b;
    }
    if (pick == nullptr) return;
    pick->active = true;
    pick->x = x; pick->y = y; pick->z = z;
    pick->life = k_blast_life;
    pick->size = static_cast<int16_t>(size > 32767 ? 32767 : size);
}

void tick_blasts(World& world) {
    for (uint8_t i = 0; i < k_max_blasts; i++) {
        Blast& b = world.blasts[i];
        if (!b.active) continue;
        if (b.life == 0) { b.active = false; continue; }
        b.life--;
    }
}

// Damage the player, shields first. Returns nothing: the caller has already
// decided the hit happened.
void hurt_player(World& world, int16_t amount) {
    world.shield_idle = 0;
    world.hit_flash = 8;
    if (world.shield > 0) {
        const int16_t taken = world.shield < amount ? world.shield : amount;
        world.shield = static_cast<int16_t>(world.shield - taken);
        amount = static_cast<int16_t>(amount - taken);
    }
    if (amount <= 0) return;
    world.hull = static_cast<int16_t>(world.hull - amount);
    if (world.hull <= 0) {
        world.hull = 0;
        world.phase = Phase::Lost;
        world.loss = Loss::Destroyed;
        spawn_blast(world, world.x, world.y, world.z, k_player_radius * 4);
    }
}

void award(World& world, uint32_t points) { world.score += points; }

// A ship's shield eats damage before its plating does, and a hardpoint's does
// not: a turret is outside the bubble, which is what makes picking one off
// worth doing before the hull is down.
void hurt_ship(World& world, Ship& ship, int16_t amount) {
    ship.hit_flash = 6;
    if (ship.shield > 0) {
        const int16_t taken = ship.shield < amount ? ship.shield : amount;
        ship.shield = static_cast<int16_t>(ship.shield - taken);
        amount = static_cast<int16_t>(amount - taken);
    }
    if (amount <= 0) return;
    ship.hull = static_cast<int16_t>(ship.hull - amount);
    if (ship.hull <= 0) {
        ship.hull = 0;
        ship.active = false;
        world.kills++;
        award(world, static_cast<uint32_t>(stats(ship.cls).score));
        spawn_blast(world, ship.x, ship.y, ship.z, hull_radius(ship.cls) * 3);
    }
}

void hurt_sub(World& world, Ship& ship, Subsystem& sub, int16_t amount) {
    ship.hit_flash = 6;
    sub.hull = static_cast<int16_t>(sub.hull - amount);
    if (sub.hull > 0) return;

    sub.hull = 0;
    world.subs_killed++;
    award(world, 150);

    switch (sub.kind) {
        case Sub::Shields:
            // The bubble collapses on the spot, and nothing brings it back.
            ship.shield = 0;
            break;
        case Sub::Navigation:
            // On the frigate this is the mission. It is not going anywhere
            // now, and no later damage un-stops it.
            if (ship.cls == Hull::Frigate) world.jump_stopped = true;
            break;
        case Sub::LifeSupport:
            // Nobody is left to fly it, and nobody is left to run the shield
            // either. Dropping the bubble here matters more than it looks: the
            // decay below is a single point every forty five ticks, and a
            // shield generator feeding one point back every thirty outran it
            // exactly, so a derelict sat there indefinitely looking dead and
            // never coming apart.
            ship.task = Task::Derelict;
            ship.shield = 0;
            break;
        default:
            break;
    }
}

// The one place a shot decides what it hit on a ship.
//
// Four steps, in this order, and the order is the mechanic:
//
//   1. The bounding sphere, as a cheap reject. Most bolt and ship pairs never
//      get past this and it costs three subtractions.
//   2. The hardpoint the player has actually SELECTED, tested with a generous
//      radius. This is an aiming aid and it is deliberate: the screen is 120
//      pixels across, a turret on a frigate at eighty units is two pixels, and
//      without it "target the engines and shoot the engines" is a thing the
//      game asks for and does not let you do. Selecting a hardpoint is the
//      player saying which part they mean, so the game takes them at their
//      word within a couple of hull widths.
//   3. Any other live hardpoint whose own sphere the shot went through, at its
//      true size. This is how a turret is destroyed by someone who never
//      selected it.
//   4. The plating, as a box for the capitals and a sphere for everything
//      else.
bool apply_hit_to_ship(World& world, Ship& ship, uint8_t index, int32_t px,
                       int32_t py, int32_t pz, int32_t dx, int32_t dy,
                       int32_t dz, int16_t damage, bool from_player) {
    if (!segment_hits_sphere(px, py, pz, dx, dy, dz, ship.x, ship.y, ship.z,
                             hull_radius(ship.cls))) {
        return false;
    }

    auto sub_hit = [&](const Subsystem& sub, int32_t scale_num,
                       int32_t scale_den) {
        int32_t sx, sy, sz;
        sub_position(ship, sub, sx, sy, sz);
        const int32_t radius =
            (sub_radius(ship, sub) * scale_num) / scale_den;
        return segment_hits_sphere(px, py, pz, dx, dy, dz, sx, sy, sz, radius);
    };

    if (from_player && world.target == static_cast<int8_t>(index) &&
        world.target_sub >= 0 &&
        world.target_sub < static_cast<int8_t>(ship.sub_count)) {
        Subsystem& aimed = ship.subs[world.target_sub];
        if (sub_alive(aimed) && sub_hit(aimed, 5, 2)) {
            hurt_sub(world, ship, aimed, damage);
            return true;
        }
    }

    for (uint8_t s = 0; s < ship.sub_count; s++) {
        Subsystem& sub = ship.subs[s];
        if (!sub_alive(sub)) continue;
        if (sub_hit(sub, 1, 1)) {
            hurt_sub(world, ship, sub, damage);
            return true;
        }
    }

    const HullBox& box = k_hull_box[static_cast<int>(ship.cls)];
    if (box.hx > 0 &&
        !segment_hits_box(ship, px, py, pz, dx, dy, dz, box)) {
        return false;
    }

    hurt_ship(world, ship, damage);
    return true;
}

// ---- spawning ----

Ship* free_ship(World& world) {
    for (uint8_t i = 0; i < k_max_ships; i++) {
        if (!world.ships[i].active) return &world.ships[i];
    }
    return nullptr;
}

void spawn_wave(World& world, uint8_t wave) {
    if (wave == 0 || wave > k_wave_count) return;
    const WaveSpec& spec = k_waves[wave - 1];

    int32_t fwd[3], right[3], up[3];
    basis_of(world.q, fwd, right, up);

    for (uint8_t i = 0; i < spec.count; i++) {
        const Spawn& s = spec.ships[i];
        Ship* ship = free_ship(world);
        if (ship == nullptr) return;

        *ship = Ship{};
        ship->active = true;
        ship->cls = s.cls;

        const int32_t ahead = units(s.ahead);
        const int32_t across = units(s.right);
        const int32_t above = units(s.up);
        ship->x = world.x + along(fwd[0], ahead) + along(right[0], across) +
                  along(up[0], above);
        ship->y = world.y + along(fwd[1], ahead) + along(right[1], across) +
                  along(up[1], above);
        ship->z = world.z + along(fwd[2], ahead) + along(right[2], across) +
                  along(up[2], above);

        // Facing the player, which is where they came from.
        ship->q = pse::quat_identity();
        face_toward(ship->q, world.x - ship->x, world.y - ship->y,
                    world.z - ship->z);

        const HullStats& st = stats(s.cls);
        ship->hull = ship->hull_max = st.hull;
        ship->shield = ship->shield_max = st.shield;
        ship->speed = st.speed;
        ship->task = Task::Pursue;
        ship->pass_shots = 0;
        // Staggered, so a wave does not open with every gun on the same tick.
        ship->reload = static_cast<uint16_t>(90 + (next_random(world) % 120));

        const SubLayout& layout = k_layouts[static_cast<int>(s.cls)];
        ship->sub_count = layout.count;
        for (uint8_t k = 0; k < layout.count; k++) {
            const SubSpec& sp = layout.specs[k];
            Subsystem& sub = ship->subs[k];
            sub.kind = sp.kind;
            sub.ox = sp.ox; sub.oy = sp.oy; sub.oz = sp.oz;
            sub.hull = sub.hull_max = sp.hull;
            sub.radius = sp.radius;
        }
    }
}

uint8_t live_enemies(const World& world) {
    uint8_t count = 0;
    for (uint8_t i = 0; i < k_max_ships; i++) {
        if (world.ships[i].active) count++;
    }
    return count;
}

// ---- the player ----

void tick_player(World& world, const Input& input) {
    // Commanded rates, approached rather than jumped to. A craft that reaches
    // full rate in one tick reads as a cursor rather than as something with
    // mass, and the ramp is the whole difference.
    const int32_t want_x = -static_cast<int32_t>(input.pitch) * k_pitch_rate;
    const int32_t want_y = static_cast<int32_t>(input.yaw) * k_yaw_rate;
    const int32_t want_z = -static_cast<int32_t>(input.roll) * k_roll_rate;

    world.wx += ((want_x - world.wx) * k_rate_gain) / 256;
    world.wy += ((want_y - world.wy) * k_rate_gain) / 256;
    world.wz += ((want_z - world.wz) * k_rate_gain) / 256;

    world.q = pse::quat_integrate(world.q, world.wx, world.wy, world.wz);

    // The lever moves at the speed of a thumb, and the ship follows it at the
    // speed of a ship. Easing the speed rather than the lever is what makes a
    // hard deceleration read as mass instead of as a brake.
    world.throttle += static_cast<int32_t>(input.throttle) * k_throttle_step;
    world.throttle = clamp32(world.throttle, 0, k_throttle_one);

    const int32_t commanded =
        static_cast<int32_t>((static_cast<int64_t>(world.throttle) *
                              k_player_speed_max) / k_throttle_one);
    world.speed += ((commanded - world.speed) * k_speed_gain) / 256;

    int32_t fwd[3], right[3], up[3];
    basis_of(world.q, fwd, right, up);

    world.x += along(fwd[0], world.speed);
    world.y += along(fwd[1], world.speed);
    world.z += along(fwd[2], world.speed);

    // A soft wall rather than a clamp. Beyond the arena the outward part of
    // the velocity is simply not applied, so the ship slides along the
    // boundary instead of stopping dead against it, and the HUD says so.
    world.out_of_bounds = false;
    int32_t* axis[3] = {&world.x, &world.y, &world.z};
    for (int a = 0; a < 3; a++) {
        if (*axis[a] > k_arena_half) {
            *axis[a] = k_arena_half;
            world.out_of_bounds = true;
        } else if (*axis[a] < -k_arena_half) {
            *axis[a] = -k_arena_half;
            world.out_of_bounds = true;
        }
    }

    // Shields recharge, hull does not. Letting them come back is the reason
    // to break off, and the only reason there is one.
    if (world.shield_idle < 0xFFFF) world.shield_idle++;
    if (world.shield_idle >= k_shield_regen_delay &&
        world.shield < k_player_shield_max &&
        (world.tick % k_shield_regen_period) == 0) {
        world.shield = static_cast<int16_t>(world.shield + k_shield_regen);
        if (world.shield > k_player_shield_max) {
            world.shield = k_player_shield_max;
        }
    }

    if (world.gun_reload > 0) world.gun_reload--;
    if (world.missile_reload > 0) world.missile_reload--;
    if (world.hit_flash > 0) world.hit_flash--;

    if (input.fire && world.gun_reload == 0) {
        world.gun_reload = k_gun_period;
        // Both barrels aim at the convergence point rather than straight
        // ahead. Parallel bolts straddle anything nearer than infinity, so a
        // player putting the crosshair on a fighter would watch both shots go
        // by either side of it and conclude the guns were broken.
        const int32_t aim_x = world.x + along(fwd[0], k_gun_convergence);
        const int32_t aim_y = world.y + along(fwd[1], k_gun_convergence);
        const int32_t aim_z = world.z + along(fwd[2], k_gun_convergence);
        for (int side = -1; side <= 1; side += 2) {
            const int32_t offset = side * k_gun_offset;
            const int32_t bx = world.x + along(right[0], offset);
            const int32_t by = world.y + along(right[1], offset);
            const int32_t bz = world.z + along(right[2], offset);
            int32_t dx, dy, dz;
            if (!normalize_q14(aim_x - bx, aim_y - by, aim_z - bz, dx, dy, dz)) {
                continue;
            }
            spawn_shot(world, Bolt::PlayerGun, bx, by, bz, dx, dy, dz,
                       k_gun_speed, k_gun_life, k_gun_damage);
        }
    }

    if (input.launch && world.missile_reload == 0 && world.missiles > 0 &&
        world.target >= 0 && world.ships[world.target].active) {
        for (uint8_t i = 0; i < k_max_missiles; i++) {
            Missile& m = world.missiles_live[i];
            if (m.active) continue;
            m.active = true;
            m.x = world.x; m.y = world.y; m.z = world.z;
            m.q = world.q;
            m.life = k_missile_life;
            m.target = world.target;
            m.target_sub = world.target_sub;
            world.missiles--;
            world.missile_reload = k_missile_period;
            break;
        }
    }
}

// ---- enemies ----

// Where to shoot so the bolt and the player arrive together. First order
// lead: the player's velocity is constant along their nose, so the crossing
// is one division and no iteration.
void lead_player(const World& world, int32_t from_x, int32_t from_y,
                 int32_t from_z, int32_t shot_speed, int32_t& ax, int32_t& ay,
                 int32_t& az) {
    const int32_t range = distance(from_x, from_y, from_z, world.x, world.y,
                                   world.z);
    const int32_t flight = shot_speed > 0 ? range / shot_speed : 0;

    int32_t fwd[3];
    forward_of(world.q, fwd);
    // The speed the player is ACTUALLY making, not the maximum. Leading a
    // stationary ship as though it were at full ahead puts every shot in
    // front of it, which reads as enemies that cannot shoot straight.
    ax = world.x + along(fwd[0], world.speed * flight);
    ay = world.y + along(fwd[1], world.speed * flight);
    az = world.z + along(fwd[2], world.speed * flight);
}

void fire_from(World& world, int32_t x, int32_t y, int32_t z, Bolt kind,
               int32_t speed, uint16_t life, int16_t damage) {
    int32_t ax, ay, az;
    lead_player(world, x, y, z, speed, ax, ay, az);
    int32_t dx, dy, dz;
    if (!normalize_q14(ax - x, ay - y, az - z, dx, dy, dz)) return;
    spawn_shot(world, kind, x, y, z, dx, dy, dz, speed, life, damage);
}

void tick_fighter(World& world, Ship& ship, int32_t range) {
    const HullStats& st = stats(ship.cls);
    const bool can_turn = has_capability(ship, Sub::Navigation);

    // A bomber breaks off much further out than a fighter does. It is not
    // trying to get on your six, it is trying to stay at the range its
    // torpedoes work from and not be somewhere a fighter's guns can hold it.
    const int32_t close_range =
        ship.cls == Hull::Bomber ? units(50) : k_break_close;
    const int32_t rate =
        ship.cls == Hull::Bomber ? k_turn_bomber : k_turn_fighter;

    if (ship.task == Task::Derelict) return;

    // Hurt badly enough and it stops fighting. Checked before the state
    // machine and never unset: a ship that has decided to run does not change
    // its mind because the player backed off.
    if (ship.task != Task::Retreat &&
        static_cast<int32_t>(ship.hull) * 100 <
            static_cast<int32_t>(ship.hull_max) * k_retreat_hull_percent) {
        ship.task = Task::Retreat;
        ship.task_ticks = 0;
    }

    ship.task_ticks++;

    switch (ship.task) {
        case Task::Pursue:
            // Two ways to end a pass. Range, so a contact that closes to knife
            // fighting goes through instead of trying to stay there, and shot
            // count, so one that never quite closes still leaves eventually
            // rather than sitting on the player's nose forever.
            if (range < close_range || ship.pass_shots >= k_shots_per_pass) {
                ship.task = Task::Break;
                ship.task_ticks = 0;
                ship.pass_shots = 0;
                ship.break_ticks = static_cast<uint16_t>(
                    k_break_ticks_min + (next_random(world) % k_break_ticks_span));
            }
            break;

        case Task::Break:
            // Actively turning AWAY, not merely holding the heading. Holding
            // it was the whole bug: a contact that stops steering next to you
            // is still next to you, and the moment it resumed it was already
            // pointing at you again.
            if (can_turn) {
                turn_toward(ship.q, ship.x - world.x, ship.y - world.y,
                            ship.z - world.z, rate);
            }
            if (ship.task_ticks > ship.break_ticks || range > k_break_range) {
                ship.task = Task::Pursue;
                ship.task_ticks = 0;
            }
            // Guns cold on the way out. This is the player's window, and a
            // contact still shooting over its shoulder does not give them one.
            return;

        case Task::Retreat:
            if (can_turn) {
                turn_toward(ship.q, ship.x - world.x, ship.y - world.y,
                            ship.z - world.z, rate);
            }
            if (range > k_escape_range) {
                ship.active = false;
                world.routed++;
                award(world, static_cast<uint32_t>(stats(ship.cls).score) /
                                 k_rout_score_divisor);
            }
            return;

        case Task::Derelict:
            return;
    }

    if (can_turn) {
        turn_toward(ship.q, world.x - ship.x, world.y - ship.y,
                    world.z - ship.z, rate);
    }

    if (ship.reload > 0) return;

    int32_t fwd[3];
    forward_of(ship.q, fwd);
    int32_t dx, dy, dz;
    if (!normalize_q14(world.x - ship.x, world.y - ship.y, world.z - ship.z,
                       dx, dy, dz)) {
        return;
    }
    const int32_t aligned =
        (static_cast<int32_t>(fwd[0]) * dx + fwd[1] * dy + fwd[2] * dz) / k_q14;

    // Roughly a five degree cone, and inside gun range. A fighter that fires
    // whenever the player is vaguely ahead is unmissable and unfun.
    if (aligned < 15900 || range > st.fire_range) return;

    ship.reload = st.fire_period;
    ship.pass_shots++;
    const int32_t nose = ship.cls == Hull::Bomber ? centi(180) : centi(110);
    fire_from(world, ship.x + along(fwd[0], nose), ship.y + along(fwd[1], nose),
              ship.z + along(fwd[2], nose), Bolt::EnemyGun, k_bolt_speed,
              k_bolt_life, k_bolt_damage);
}

void tick_capital(World& world, Ship& ship, int32_t range) {
    const HullStats& st = stats(ship.cls);

    // A capital ship does not dogfight. It holds a heading toward the player
    // and lets its turrets do the work, and with its navigation shot away it
    // cannot even do that.
    if (has_capability(ship, Sub::Navigation)) {
        turn_toward(
            ship.q, world.x - ship.x, world.y - ship.y, world.z - ship.z,
            ship.cls == Hull::Frigate ? k_turn_frigate : k_turn_gunship);
    }

    if (ship.reload > 0) {
        ship.reload--;
        return;
    }
    if (range > st.fire_range) return;

    // Turrets take turns. Firing every gun on the same tick makes a frigate a
    // shotgun rather than a warship, and it wastes the whole point of being
    // able to shoot one of them off.
    uint8_t turret_count = 0;
    for (uint8_t s = 0; s < ship.sub_count; s++) {
        if (ship.subs[s].kind == Sub::Weapons && sub_alive(ship.subs[s])) {
            turret_count++;
        }
    }
    if (turret_count == 0) return;

    const uint8_t pick = ship.next_turret % turret_count;
    ship.next_turret = static_cast<uint8_t>((ship.next_turret + 1) % turret_count);

    uint8_t seen = 0;
    for (uint8_t s = 0; s < ship.sub_count; s++) {
        Subsystem& sub = ship.subs[s];
        if (sub.kind != Sub::Weapons || !sub_alive(sub)) continue;
        if (seen++ != pick) continue;
        int32_t tx, ty, tz;
        sub_position(ship, sub, tx, ty, tz);
        ship.reload = st.fire_period;
        fire_from(world, tx, ty, tz, Bolt::TurretShell, k_shell_speed,
                  k_shell_life, k_shell_damage);
        return;
    }
}

void tick_ships(World& world) {
    for (uint8_t i = 0; i < k_max_ships; i++) {
        Ship& ship = world.ships[i];
        if (!ship.active) continue;

        if (ship.hit_flash > 0) ship.hit_flash--;
        if (ship.reload > 0 && ship.cls != Hull::Gunship &&
            ship.cls != Hull::Frigate) {
            ship.reload--;
        }

        const int32_t range = range_to(world, ship);

        if (ship.task == Task::Derelict) {
            // Nobody is flying it. It coasts, and it comes apart. Straight
            // into the hull: this is the structure failing from the inside,
            // and a shield is not something that stops that.
            if ((world.tick % k_derelict_period) == 0) {
                ship.hull = static_cast<int16_t>(ship.hull - 1);
                if (ship.hull <= 0) {
                    ship.hull = 0;
                    ship.active = false;
                    world.kills++;
                    award(world, static_cast<uint32_t>(stats(ship.cls).score));
                    spawn_blast(world, ship.x, ship.y, ship.z,
                                hull_radius(ship.cls) * 3);
                    continue;
                }
            }
        } else if (ship.cls == Hull::Gunship || ship.cls == Hull::Frigate) {
            tick_capital(world, ship, range);
        } else {
            tick_fighter(world, ship, range);
        }

        // Shot off engines are shot off engines, whatever the ship wanted.
        const int32_t speed = has_capability(ship, Sub::Engines) ? ship.speed : 0;
        if (speed > 0) {
            int32_t fwd[3];
            forward_of(ship.q, fwd);
            ship.x += along(fwd[0], speed);
            ship.y += along(fwd[1], speed);
            ship.z += along(fwd[2], speed);
        }

        // A capital's shield generator feeds the bubble back up, slowly, and
        // only while it is still there.
        if (ship.shield < ship.shield_max && has_capability(ship, Sub::Shields) &&
            ship.sub_count > 0 && (world.tick % 30) == 0) {
            ship.shield++;
        }

        // Ramming. Both sides lose, and the player loses more, which is the
        // only thing stopping a fighter being a battering ram. Charged once
        // per contact: see Ship::touching.
        const int32_t contact = hull_radius(ship.cls) + k_player_radius;
        const bool touching = range < contact;
        if (touching && !ship.touching) {
            hurt_player(world, 22);
            hurt_ship(world, ship, 30);
        }
        ship.touching = touching;
    }
}

// ---- shots and missiles ----

void tick_shots(World& world) {
    for (uint8_t i = 0; i < k_max_bolts; i++) {
        Shot& shot = world.shots[i];
        if (!shot.active) continue;
        if (shot.life == 0) { shot.active = false; continue; }
        shot.life--;

        const int32_t px = shot.x, py = shot.y, pz = shot.z;
        shot.x += shot.vx;
        shot.y += shot.vy;
        shot.z += shot.vz;

        if (shot.kind == Bolt::PlayerGun) {
            for (uint8_t s = 0; s < k_max_ships; s++) {
                Ship& ship = world.ships[s];
                if (!ship.active) continue;
                if (apply_hit_to_ship(world, ship, s, px, py, pz, shot.vx,
                                      shot.vy, shot.vz, shot.damage, true)) {
                    shot.active = false;
                    break;
                }
            }
        } else if (segment_hits_sphere(px, py, pz, shot.vx, shot.vy, shot.vz,
                                       world.x, world.y, world.z,
                                       k_player_radius)) {
            shot.active = false;
            hurt_player(world, shot.damage);
        }
    }
}

void tick_missiles(World& world) {
    for (uint8_t i = 0; i < k_max_missiles; i++) {
        Missile& m = world.missiles_live[i];
        if (!m.active) continue;
        if (m.life == 0) { m.active = false; continue; }
        m.life--;

        // Where it is steering: the hardpoint if it was launched at one, the
        // hull otherwise. Chasing a subsystem is how a fighter takes a turret
        // off something its guns would spend a minute on.
        bool have_aim = false;
        int32_t ax = 0, ay = 0, az = 0;
        if (m.target >= 0 && world.ships[m.target].active) {
            const Ship& ship = world.ships[m.target];
            have_aim = true;
            if (m.target_sub >= 0 && m.target_sub < ship.sub_count &&
                sub_alive(ship.subs[m.target_sub])) {
                sub_position(ship, ship.subs[m.target_sub], ax, ay, az);
            } else {
                ax = ship.x; ay = ship.y; az = ship.z;
            }
        }
        if (have_aim) {
            turn_toward(m.q, ax - m.x, ay - m.y, az - m.z, k_missile_turn);
        }

        int32_t fwd[3];
        forward_of(m.q, fwd);
        const int32_t px = m.x, py = m.y, pz = m.z;
        const int32_t dx = along(fwd[0], k_missile_speed);
        const int32_t dy = along(fwd[1], k_missile_speed);
        const int32_t dz = along(fwd[2], k_missile_speed);
        m.x += dx; m.y += dy; m.z += dz;

        // Armed only once it is clear of the launching ship, so a missile
        // fired at something already on top of you does not detonate on the
        // rail.
        const int32_t flown = k_missile_life - m.life;
        if (static_cast<int32_t>(flown) * k_missile_speed < k_missile_arm) {
            continue;
        }

        for (uint8_t s = 0; s < k_max_ships; s++) {
            Ship& ship = world.ships[s];
            if (!ship.active) continue;
            if (apply_hit_to_ship(world, ship, s, px, py, pz, dx, dy, dz,
                                  k_missile_damage, true)) {
                m.active = false;
                break;
            }
        }
    }
}

// ---- targeting ----

void drop_dead_target(World& world) {
    if (world.target < 0) return;
    if (!world.ships[world.target].active) {
        world.target = -1;
        world.target_sub = -1;
    }
}

void cycle(World& world) {
    // Same ship, next live hardpoint. This is the "press it again on the
    // thing you already have" half of the control: a frigate is six presses
    // deep, and the seventh moves on.
    if (world.target >= 0 && world.ships[world.target].active) {
        const Ship& ship = world.ships[world.target];
        for (int8_t s = static_cast<int8_t>(world.target_sub + 1);
             s < static_cast<int8_t>(ship.sub_count); s++) {
            if (sub_alive(ship.subs[s])) {
                world.target_sub = s;
                return;
            }
        }
    }

    int8_t order[k_max_ships];
    const uint8_t count = target_order(world, order);
    if (count == 0) {
        world.target = -1;
        world.target_sub = -1;
        return;
    }

    // The next one along, wrapping. Where the current target sits in the
    // order is looked up rather than remembered: the order is recomputed from
    // the ships' actual positions on every press, so a remembered index would
    // be an index into a list that no longer exists.
    uint8_t at = count - 1;
    for (uint8_t i = 0; i < count; i++) {
        if (order[i] == world.target) { at = i; break; }
    }
    world.target = order[(at + 1) % count];
    world.target_sub = -1;
}

// ---- the mission ----

void tick_mission(World& world) {
    if (world.phase == Phase::Briefing) {
        if (world.wave_timer > 0) {
            world.wave_timer--;
            return;
        }
        spawn_wave(world, world.wave);
        world.phase = Phase::Fighting;
        return;
    }

    if (world.phase != Phase::Fighting) return;

    // The frigate's clock. It runs while there is a frigate on the field with
    // its navigation intact and somebody alive to fly it, and stopping it is
    // the only thing that has to happen in the whole sortie.
    bool frigate_charging = false;
    for (uint8_t i = 0; i < k_max_ships; i++) {
        const Ship& ship = world.ships[i];
        if (!ship.active || ship.cls != Hull::Frigate) continue;
        if (world.jump_stopped || ship.task == Task::Derelict) continue;
        if (!has_capability(ship, Sub::Navigation)) continue;
        frigate_charging = true;
    }
    if (frigate_charging) {
        world.jump_charge++;
        if (world.jump_charge >= k_jump_charge) {
            world.phase = Phase::Lost;
            world.loss = Loss::Jumped;
            return;
        }
    }

    if (live_enemies(world) > 0) return;

    if (world.wave >= k_wave_count) {
        world.phase = Phase::Won;
        return;
    }
    world.wave++;
    world.phase = Phase::Briefing;
    world.wave_timer = k_wave_gap;
    // A fresh pair of missiles between engagements. Not a full rack: running
    // the rack dry has to still cost something.
    world.missiles = static_cast<uint8_t>(
        world.missiles + 2 > k_missiles_max ? k_missiles_max : world.missiles + 2);
}

}  // namespace

// ---- the public surface ----

const char* sub_name(Sub kind) {
    switch (kind) {
        case Sub::Navigation:  return "NAV";
        case Sub::Shields:     return "SHIELDS";
        case Sub::Weapons:     return "TURRET";
        case Sub::Engines:     return "ENGINES";
        case Sub::LifeSupport: return "LIFE SUP";
        default:               return "HULL";
    }
}

const char* hull_name(Hull cls) {
    switch (cls) {
        case Hull::Fighter: return "FIGHTER";
        case Hull::Bomber:  return "BOMBER";
        case Hull::Gunship: return "GUNSHIP";
        case Hull::Frigate: return "FRIGATE";
        default:            return "CONTACT";
    }
}

int32_t hull_radius(Hull cls) { return stats(cls).radius; }
int32_t hull_length(Hull cls) { return stats(cls).length; }

void player_basis(const World& world, pse::Basis& out) {
    out = pse::quat_basis(world.q);
}

void sub_position(const Ship& ship, const Subsystem& sub, int32_t& ox,
                  int32_t& oy, int32_t& oz) {
    // The seat is in thousandths of the model, in the ship's own frame. Turned
    // by the ship's attitude, so a turret stays on its sponson however the
    // hull is pointing, then scaled by how big this class of ship is drawn.
    int32_t rx, ry, rz;
    pse::quat_rotate(ship.q, sub.ox, sub.oy, sub.oz, rx, ry, rz);
    const int32_t length = stats(ship.cls).length;
    ox = ship.x + static_cast<int32_t>(
                      (static_cast<int64_t>(rx) * length) / 1000);
    oy = ship.y + static_cast<int32_t>(
                      (static_cast<int64_t>(ry) * length) / 1000);
    oz = ship.z + static_cast<int32_t>(
                      (static_cast<int64_t>(rz) * length) / 1000);
}

int32_t sub_radius(const Ship& ship, const Subsystem& sub) {
    return static_cast<int32_t>(
        (static_cast<int64_t>(sub.radius) * stats(ship.cls).length) / 1000);
}

int32_t distance(int32_t ax, int32_t ay, int32_t az, int32_t bx, int32_t by,
                 int32_t bz) {
    const int64_t dx = ax - bx, dy = ay - by, dz = az - bz;
    return static_cast<int32_t>(
        isqrt64(static_cast<uint64_t>(dx * dx + dy * dy + dz * dz)));
}

int32_t range_to(const World& world, const Ship& ship) {
    return distance(world.x, world.y, world.z, ship.x, ship.y, ship.z);
}

int32_t alignment(const World& world, int32_t x, int32_t y, int32_t z) {
    int32_t dx, dy, dz;
    if (!normalize_q14(x - world.x, y - world.y, z - world.z, dx, dy, dz)) {
        return k_q14;
    }
    int32_t fwd[3];
    forward_of(world.q, fwd);
    return (fwd[0] * dx + fwd[1] * dy + fwd[2] * dz) / k_q14;
}

void bearing(const World& world, int32_t x, int32_t y, int32_t z, int32_t& bx,
             int32_t& by, int32_t& bz) {
    int32_t nx, ny, nz;
    if (!normalize_q14(x - world.x, y - world.y, z - world.z, nx, ny, nz)) {
        bx = 0; by = 0; bz = k_q14;
        return;
    }
    pse::quat_unrotate(world.q, nx, ny, nz, bx, by, bz);
}

bool has_capability(const Ship& ship, Sub kind) {
    bool carried = false;
    for (uint8_t s = 0; s < ship.sub_count; s++) {
        if (ship.subs[s].kind != kind) continue;
        carried = true;
        if (sub_alive(ship.subs[s])) return true;
    }
    // A fighter has no shield generator to shoot off, and its shields are not
    // therefore down. Only a ship that carries the hardpoint can lose it.
    return !carried;
}

uint8_t target_order(const World& world, int8_t out[k_max_ships]) {
    int32_t keys[k_max_ships];
    uint8_t count = 0;

    for (uint8_t i = 0; i < k_max_ships; i++) {
        const Ship& ship = world.ships[i];
        if (!ship.active) continue;

        const int32_t align = alignment(world, ship.x, ship.y, ship.z);
        int32_t key;
        if (align >= k_view_cos) {
            // In the forward cone: nearest wins. The million keeps this whole
            // group above the out of view group, whose keys are a Q14 cosine
            // and so can never exceed 16384.
            const int32_t range_units = range_to(world, ship) >> k_fp;
            key = 1000000 - range_units;
        } else {
            key = align;
        }

        // Insertion sort, descending. At most twelve contacts, so the simple
        // thing is also the fast thing.
        uint8_t at = count;
        while (at > 0 && keys[at - 1] < key) {
            keys[at] = keys[at - 1];
            out[at] = out[at - 1];
            at--;
        }
        keys[at] = key;
        out[at] = static_cast<int8_t>(i);
        count++;
    }
    return count;
}

const Ship* target_ship(const World& world) {
    if (world.target < 0 || world.target >= static_cast<int8_t>(k_max_ships)) {
        return nullptr;
    }
    const Ship& ship = world.ships[world.target];
    return ship.active ? &ship : nullptr;
}

const Subsystem* target_subsystem(const World& world) {
    const Ship* ship = target_ship(world);
    if (ship == nullptr || world.target_sub < 0) return nullptr;
    if (world.target_sub >= static_cast<int8_t>(ship->sub_count)) return nullptr;
    return &ship->subs[world.target_sub];
}

uint32_t jump_ticks_left(const World& world) {
    if (world.jump_stopped) return 0;
    bool present = false;
    for (uint8_t i = 0; i < k_max_ships; i++) {
        if (world.ships[i].active && world.ships[i].cls == Hull::Frigate) {
            present = true;
        }
    }
    if (!present) return 0;
    return world.jump_charge >= k_jump_charge ? 0
                                              : k_jump_charge - world.jump_charge;
}

void world_init(World& world, uint32_t seed) {
    world = World{};
    world.rng = seed == 0 ? 0x5A1CE001u : seed;
    world.q = pse::quat_identity();
    // Launched at full ahead. The throttle is there to be pulled BACK, to
    // turn inside something: opening at a standstill would just be a game
    // that starts by not moving.
    world.throttle = k_throttle_one;
    world.speed = k_player_speed_max;
    world.hull = k_player_hull_max;
    world.shield = k_player_shield_max;
    world.missiles = k_missiles_max;
    world.target = -1;
    world.target_sub = -1;
    world.phase = Phase::Briefing;
    world.loss = Loss::None;
    world.wave = 1;
    world.wave_timer = 90;
}

void world_tick(World& world, const Input& input) {
    if (!in_flight(world)) return;

    world.tick++;

    tick_player(world, input);
    tick_ships(world);
    tick_shots(world);
    tick_missiles(world);
    tick_blasts(world);
    tick_mission(world);

    drop_dead_target(world);
    if (input.cycle_target) cycle(world);
}

}  // namespace sd
