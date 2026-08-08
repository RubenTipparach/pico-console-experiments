#pragma once

// Star Dancer: the flight model, the guns, the enemy ships and the mission.
//
// Pure integer C++ with no SDK, no floats and no allocation, which is what
// lets the host tests prove the things a comment can only claim: that a rolled
// ship still pitches about its own nose, that a bolt fired at a turret damages
// that turret and not the hull beside it, that killing a frigate's navigation
// really does stop the jump, and that a battle is a pure function of its
// inputs.
//
// Axes: +x right, +y up, +z forward at rest. A ship's attitude is a unit
// quaternion, not a set of angles, for the reason pse/quat.hpp sets out at
// length: the stick commands rates about the ship's OWN axes, and Euler
// angles cannot carry that. Roll a fighter ninety degrees and pull back, and
// with Euler rates it yaws. With a quaternion it turns where the nose is
// pointing, at every attitude, which for a space sim is the entire game.
//
// Units and tick length are in tuning.hpp. One tick is 10 ms.

#include <cstdint>

#include "pse/quat.hpp"

#include "tuning.hpp"

namespace sd {

// ---- ships ----

enum class Hull : uint8_t {
    Fighter,     // fast, fragile, no subsystems worth naming
    Bomber,      // slow, tough, throws torpedoes at you
    Gunship,     // a small warship: turrets, engines, a shield generator
    Frigate,     // the capital ship, and the one with everything on it
    HullCount,
};

// What a hardpoint is for. The order is the order they are listed on a ship
// and the order the HUD cycles them in, nose to tail, which is also roughly
// the order they matter in.
enum class Sub : uint8_t {
    Navigation,
    Shields,
    Weapons,        // a turret. A ship may carry several.
    Engines,
    LifeSupport,
    SubKindCount,
};

constexpr uint8_t k_max_subs = 6;
constexpr uint8_t k_max_ships = 12;
constexpr uint8_t k_max_bolts = 40;
constexpr uint8_t k_max_missiles = 6;

// The name a HUD prints for a hardpoint. Here rather than in the renderer
// because the sim is what decides a subsystem exists at all, and a second
// table somewhere else is a second place for it to go stale.
const char* sub_name(Sub kind);
const char* hull_name(Hull cls);

// One hardpoint on a ship.
//
// `ox, oy, oz` are its seat in the ship's OWN frame, and `radius` the sphere a
// shot has to enter to be counted against it rather than against the plating.
// Both are in THOUSANDTHS OF THE MODEL, not in world units: the .obj files are
// all authored exactly one unit long, so a seat of 388 means 0.388 along the
// model's own z, whatever size that class of ship is drawn at.
//
// Written that way because the alternative drifted. With the seats in absolute
// world units, hull_length() was two numbers pretending to be one: making the
// frigate bigger moved the picture and left the turrets behind, on a hull they
// were no longer attached to, and nothing said so. Now one number decides how
// big a frigate is and everything that sits on one follows it.
//
// int16 because six of these per ship times twelve ships is RAM spent whether
// or not a frigate is on the field.
struct Subsystem {
    Sub kind;
    int16_t ox, oy, oz;
    int16_t hull;
    int16_t hull_max;
    int16_t radius;
};

enum class Task : uint8_t {
    Pursue,      // turn onto the player and close, guns free
    Break,       // burst spent or too close: turn away, run, come about
    Retreat,     // too badly hurt to fight: leave, and keep leaving
    Derelict,    // life support gone, nobody flying it
};

struct Ship {
    bool active;
    Hull cls;

    int32_t x, y, z;              // fp16
    pse::Quat q;
    int32_t speed;                // fp16 per tick, along the ship's own +z

    int16_t hull, hull_max;
    int16_t shield, shield_max;

    Task task;
    uint16_t task_ticks;          // how long the current task has run
    // How long THIS break lasts, rolled when it starts so every contact does
    // not come about on the same tick.
    uint16_t break_ticks;
    // Shots put down on this pass. A pass ends on the count or on the range,
    // and without the count a fighter that never quite closes never leaves.
    uint8_t pass_shots;
    uint16_t reload;              // ticks until the next shot
    uint16_t hit_flash;           // ticks of white left on the model

    // Was this hull touching the player last tick? A collision costs both
    // sides on the tick it STARTS and not on every tick it lasts. Without the
    // edge, flying into a frigate is not a collision, it is a grinder: the
    // contact holds for as long as it takes to pass through twenty six units
    // of ship, and charging one was an instant and unavoidable death.
    bool touching;

    uint8_t sub_count;
    Subsystem subs[k_max_subs];

