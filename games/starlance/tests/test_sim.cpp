// Starlance's flight model and combat rules, proven rather than asserted.
//
// The sim is pure integer C++ with no SDK in it, which is the whole reason
// these can run at all: a battle is a function from a seed and a list of
// inputs to a World, so everything below is a claim about that function.

#include <cstdint>
#include <cstdio>

#include "sim.hpp"

namespace {

int g_checks = 0;
int g_failures = 0;

void check(bool ok, const char* expr, int line) {
    g_checks++;
    if (ok) return;
    g_failures++;
    std::printf("FAIL line %d: %s\n", line, expr);
}

#define CHECK(expr) check((expr), #expr, __LINE__)

sl::Input nothing() { return sl::Input{}; }

void run(sl::World& world, int ticks, const sl::Input& in) {
    for (int i = 0; i < ticks; i++) sl::world_tick(world, in);
}

// Put the sortie straight into one wave with nothing else on the field.
void jump_to_wave(sl::World& world, uint8_t wave) {
    world.wave = wave;
    world.phase = sl::Phase::Briefing;
    world.wave_timer = 0;
    sl::world_tick(world, nothing());
}

const sl::Ship* first_of(const sl::World& world, sl::Hull cls) {
    for (uint8_t i = 0; i < sl::k_max_ships; i++) {
        if (world.ships[i].active && world.ships[i].cls == cls) {
            return &world.ships[i];
        }
    }
    return nullptr;
}

int8_t index_of(const sl::World& world, sl::Hull cls) {
    for (uint8_t i = 0; i < sl::k_max_ships; i++) {
        if (world.ships[i].active && world.ships[i].cls == cls) {
            return static_cast<int8_t>(i);
        }
    }
    return -1;
}

int8_t find_sub(const sl::Ship& ship, sl::Sub kind) {
    for (uint8_t s = 0; s < ship.sub_count; s++) {
        if (ship.subs[s].kind == kind) return static_cast<int8_t>(s);
    }
    return -1;
}

// Nobody but the ship under test.
void clear_except(sl::World& world, int8_t keep) {
    for (uint8_t i = 0; i < sl::k_max_ships; i++) {
        if (i != static_cast<uint8_t>(keep)) world.ships[i].active = false;
    }
}

// Hold the player at a standoff from a point with the nose on it, and fire.
//
// The ship cruises at a fixed speed and has no brake, which is the flight
// model working as intended and a nuisance for a test about the guns: left to
// itself it flies into whatever it is shooting at, rams it, and the thing the
// test wanted to measure is buried under collision damage. Re-placing it every
// tick isolates the weapon from the flight.
//
// Returns the tick it stopped on, so a test can tell "it happened" from "it
// ran out of patience".
// `facing` is the attitude to hold, and the standoff is measured back along
// its nose, so a test can pick which side of a ship it attacks from. That is
// not a detail: a frigate's life support pod is slung under the belly with the
// navigation array in the nose and the drives at the tail, so a bolt fired
// down the centre line from ahead is stopped by the nav array every time. The
// pod is reachable from below and nowhere else, which is the geometry working,
// not a bug in it.
template <typename Aim, typename Done>
int hold_and_fire(sl::World& world, const pse::Quat& facing, int32_t standoff,
                  int max_ticks, Aim aim, Done done) {
    int32_t fx, fy, fz;
    pse::quat_rotate(facing, 0, 0, pse::k_quat_one, fx, fy, fz);

    for (int i = 0; i < max_ticks; i++) {
        int32_t tx, ty, tz;
        if (!aim(tx, ty, tz)) return i;
        world.x = tx - static_cast<int32_t>(
                           (static_cast<int64_t>(fx) * standoff) >> 14);
        world.y = ty - static_cast<int32_t>(
                           (static_cast<int64_t>(fy) * standoff) >> 14);
        world.z = tz - static_cast<int32_t>(
                           (static_cast<int64_t>(fz) * standoff) >> 14);
        world.q = facing;
        world.wx = world.wy = world.wz = 0;

        sl::Input fire = nothing();
        fire.fire = true;
        sl::world_tick(world, fire);
        if (done()) return i;
    }
    return max_ticks;
}

// Nose on +z, which is where a ship starts.
pse::Quat facing_ahead() { return pse::quat_identity(); }

// Nose straight up, for attacking something on a ship's underside.
pse::Quat facing_up() {
    return pse::quat_from_axis_angle(1.0f, 0.0f, 0.0f, -1.5708f);
}

// ---- flight ----

// The claim the quaternion is here for: pulling back turns the ship about ITS
// OWN lateral axis, whatever attitude it is already in.
//
// Roll the ship a quarter turn and pull back. With Euler angles the pitch
// command would turn the ship about the world's x axis, which after a ninety
// degree roll is the ship's YAW axis, so the nose would swing sideways
// instead of up. With a quaternion it goes where the nose is pointing. The
// measurement is the same in both cases and only one of them passes.
void test_a_rolled_ship_still_pitches_about_its_own_nose() {
    sl::World world;
    sl::world_init(world);

    // Roll ninety degrees to the right, then stop and settle.
    sl::Input roll = nothing();
    roll.roll = 1;
    // k_roll_rate is Q14 radians a tick; a quarter turn is 1.5708 radians.
    const int roll_ticks = static_cast<int>((1.5708 * 16384) / sl::k_roll_rate);
    run(world, roll_ticks, roll);
    run(world, 40, nothing());

    pse::Basis rolled;
    sl::player_basis(world, rolled);
    // Rolling right drops the right wing, so the canopy leans right and the
    // ship's own up (column 1 of the basis) swings onto the world's +x.
    const float up_x = rolled.m[1], up_y = rolled.m[4];
    CHECK(up_x > 0.75f);
    CHECK(up_y > -0.4f && up_y < 0.4f);

    // Now pull back. The nose has to climb along the ship's own up, which is
    // world +x while it is rolled, and must NOT climb along the world's y.
    sl::Input pitch = nothing();
    pitch.pitch = 1;
    const float before_x = rolled.m[2], before_y = rolled.m[5];
    run(world, 60, pitch);

    pse::Basis after;
    sl::player_basis(world, after);
    const float moved_x = after.m[2] - before_x;
    const float moved_y = after.m[5] - before_y;

    // An Euler rig would have put this movement on the world's y instead,
    // because its pitch angle turns about an axis the roll angle has already
    // moved. That is the whole reason this game carries a quaternion.
    CHECK(moved_x > 0.05f);
    CHECK(moved_y > -0.05f && moved_y < 0.05f);
}

void test_the_ship_flies_where_its_nose_points() {
    sl::World world;
    sl::world_init(world);
    run(world, 100, nothing());
    // Started at the origin looking down +z, so a hundred ticks of cruise is
    // a hundred ticks of +z and nothing else.
    CHECK(world.z > sl::k_player_speed * 90);
    CHECK(world.x == 0 && world.y == 0);

    sl::Input yaw = nothing();
    yaw.yaw = 1;
    run(world, 200, yaw);
    // Yawing right has to take the ship to the right, which is +x.
    CHECK(world.x > 0);
}

void test_a_flight_is_a_pure_function_of_its_inputs() {
    sl::World a, b;
    sl::world_init(a, 0xC0FFEE11u);
    sl::world_init(b, 0xC0FFEE11u);

    sl::Input in = nothing();
    in.fire = true;
    in.yaw = 1;
    for (int i = 0; i < 3000; i++) {
        in.pitch = static_cast<int8_t>((i / 120) % 3 - 1);
        sl::world_tick(a, in);
        sl::world_tick(b, in);
    }
    CHECK(a.tick == b.tick);
    CHECK(a.x == b.x && a.y == b.y && a.z == b.z);
    CHECK(a.score == b.score && a.kills == b.kills);
    CHECK(a.hull == b.hull && a.shield == b.shield);
}

void test_the_arena_holds_the_player() {
    sl::World world;
    sl::world_init(world);
    run(world, 40000, nothing());
    CHECK(world.x <= sl::k_arena_half && world.x >= -sl::k_arena_half);
    CHECK(world.y <= sl::k_arena_half && world.y >= -sl::k_arena_half);
    CHECK(world.z <= sl::k_arena_half && world.z >= -sl::k_arena_half);
    CHECK(world.out_of_bounds);
}

// ---- targeting ----

void test_the_cycle_takes_what_is_in_front_before_what_is_behind() {
    sl::World world;
    sl::world_init(world);
    jump_to_wave(world, 3);

    int8_t order[sl::k_max_ships];
    const uint8_t count = sl::target_order(world, order);
    CHECK(count == 4);

    // Everything inside the forward cone comes first, and within that group
    // the nearest leads. Outside the cone, the one needing the smallest turn
    // leads. Walking the order and checking it never goes backwards through
    // those two keys is the whole claim.
    bool left_the_cone = false;
    int32_t last_range = -1;
    int32_t last_align = 32767;
    for (uint8_t i = 0; i < count; i++) {
        const sl::Ship& ship = world.ships[order[i]];
        const int32_t align = sl::alignment(world, ship.x, ship.y, ship.z);
        const int32_t range = sl::range_to(world, ship);
        if (align >= sl::k_view_cos) {
            CHECK(!left_the_cone);
            CHECK(range >= last_range);
            last_range = range;
        } else {
            left_the_cone = true;
            CHECK(align <= last_align);
            last_align = align;
        }
    }
}

// Pressing Y on a ship you already have selected walks its hardpoints, and the
// press after the last one moves on to the next contact. That is the whole of
// the control the brief asked for, and it is one rule rather than two.
void test_pressing_target_again_walks_the_hardpoints() {
    sl::World world;
    sl::world_init(world);
    jump_to_wave(world, 5);

    sl::Input y = nothing();
    y.cycle_target = true;

    int8_t frigate_at = -1;
    for (int press = 0; press < 24 && frigate_at < 0; press++) {
        sl::world_tick(world, y);
        const sl::Ship* ship = sl::target_ship(world);
        if (ship != nullptr && ship->cls == sl::Hull::Frigate) {
            frigate_at = world.target;
        }
    }
    CHECK(frigate_at >= 0);
    if (frigate_at < 0) return;

    // Landing on a ship selects the ship, not one of its parts.
    CHECK(world.target_sub == -1);

    const uint8_t subs = world.ships[frigate_at].sub_count;
    CHECK(subs == 6);

    // Then one press per hardpoint, in order, all on the same ship.
    for (uint8_t s = 0; s < subs; s++) {
        sl::world_tick(world, y);
        CHECK(world.target == frigate_at);
        CHECK(world.target_sub == static_cast<int8_t>(s));
        CHECK(sl::target_subsystem(world) != nullptr);
    }

    // And the press after the last one leaves the ship.
    sl::world_tick(world, y);
    CHECK(world.target != frigate_at || world.target_sub == -1);
}

void test_a_dead_hardpoint_is_skipped_by_the_cycle() {
    sl::World world;
    sl::world_init(world);
    jump_to_wave(world, 5);

    const int8_t at = index_of(world, sl::Hull::Frigate);
    CHECK(at >= 0);
    if (at < 0) return;

    // Take the navigation array off it, then walk the hardpoints and check
    // the cycle never offers the one that is gone.
    const int8_t nav = find_sub(world.ships[at], sl::Sub::Navigation);
    CHECK(nav >= 0);
    world.ships[at].subs[nav].hull = 0;

    world.target = at;
    world.target_sub = -1;

    sl::Input y = nothing();
    y.cycle_target = true;
    for (int press = 0; press < 6; press++) {
        sl::world_tick(world, y);
        if (world.target != at) break;
        CHECK(world.target_sub != nav);
    }
}

// ---- hardpoints ----

// Aiming at a turret damages the turret, and the plating beside it stays
// where it was. Without this the whole subsystem idea is decoration.
void test_a_bolt_on_a_turret_hurts_the_turret_and_not_the_hull() {
    sl::World world;
    sl::world_init(world);
    jump_to_wave(world, 5);

    const int8_t at = index_of(world, sl::Hull::Frigate);
    CHECK(at >= 0);
    if (at < 0) return;

    sl::Ship& frigate = world.ships[at];
    const int8_t turret = find_sub(frigate, sl::Sub::Weapons);
    CHECK(turret >= 0);
    if (turret < 0) return;

    clear_except(world, at);

    const int16_t hull_before = frigate.hull;
    const int16_t shield_before = frigate.shield;
    const int16_t turret_before = frigate.subs[turret].hull;

    // Nose on the sponson, forty units out, and hold the trigger. The turret
    // sits outboard of the frigate's plating box, so a bolt on this line
    // reaches it without the hull swallowing the shot first, which is exactly
    // what the box is there for.
    hold_and_fire(world, facing_ahead(), sl::units(40), 300,
                  [&](int32_t& x, int32_t& y, int32_t& z) {
                      sl::sub_position(frigate, frigate.subs[turret], x, y, z);
                      return frigate.active;
                  },
                  [&] { return frigate.subs[turret].hull < turret_before; });

    CHECK(frigate.subs[turret].hull < turret_before);
    // The bubble is around the hull, not around the turret, so a hardpoint
    // hit must not have gone through the shield either.
    CHECK(frigate.shield == shield_before);
    CHECK(frigate.hull == hull_before);
}

// Selecting a hardpoint is the player saying which part of a ship they mean,
// and the guns honour it within a couple of hull widths. Without that aid a
// turret on a frigate at eighty units is two pixels and the mechanic the game
// is built around cannot be operated.
void test_selecting_a_hardpoint_makes_it_hittable() {
    sl::World world;
    sl::world_init(world);
    jump_to_wave(world, 5);

    const int8_t at = index_of(world, sl::Hull::Frigate);
    if (at < 0) { CHECK(false); return; }
    clear_except(world, at);

    sl::Ship& frigate = world.ships[at];
    const int8_t engines = find_sub(frigate, sl::Sub::Engines);
    if (engines < 0) { CHECK(false); return; }

    // Aim a whole hull width off the engine block, with nothing selected.
    const int16_t before = frigate.subs[engines].hull;
    world.target = -1;
    world.target_sub = -1;
    hold_and_fire(world, facing_ahead(), sl::units(30), 200,
                  [&](int32_t& x, int32_t& y, int32_t& z) {
                      sl::sub_position(frigate, frigate.subs[engines], x, y, z);
                      y += sl::units(3);
                      return frigate.active;
                  },
                  [&] { return frigate.subs[engines].hull < before; });
    CHECK(frigate.subs[engines].hull == before);

    // Same line of fire, engines now selected.
    world.target = at;
    world.target_sub = engines;
    hold_and_fire(world, facing_ahead(), sl::units(30), 200,
                  [&](int32_t& x, int32_t& y, int32_t& z) {
                      sl::sub_position(frigate, frigate.subs[engines], x, y, z);
                      y += sl::units(3);
                      return frigate.active;
                  },
                  [&] { return frigate.subs[engines].hull < before; });
    CHECK(frigate.subs[engines].hull < before);
}

void test_shooting_the_engines_stops_the_ship() {
    sl::World world;
    sl::world_init(world);
    jump_to_wave(world, 4);

    const int8_t at = index_of(world, sl::Hull::Gunship);
    CHECK(at >= 0);
    if (at < 0) return;

    sl::Ship& gunship = world.ships[at];
    const int8_t engines = find_sub(gunship, sl::Sub::Engines);
    CHECK(engines >= 0);
    if (engines < 0) return;

    CHECK(sl::has_capability(gunship, sl::Sub::Engines));
    const int32_t moved_before = gunship.z;
    run(world, 60, nothing());
    CHECK(gunship.x != 0 || gunship.y != 0 || gunship.z != moved_before);

    gunship.subs[engines].hull = 0;
    CHECK(!sl::has_capability(gunship, sl::Sub::Engines));

    const int32_t x = gunship.x, y = gunship.y, z = gunship.z;
    run(world, 120, nothing());
    CHECK(gunship.x == x && gunship.y == y && gunship.z == z);
}

void test_losing_life_support_leaves_a_derelict() {
    sl::World world;
    sl::world_init(world);
    jump_to_wave(world, 5);

    const int8_t at = index_of(world, sl::Hull::Frigate);
    if (at < 0) { CHECK(false); return; }

    sl::Ship& frigate = world.ships[at];
    const int8_t crew = find_sub(frigate, sl::Sub::LifeSupport);
    CHECK(crew >= 0);
    if (crew < 0) return;

    clear_except(world, at);

    // Damage it the way a bolt does, so the side effect being tested is the
    // one the game actually runs and not one this test performed itself.
    world.target = at;
    world.target_sub = crew;
    hold_and_fire(world, facing_up(), sl::units(34), 1200,
                  [&](int32_t& x, int32_t& y, int32_t& z) {
                      sl::sub_position(frigate, frigate.subs[crew], x, y, z);
                      return frigate.active;
                  },
                  [&] { return frigate.subs[crew].hull == 0; });
    CHECK(frigate.subs[crew].hull == 0);
    CHECK(frigate.task == sl::Task::Derelict);

    // Nobody is flying it, so it comes apart on its own.
    const int16_t hull = frigate.hull;
    run(world, sl::k_derelict_period * 4, nothing());
    CHECK(frigate.hull < hull);
}

// ---- the mission ----

void test_the_frigate_leaves_if_its_navigation_is_left_alone() {
    sl::World world;
    sl::world_init(world);
    jump_to_wave(world, 5);
    CHECK(sl::jump_ticks_left(world) > 0);

    // Alone with the frigate, and with its guns already off it, so the only
    // thing that can end this sortie is the clock. Otherwise the test passes
    // on the escort killing the player, which is a different loss entirely
    // and would keep passing after the jump stopped working.
    const int8_t at = index_of(world, sl::Hull::Frigate);
    if (at < 0) { CHECK(false); return; }
    clear_except(world, at);
    for (uint8_t s = 0; s < world.ships[at].sub_count; s++) {
        if (world.ships[at].subs[s].kind == sl::Sub::Weapons) {
            world.ships[at].subs[s].hull = 0;
        }
    }

    for (uint32_t i = 0; i < sl::k_jump_charge + 400 && sl::in_flight(world);
         i++) {
        sl::world_tick(world, nothing());
    }
    CHECK(world.phase == sl::Phase::Lost);
    CHECK(world.loss == sl::Loss::Jumped);
}

void test_killing_the_navigation_array_stops_the_jump_for_good() {
    sl::World world;
    sl::world_init(world);
    jump_to_wave(world, 5);

    const int8_t at = index_of(world, sl::Hull::Frigate);
    if (at < 0) { CHECK(false); return; }
    const int8_t nav = find_sub(world.ships[at], sl::Sub::Navigation);
    if (nav < 0) { CHECK(false); return; }
    clear_except(world, at);
    sl::Ship& frigate = world.ships[at];

    run(world, 400, nothing());
    const uint32_t charged = world.jump_charge;
    CHECK(charged > 0);
    CHECK(!world.jump_stopped);

    world.target = at;
    world.target_sub = nav;
    hold_and_fire(world, facing_ahead(), sl::units(40), 1500,
                  [&](int32_t& x, int32_t& y, int32_t& z) {
                      sl::sub_position(frigate, frigate.subs[nav], x, y, z);
                      return frigate.active;
                  },
                  [&] { return world.jump_stopped; });
    CHECK(world.jump_stopped);
    CHECK(sl::jump_ticks_left(world) == 0);

    // And it stays stopped. The clock does not restart, and the sortie cannot
    // be lost to a jump any more however long it runs.
    run(world, 3000, nothing());
    CHECK(world.loss != sl::Loss::Jumped);
    CHECK(world.jump_stopped);
}

void test_clearing_a_wave_calls_in_the_next_one() {
    sl::World world;
    sl::world_init(world);
    jump_to_wave(world, 1);
    CHECK(world.wave == 1);

    for (uint8_t i = 0; i < sl::k_max_ships; i++) {
        world.ships[i].active = false;
    }
    sl::world_tick(world, nothing());
    CHECK(world.wave == 2);
    CHECK(world.phase == sl::Phase::Briefing);
    CHECK(world.wave_timer > 0);

    run(world, world.wave_timer + 2, nothing());
    CHECK(world.phase == sl::Phase::Fighting);
    uint8_t live = 0;
    for (uint8_t i = 0; i < sl::k_max_ships; i++) {
        if (world.ships[i].active) live++;
    }
    CHECK(live == 3);
}

void test_clearing_the_last_wave_wins_the_sortie() {
    sl::World world;
    sl::world_init(world);
    jump_to_wave(world, sl::k_wave_count);
    for (uint8_t i = 0; i < sl::k_max_ships; i++) {
        world.ships[i].active = false;
    }
    sl::world_tick(world, nothing());
    CHECK(world.phase == sl::Phase::Won);
    CHECK(!sl::in_flight(world));

    // A decided sortie stops taking input, so a held trigger cannot run the
    // score up after the fact.
    const uint32_t score = world.score;
    const uint32_t tick = world.tick;
    sl::Input fire = nothing();
    fire.fire = true;
    run(world, 200, fire);
    CHECK(world.score == score);
    CHECK(world.tick == tick);
}

// ---- weapons ----

void test_the_guns_converge_rather_than_running_parallel() {
    sl::World world;
    sl::world_init(world);
    sl::Input fire = nothing();
    fire.fire = true;
    sl::world_tick(world, fire);

    int found = 0;
    int32_t vx[2] = {0, 0};
    for (uint8_t i = 0; i < sl::k_max_bolts; i++) {
        if (!world.shots[i].active) continue;
        if (world.shots[i].kind != sl::Bolt::PlayerGun) continue;
        if (found < 2) vx[found] = world.shots[i].vx;
        found++;
    }
    CHECK(found == 2);
    // Fired from either side of the nose and aimed at one point ahead, so the
    // two have equal and opposite sideways velocity. Parallel barrels would
    // make both of these zero, and a player putting the crosshair on a fighter
    // would watch both bolts go by either side of it.
    CHECK(vx[0] == -vx[1]);
    CHECK(vx[0] != 0);
}

void test_a_missile_needs_a_target_and_spends_a_round() {
    sl::World world;
    sl::world_init(world);
    jump_to_wave(world, 1);

    sl::Input launch = nothing();
    launch.launch = true;

    const uint8_t rack = world.missiles;
    CHECK(rack == sl::k_missiles_max);

    // Nothing targeted: the launcher does nothing at all, and above all does
    // not quietly eat a missile.
    world.target = -1;
    run(world, 30, launch);
    CHECK(world.missiles == rack);

    sl::Input y = nothing();
    y.cycle_target = true;
    sl::world_tick(world, y);
    CHECK(world.target >= 0);

    sl::world_tick(world, launch);
    CHECK(world.missiles == rack - 1);
    uint8_t flying = 0;
    for (uint8_t i = 0; i < sl::k_max_missiles; i++) {
        if (world.missiles_live[i].active) flying++;
    }
    CHECK(flying == 1);

    // And it will not fire again until the launcher has cycled.
    run(world, 10, launch);
    CHECK(world.missiles == rack - 1);
}

void test_shields_come_back_and_hull_does_not() {
    sl::World world;
    sl::world_init(world);
    // An empty sky: the point is what the ship does on its own, and a wave
    // arriving partway through would be measuring the enemy's aim instead.
    world.wave_timer = 60000;
    world.shield = 20;
    world.hull = 50;
    world.shield_idle = 0;

    // Nothing recharges while the shield is still settling.
    run(world, sl::k_shield_regen_delay / 2, nothing());
    CHECK(world.shield == 20);

    run(world, sl::k_shield_regen_delay + 400, nothing());
    CHECK(world.shield > 20);
    CHECK(world.hull == 50);
}

void test_a_kill_leaves_something_to_look_at() {
    sl::World world;
    sl::world_init(world);
    jump_to_wave(world, 1);

    for (uint8_t i = 0; i < sl::k_max_ships; i++) {
        if (!world.ships[i].active) continue;
        world.ships[i].hull = 1;
        world.ships[i].shield = 0;
        break;
    }

    sl::Input fire = nothing();
    fire.fire = true;
    // Sit the player right on top of the first contact so the shot cannot
    // miss, then hold the trigger.
    const sl::Ship* victim = first_of(world, sl::Hull::Fighter);
    CHECK(victim != nullptr);
    if (victim == nullptr) return;

    for (int i = 0; i < 600 && world.kills == 0 && sl::in_flight(world); i++) {
        sl::world_tick(world, fire);
    }

    if (world.kills > 0) {
        uint8_t blasts = 0;
        for (uint8_t i = 0; i < sl::k_max_blasts; i++) {
            if (world.blasts[i].active) blasts++;
        }
        CHECK(blasts > 0);
        CHECK(world.score > 0);
    }
}

// ---- budget ----

void test_the_world_fits_its_ram_budget() {
    std::printf("sizeof(World) = %u bytes\n",
                static_cast<unsigned>(sizeof(sl::World)));
    // The framebuffer, the depth buffer and the triangle queue have already
    // spent most of a PicoSystem's 264 KB before this file allocates a byte.
    // Eight is generous for a battle and small enough to be sure.
    CHECK(sizeof(sl::World) <= 8192);
}

}  // namespace

int main() {
    test_a_rolled_ship_still_pitches_about_its_own_nose();
    test_the_ship_flies_where_its_nose_points();
    test_a_flight_is_a_pure_function_of_its_inputs();
    test_the_arena_holds_the_player();

    test_the_cycle_takes_what_is_in_front_before_what_is_behind();
    test_pressing_target_again_walks_the_hardpoints();
    test_a_dead_hardpoint_is_skipped_by_the_cycle();

    test_a_bolt_on_a_turret_hurts_the_turret_and_not_the_hull();
    test_selecting_a_hardpoint_makes_it_hittable();
    test_shooting_the_engines_stops_the_ship();
    test_losing_life_support_leaves_a_derelict();

    test_the_frigate_leaves_if_its_navigation_is_left_alone();
    test_killing_the_navigation_array_stops_the_jump_for_good();
    test_clearing_a_wave_calls_in_the_next_one();
    test_clearing_the_last_wave_wins_the_sortie();

    test_the_guns_converge_rather_than_running_parallel();
    test_a_missile_needs_a_target_and_spends_a_round();
    test_shields_come_back_and_hull_does_not();
    test_a_kill_leaves_something_to_look_at();

    test_the_world_fits_its_ram_budget();

    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
