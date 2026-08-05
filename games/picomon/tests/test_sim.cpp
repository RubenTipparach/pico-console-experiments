// Host side tests for the Picomon sim. No SDK, no renderer, no device.
//
// The sim is pure integer C++ driven by a seeded generator, so its promises can
// be proven by playing it rather than asserted in a comment: that the map is
// walkable, that a warp lands somewhere legal, that a battle ends, that a
// catch obeys its own formula, and that a save round trips.

#include <cstdio>
#include <cstring>

#include "sim.hpp"

namespace {

int g_failures = 0;

void check(bool ok, const char* what) {
    if (ok) return;
    std::printf("FAIL: %s\n", what);
    g_failures++;
}

pm::Input none() { return pm::Input{}; }

pm::Input press_a() {
    pm::Input in{};
    in.a_pressed = true;
    return in;
}

pm::Input hold(int dir) {
    pm::Input in{};
    if (dir == 0) { in.up = true; in.up_pressed = true; }
    if (dir == 1) { in.right = true; in.right_pressed = true; }
    if (dir == 2) { in.down = true; in.down_pressed = true; }
    if (dir == 3) { in.left = true; in.left_pressed = true; }
    return in;
}

pm::World start_world(uint32_t seed) {
    pm::World w;
    pm::world_init(w, seed);
    pm::world_tick(w, press_a());          // leave the title
    return w;
}

// ---- the data itself -----------------------------------------------------

void test_data_is_sane() {
    check(pm::k_zone_count > 0, "there is at least one zone");
    check(pm::k_species_count > 0, "there is at least one species");
    for (int z = 0; z < pm::k_zone_count; z++) {
        const pm::Zone& zone = pm::k_zones[z];
        check(zone.w > 0 && zone.h > 0, "zone has a size");
        // Every warp has to land on a tile the player can occupy, or the
        // player is stranded inside scenery with no way out.
        for (int i = 0; i < zone.warp_count; i++) {
            const pm::WarpDef& wp = zone.warps[i];
            check(wp.dest < pm::k_zone_count, "warp goes to a real zone");
            const pm::Zone& dest = pm::k_zones[wp.dest];
            check(pm::tile_walkable(dest, wp.dx, wp.dy),
                  "warp lands on a walkable tile");
            check(pm::tile_walkable(zone, wp.x, wp.y),
                  "the warp tile itself is walkable");
        }
        // An NPC standing in a wall can never be talked to.
        for (int i = 0; i < zone.npc_count; i++) {
            check(pm::tile_walkable(zone, zone.npcs[i].x, zone.npcs[i].y),
                  "npc stands somewhere reachable");
        }
    }
}

void test_type_ring() {
    const int ember = int(pm::Type::Ember), leaf = int(pm::Type::Leaf);
    const int stone = int(pm::Type::Stone), spark = int(pm::Type::Spark);
    const int tide = int(pm::Type::Tide), mind = int(pm::Type::Mind);
    check(pm::type_multiplier(ember, leaf) == 8, "ember burns leaf");
    check(pm::type_multiplier(leaf, stone) == 8, "leaf breaks stone");
    check(pm::type_multiplier(stone, spark) == 8, "stone grounds spark");
    check(pm::type_multiplier(spark, tide) == 8, "spark charges tide");
    check(pm::type_multiplier(tide, ember) == 8, "tide douses ember");
    check(pm::type_multiplier(leaf, ember) == 2, "the ring runs one way");
    check(pm::type_multiplier(ember, ember) == 4, "same type is even");
    check(pm::type_multiplier(mind, ember) == 4, "mind is even against all");
    check(pm::type_multiplier(ember, mind) == 4, "and takes even from all");
    check(pm::type_multiplier(mind, mind) == 8, "except from itself");
    // The ring has to be a ring: every type beats exactly one and loses to
    // exactly one, or a player who learns half of it learns a lie.
    for (int a = 0; a < 5; a++) {
        int beats = 0, loses = 0;
        for (int d = 0; d < 5; d++) {
            if (pm::type_multiplier(a, d) == 8) beats++;
            if (pm::type_multiplier(d, a) == 8) loses++;
        }
        check(beats == 1 && loses == 1, "each type beats one and loses to one");
    }
}

// ---- the overworld -------------------------------------------------------

void test_walking_stays_legal() {
    pm::World w = start_world(1234);
    // Walk a long pseudo random path. Whatever it does, the player must never
    // end a tick standing somewhere they could not have walked to.
    uint32_t r = 99;
    for (int i = 0; i < 40000; i++) {
        r = r * 1103515245u + 12345u;
        pm::World before = w;
        pm::world_tick(w, w.mode == pm::Mode::Overworld ? hold(int((r >> 16) & 3))
                                                        : press_a());
        if (w.mode == pm::Mode::Overworld && w.step == 0) {
            if (!pm::tile_walkable(pm::zone_of(w), w.tx, w.ty)) {
                std::printf("  stood on a blocked tile in zone %d at %d,%d\n",
                            w.zone, w.tx, w.ty);
                check(false, "the player never stands on a blocked tile");
                return;
            }
        }
        check(w.zone < pm::k_zone_count, "zone stays in range");
        (void)before;
    }
    check(true, "40000 ticks of walking stayed legal");
}

void test_encounters_happen_and_end() {
    // Stand in the tall grass on Route 1 and walk until something appears.
    pm::World w = start_world(777);
    w.zone = pm::zone_route1;
    w.tx = 4;
    w.ty = 6;
    w.mode = pm::Mode::Overworld;
    int battles = 0;
    for (int i = 0; i < 60000 && battles < 3; i++) {
        if (w.mode == pm::Mode::Battle) {
            battles++;
            // Fight it out with the first move until the battle ends.
            for (int t = 0; t < 4000 && w.mode == pm::Mode::Battle; t++) {
                pm::Input in = press_a();
                pm::world_tick(w, in);
            }
            check(w.mode != pm::Mode::Battle, "a battle always ends");
            continue;
        }
        pm::world_tick(w, hold((i / 9) % 2 == 0 ? 1 : 3));
    }
    check(battles >= 3, "tall grass produces encounters");
}

// ---- the two formulas ----------------------------------------------------

void test_catch_formula() {
    pm::Mon m = pm::make_mon(0, 10);
    const uint8_t full = pm::catch_value(m, 4);
    m.hp = 1;
    const uint8_t hurt = pm::catch_value(m, 4);
    check(hurt > full, "a worn down creature is easier to catch");
    const uint8_t great = pm::catch_value(m, 6);
    check(great >= hurt, "a better ball is never worse");
    check(pm::catch_value(m, 4) >= 1, "the odds never reach zero");
    // The shake table has to rise with a, or a better ball would make a catch
    // less likely, which is the exact opposite of what it says on the tin.
    for (int a = 8; a < 256; a += 8) {
        check(pm::shake_threshold(uint8_t(a)) >=
              pm::shake_threshold(uint8_t(a - 8)),
              "the shake threshold rises with the catch value");
    }
}

void test_damage_is_bounded() {
    for (int lv = 2; lv <= 50; lv += 6) {
        pm::Mon a = pm::make_mon(0, uint8_t(lv));
        pm::Mon d = pm::make_mon(2, uint8_t(lv));
        for (int m = 0; m < 4; m++) {
            if (a.moves[m] == 0xFF) continue;
            // A status move deals nothing by design, which is the one case
            // where zero is the right answer.
            const bool status = pm::k_moves[a.moves[m]].power == 0;
            for (int roll = 0; roll < 256; roll += 17) {
                const int dmg = pm::damage_of(a, d, a.moves[m], 0, 0,
                                              uint8_t(roll));
                if (status) {
                    check(dmg == 0, "a status move deals no damage");
                    continue;
                }
                check(dmg >= 1, "a hit always does something");
                check(dmg < 1000, "a hit never runs away with itself");
            }
        }
    }
}

void test_levelling_never_overflows() {
    // xp is sixteen bits on purpose. The curve has to stay inside it for every
    // level the game can reach, or a save silently corrupts at high level.
    for (int lv = 1; lv < 50; lv++) {
        check(pm::xp_for_level(uint8_t(lv)) < 60000,
              "the experience curve fits sixteen bits");
    }
}

// ---- persistence ---------------------------------------------------------

void test_save_round_trips() {
    pm::World w = start_world(4242);
    w.money = 1234;
    w.party[0].hp = 3;
    pm::flag_set(w, pm::k_flag_count > 0 ? 0 : 0);
    pm::SaveData data;
    pm::world_make_save(w, data);

    pm::World loaded;
    pm::world_init(loaded, 1);
    check(pm::world_load(loaded, data), "a save loads");
    check(loaded.money == 1234, "money survives");
    check(loaded.party[0].hp == 3, "damage survives");
    check(loaded.zone == w.zone && loaded.tx == w.tx && loaded.ty == w.ty,
          "position survives");
    if (pm::k_flag_count > 0) check(pm::flag_get(loaded, 0), "flags survive");

    // A save from another version, or one pointing at a zone that no longer
    // exists, has to be refused rather than loaded into a broken game.
    data.version = 99;
    pm::World bad;
    pm::world_init(bad, 1);
    check(!pm::world_load(bad, data), "a save from another version is refused");
    data.version = pm::k_save_version;
    data.zone = 200;
    check(!pm::world_load(bad, data), "a save naming no zone is refused");
}

void test_state_fits_its_budget() {
    // The device numbers, checked here so they cannot drift unnoticed.
    std::printf("  sizeof(World)    = %zu bytes\n", sizeof(pm::World));
    std::printf("  sizeof(SaveData) = %zu bytes\n", sizeof(pm::SaveData));
    check(sizeof(pm::World) <= 1024, "the live world fits its RAM budget");
    check(sizeof(pm::SaveData) <= 256, "the save block fits one flash block");
}

}  // namespace

int main() {
    test_data_is_sane();
    test_type_ring();
    test_walking_stays_legal();
    test_encounters_happen_and_end();
    test_catch_formula();
    test_damage_is_bounded();
    test_levelling_never_overflows();
    test_save_round_trips();
    test_state_fits_its_budget();

    if (g_failures) {
        std::printf("%d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("picomon sim tests passed\n");
    return 0;
}