    // Which turret fires next, so a frigate's four guns take turns rather
    // than all four firing on the same tick.
    uint8_t next_turret;
};

// ---- shots ----

enum class Bolt : uint8_t { PlayerGun, EnemyGun, TurretShell };

struct Shot {
    bool active;
    Bolt kind;
    int32_t x, y, z;
    int32_t vx, vy, vz;           // fp16 per tick
    uint16_t life;
    int16_t damage;
};

struct Missile {
    bool active;
    int32_t x, y, z;
    pse::Quat q;
    uint16_t life;
    // Which ship it is chasing, and which hardpoint on it. A missile locked
    // to a subsystem flies at the subsystem, which is how a turret gets taken
    // off a frigate that a fighter's guns would take a minute to chew
    // through.
    int8_t target;
    int8_t target_sub;
};

// A hull coming apart, so the renderer has something to draw and the tests
// have something to check.
//
// It lives in the sim rather than in the renderer because the alternative was
// the renderer noticing that a ship which was in the table last frame is not
// in it now, which is a guess: the slot is reused, so a kill on the same tick
// a wave arrives reads as no kill at all. The sim knows a ship died, at a
// position, on a tick. It costs five slots of twenty bytes.
struct Blast {
    bool active;
    int32_t x, y, z;
    uint16_t life;       // ticks left
    // Widest radius, in hundredths of a world unit rather than fp16: a
    // frigate's fireball is 39 units across and fp16 would need an int32 to
    // say so, which is four more bytes per slot to hold a number the renderer
    // is going to turn into pixels anyway.
    int16_t size;
};

constexpr uint8_t k_max_blasts = 5;
constexpr uint16_t k_blast_life = 55;

// ---- the mission ----

enum class Phase : uint8_t {
    Briefing,    // the wave has not arrived yet
    Fighting,
    Won,
    Lost,
};

enum class Loss : uint8_t {
    None,
    Destroyed,   // the player's hull went
    Jumped,      // the frigate charged its jump and left
};

// Which sortie is being flown.
//
// Two, and they are different jobs rather than two difficulties of the same
// one. Patrol is fighters and bombers and nothing that carries a hardpoint, so
// it is the whole game minus the half that needs explaining. Assault is the
// one the game is really about: the same opening, then a gunship, then a
// frigate charging a jump, which is where targeting a subsystem stops being a
// curiosity and becomes the thing you have to do.
enum class Mission : uint8_t { Patrol, Assault, MissionCount };

// The longest any sortie runs to, which is what the wave counter is sized
// against. Ask wave_count() for a particular mission's length.
constexpr uint8_t k_max_waves = 5;

uint8_t wave_count(Mission mission);
const char* mission_name(Mission mission);

// ---- input ----
//
// Held states, not edges, except for the two that are genuinely events. The
// stick is analogue in spirit and digital in fact: a direction is either
// commanded or it is not, and the rate ramp in the flight model is what makes
// that feel like a stick rather than a switch.
struct Input {
    int8_t pitch;        // -1 nose down, +1 nose up
    int8_t yaw;          // -1 left, +1 right
    int8_t roll;         // -1 left, +1 right
    int8_t throttle;     // -1 back, +1 forward, held
    bool fire;           // guns, held
    bool launch;         // missile, held (the reload is what limits it)
    // One step, on the tick it is set. The shell raises it when the target
    // button is RELEASED rather than when it goes down, because holding that
    // button means something else: see sdr::Chrome::look_at_target.
    bool cycle_target;
};

struct World {
    uint32_t tick;
    uint32_t rng;

    // ---- the player ----
    int32_t x, y, z;
    pse::Quat q;
    // Current turn rates about the ship's own axes, Q14 radians per tick.
    // Body frame, which is the frame the stick commands in, so nothing is
    // converted before it is integrated.
    int32_t wx, wy, wz;

    // The lever, 0 to k_throttle_one, and the speed actually being made,
    // fp16 per tick. Two numbers rather than one because the ship has mass:
    // the lever moves as fast as the thumb does and the speed follows it.
    int32_t throttle;
    int32_t speed;

    int16_t hull, shield;
    uint16_t shield_idle;        // ticks since the last hit on the player
    uint8_t missiles;
    uint16_t gun_reload;
    uint16_t missile_reload;
    uint16_t hit_flash;
    bool out_of_bounds;

    // ---- the battle ----
    Ship ships[k_max_ships];
    Shot shots[k_max_bolts];
    Missile missiles_live[k_max_missiles];
    Blast blasts[k_max_blasts];

    int8_t target;               // index into ships, or -1
    int8_t target_sub;           // index into that ship's subs, or -1 for the hull

    Mission mission;
    Phase phase;
    Loss loss;
    uint8_t wave;                // 1..wave_count(mission)
    uint16_t wave_timer;         // ticks until the next wave arrives
    uint32_t jump_charge;        // the frigate's subspace clock, counting up
    bool jump_stopped;           // its navigation is gone: it is not leaving

    uint32_t score;
    uint16_t kills;
    // Driven off rather than destroyed: hurt badly enough to run, and far
    // enough out to be gone. Counted apart from kills because letting one go
    // is a different outcome from killing it, and worth less.
    uint16_t routed;
    uint16_t subs_killed;
};

// ---- driving it ----

void world_init(World& world, uint32_t seed = 0x5A1CE001u,
                Mission mission = Mission::Assault);
void world_tick(World& world, const Input& input);

// ---- reading it, for the renderer and the HUD ----

// The player's own axes in world space, Q14. The renderer hangs the camera
// off these and the HUD aims with them, so both agree with the flight model
// by construction.
void player_basis(const World& world, pse::Basis& out);

// Where a hardpoint actually is in the world, fp16. The renderer boxes it,
// the missiles fly at it and the bolts are tested against it, all from here.
void sub_position(const Ship& ship, const Subsystem& sub,
                  int32_t& ox, int32_t& oy, int32_t& oz);

// The hardpoint's sphere in world units, fp16. Its stored radius is in
// thousandths of the model, so how big it really is depends on the class of
// ship carrying it.
int32_t sub_radius(const Ship& ship, const Subsystem& sub);

// Straight line distance, fp16. int64 inside: the arena is 170 units across
// and a squared fp16 separation does not fit an int32.
int32_t distance(int32_t ax, int32_t ay, int32_t az,
                 int32_t bx, int32_t by, int32_t bz);

// Range from the player to a ship, fp16.
int32_t range_to(const World& world, const Ship& ship);

// How far off the player's nose a point sits, as a Q14 cosine: 16384 is dead
// ahead, 0 is abeam, negative is behind. This is what "in my view" means
// everywhere in this game, including in the target cycle.
int32_t alignment(const World& world, int32_t x, int32_t y, int32_t z);

// The unit direction to a point, in the PLAYER'S own frame, Q14: +x to their
// right, +y over their head, +z out of the nose. Which is to say, where a
// thing is from where you are sitting, which is the only frame a cockpit
// instrument can usefully speak in. The off screen target arrow reads it
// straight off, and so does anything asking whether it should turn left.
void bearing(const World& world, int32_t x, int32_t y, int32_t z,
             int32_t& bx, int32_t& by, int32_t& bz);

// The order Y walks contacts in: everything inside the forward cone first,
// nearest first, then everything else, nearest the nose first.
//
// Two keys rather than one, and the second group is why. Sorting the whole
// field by how close it is to the crosshair reads well until a frigate two
// hundred units off ranks above the fighter shooting you from thirty, because
// the frigate happens to be nearer the middle of the screen. Inside the cone
// the useful question is which contact is on top of you; outside it, the only
// useful question is which way you would have to turn least to find one.
//
// Writes indices into `out` and returns how many. Pure, so a test can read
// the order without pressing anything.
uint8_t target_order(const World& world, int8_t out[k_max_ships]);

// The ship currently targeted, or nullptr.
const Ship* target_ship(const World& world);

// The hardpoint currently targeted, or nullptr when the whole ship is.
const Subsystem* target_subsystem(const World& world);

// Is this hardpoint still working? A destroyed one is still drawn (it is a
// hole in the hull, not a hole in space) and is skipped by the target cycle.
inline bool sub_alive(const Subsystem& sub) { return sub.hull > 0; }

// Does this ship still have this capability? A ship that never carried the
// hardpoint at all counts as having it: a fighter has no shield generator to
// shoot off, and its shields are not therefore down.
bool has_capability(const Ship& ship, Sub kind);

// Hull radius for a class, fp16. One table, read by the collision, by the
// renderer's culling, and by the reticle.
int32_t hull_radius(Hull cls);

// How long the model is, fp16, for drawing it at the right scale.
int32_t hull_length(Hull cls);

// Is there still a battle to fly? False once the sortie is decided.
inline bool in_flight(const World& world) {
    return world.phase == Phase::Briefing || world.phase == Phase::Fighting;
}

// Ticks left on the frigate's jump, or 0 when there is no frigate on the
// field or its navigation is already gone.
uint32_t jump_ticks_left(const World& world);

}  // namespace sd
