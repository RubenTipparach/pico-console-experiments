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
        // The species of tree a zone grows has to be one the art has frames
        // for. The renderer indexes a table with it and a stray value would
        // read past the end of that table.
        check(zone.trees < uint8_t(pm::TreeKind::Count),
              "zone names a tree species that exists");
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
    data.zone = w.zone;
    data.home_zone = 200;
    check(!pm::world_load(bad, data),
          "a save whose home is nowhere is refused, because a whiteout would "
          "read it and put the player there");
}

// ---- fainting, and where you wake up ------------------------------------

// Walk up to an NPC and press A until whatever it does has happened.
pm::World talk_to(uint8_t zone, uint8_t x, uint8_t y, uint8_t facing) {
    pm::World w = start_world(2468);
    w.zone = zone;
    w.tx = x;
    w.ty = y;
    w.facing = facing;
    w.mode = pm::Mode::Overworld;
    w.fade = 0;
    pm::world_tick(w, press_a());
    return w;
}

// Knock the whole party out mid battle and press through to the other side.
void faint_the_party(pm::World& w) {
    w.mode = pm::Mode::Battle;
    w.battle = pm::Battle{};
    w.battle.foe = pm::make_mon(2, 30);
    w.battle.wild = true;
    w.battle.trainer_npc = 0xFF;
    w.battle.active = 0;
    w.battle.state = pm::BattleState::Menu;
    for (int i = 0; i < w.party_count; i++) w.party[i].hp = 0;
    // Fight anyway: the sim notices the active creature is down when the
    // turn's messages have finished, which is the path a real loss takes.
    w.battle.state = pm::BattleState::Message;
    for (int i = 0; i < 200 && w.mode == pm::Mode::Battle; i++) {
        pm::world_tick(w, press_a());
    }
}

void test_a_whiteout_goes_home_and_says_so() {
    pm::World w = start_world(1357);
    // Somewhere far from anywhere, so "went home" cannot be confused with
    // "stayed put".
    w.zone = pm::zone_route1;
    w.tx = 11;
    w.ty = 20;
    w.mode = pm::Mode::Overworld;
    w.fade = 0;
    // Spend the PP first. A party that loses with moves still charged proves
    // nothing about whether the whiteout gives them back.
    for (int i = 0; i < w.party_count; i++) {
        for (int m = 0; m < 4; m++) w.party[i].pp[m] = 0;
    }

    faint_the_party(w);
    check(w.mode != pm::Mode::Battle, "a lost battle ends");
    check(w.mode == pm::Mode::Dialogue,
          "and says something rather than cutting to another town in silence");
    check(w.zone == pm::k_start.zone && w.tx == pm::k_start.x &&
          w.ty == pm::k_start.y,
          "with no CENTRE rested at, home is where the game started");
    for (int i = 0; i < w.party_count; i++) {
        check(w.party[i].hp == w.party[i].max_hp, "everyone is patched up");
        for (int m = 0; m < 4; m++) {
            if (w.party[i].moves[m] == 0xFF) continue;
            check(w.party[i].pp[m] == pm::k_moves[w.party[i].moves[m]].pp,
                  "and has its moves back, not just its health");
        }
    }
    check(w.save_pending, "and it is worth writing down");

    // Dismissing the line leaves the player standing in the world, not stuck
    // in a menu and not back in the battle.
    for (int i = 0; i < 10 && w.mode == pm::Mode::Dialogue; i++) {
        pm::world_tick(w, press_a());
    }
    check(w.mode == pm::Mode::Overworld, "and then hands control back");
    check(pm::tile_walkable(pm::zone_of(w), w.tx, w.ty),
          "on a tile that can be stood on");
}

void test_resting_at_a_centre_moves_home() {
    // The nurse is at 6,2 in the CENTRE; stand below her and face north.
    pm::World w = talk_to(pm::zone_healcentre, 6, 3, 0);
    check(w.mode == pm::Mode::Dialogue, "the nurse says something");
    check(w.home_zone == pm::zone_healcentre && w.home_x == 6 && w.home_y == 3,
          "resting at a CENTRE makes it home");
    check(w.home_x != 6 || w.home_y != 2,
          "and home is the player's tile, not the nurse's, which she blocks");

    // Now lose, and wake up there rather than at the start.
    w.mode = pm::Mode::Overworld;
    w.zone = pm::zone_route1;
    w.tx = 11;
    w.ty = 20;
    w.fade = 0;
    faint_the_party(w);
    check(w.zone == pm::zone_healcentre && w.tx == 6 && w.ty == 3,
          "a whiteout goes to the last CENTRE rested at");
    check(w.zone != pm::k_start.zone || w.ty != pm::k_start.y,
          "which is not where the game started");
    check(pm::tile_walkable(pm::zone_of(w), w.tx, w.ty),
          "and is somewhere the player can stand");
}

void test_the_nurse_actually_heals() {
    pm::World w = start_world(8642);
    w.party[0].hp = 1;
    for (int m = 0; m < 4; m++) if (w.party[0].moves[m] != 0xFF) w.party[0].pp[m] = 0;
    w.zone = pm::zone_healcentre;
    w.tx = 6;
    w.ty = 3;
    w.facing = 0;
    w.mode = pm::Mode::Overworld;
    w.fade = 0;
    pm::world_tick(w, press_a());
    check(w.party[0].hp == w.party[0].max_hp, "the nurse restores health");
    for (int m = 0; m < 4; m++) {
        if (w.party[0].moves[m] == 0xFF) continue;
        check(w.party[0].pp[m] == pm::k_moves[w.party[0].moves[m]].pp,
              "and PP, which is the other half of being able to carry on");
    }
}

// trainer_npc indexes the zone the challenge was issued in. Carrying it
// through a warp read past the end of the destination zone's NPC array, which
// ASan caught in the preview harness and an ordinary build got away with.
void test_a_challenge_does_not_follow_you_between_zones() {
    pm::World w = start_world(1122);
    // Take a challenge from the gym leader, who is the last NPC in the
    // largest cast in the game.
    w.zone = pm::zone_stonegym;
    w.tx = 6;
    w.ty = 3;
    w.facing = 0;
    w.mode = pm::Mode::Overworld;
    w.fade = 0;
    pm::world_tick(w, press_a());
    check(w.mode == pm::Mode::Dialogue, "the leader issues a challenge");
    check(w.battle.trainer_npc != 0xFF, "and the battle is armed");
    const uint8_t armed = w.battle.trainer_npc;

    // Now be somewhere else, in a zone with fewer NPCs than that index.
    const pm::Zone& small = pm::k_zones[pm::zone_healcentre];
    check(armed >= small.npc_count,
          "the armed index is out of range for the destination, which is the "
          "whole point of this test");
    w.zone = pm::zone_healcentre;
    w.tx = 6;
    w.ty = 3;
    w.mode = pm::Mode::Overworld;
    w.fade = 0;

    // Talk to the nurse and dismiss her, which is the path that read out of
    // bounds. Any answer other than a crash is fine; what must not happen is
    // a battle against whatever was in that memory.
    pm::world_tick(w, press_a());
    for (int i = 0; i < 10 && w.mode == pm::Mode::Dialogue; i++) {
        pm::world_tick(w, press_a());
    }
    check(w.mode != pm::Mode::Battle,
          "and no battle starts against an NPC from another zone");

    // And through the front door, the index is cleared on the way.
    pm::World v = start_world(1122);
    v.zone = pm::zone_stonegym;
    v.tx = 6;
    v.ty = 3;
    v.facing = 0;
    v.mode = pm::Mode::Overworld;
    v.fade = 0;
    pm::world_tick(v, press_a());
    v.mode = pm::Mode::Overworld;
    v.tx = 6;
    v.ty = 14;
    v.fade = 0;
    for (int i = 0; i < 40 && v.zone == pm::zone_stonegym; i++) {
        pm::world_tick(v, hold(2));       // south, out of the door
    }
    check(v.zone != pm::zone_stonegym, "the player leaves through the door");
    check(v.battle.trainer_npc == 0xFF,
          "and the armed challenge does not come with them");
}

void test_home_survives_a_save() {
    pm::World w = talk_to(pm::zone_healcentre, 6, 3, 0);
    pm::SaveData data;
    pm::world_make_save(w, data);
    pm::World loaded;
    pm::world_init(loaded, 1);
    check(pm::world_load(loaded, data), "the save loads");
    check(loaded.home_zone == w.home_zone && loaded.home_x == w.home_x &&
          loaded.home_y == w.home_y,
          "where the player wakes up survives being turned off");
}

// ---- the mart ------------------------------------------------------------

// Face an NPC and press A until whatever it does has happened.
const pm::NpcDef* npc_named(const pm::Zone& z, uint8_t x, uint8_t y) {
    for (int i = 0; i < z.npc_count; i++) {
        if (z.npcs[i].x == x && z.npcs[i].y == y) return &z.npcs[i];
    }
    return nullptr;
}

pm::World at_the_counter() {
    pm::World w = start_world(31337);
    w.zone = pm::zone_picomart;
    w.tx = 5;
    w.ty = 4;
    w.facing = 0;                       // north, at the clerk on (5,3)
    w.mode = pm::Mode::Overworld;
    for (int i = 0; i < 20 && w.mode != pm::Mode::Shop; i++) {
        pm::world_tick(w, press_a());
    }
    return w;
}

void test_the_shop_takes_money_and_gives_goods() {
    const pm::Zone& mart = pm::k_zones[pm::zone_picomart];
    const pm::NpcDef* clerk = npc_named(mart, 5, 3);
    check(clerk != nullptr, "the mart has a clerk");
    check(clerk && pm::NpcKind(clerk->kind) == pm::NpcKind::Shop,
          "the clerk keeps a shop");
    check(clerk && clerk->stock_count > 0, "the shop stocks something");

    pm::World w = at_the_counter();
    check(w.mode == pm::Mode::Shop, "talking to the clerk opens the counter");
    check(pm::shop_of(w) == clerk, "the open counter is the clerk's");

    const uint8_t item = pm::k_stock[clerk->stock_first];
    const uint16_t price = pm::k_items[item].price;
    check(price > 0, "everything on the shelf has a price");

    const uint16_t money = w.money;
    const int held = pm::bag_count_of(w, item);
    w.menu_cursor = 0;
    pm::world_tick(w, press_a());
    check(w.money == money - price, "buying takes the price off the money");
    check(pm::bag_count_of(w, item) == held + 1, "and puts the item in the bag");
    check(w.save_pending, "a purchase is worth writing down");

    // Broke: the refusal has to be total. Money unchanged, bag unchanged.
    w.money = uint16_t(price - 1);
    const int held2 = pm::bag_count_of(w, item);
    pm::world_tick(w, press_a());
    check(w.money == price - 1, "a purchase that cannot be afforded costs nothing");
    check(pm::bag_count_of(w, item) == held2, "and hands over nothing");

    // And it closes.
    pm::Input b{};
    b.b_pressed = true;
    pm::world_tick(w, b);
    check(w.mode == pm::Mode::Overworld, "B leaves the counter");
}

// ---- the gym -------------------------------------------------------------

void test_the_gym_is_three_minions_and_a_leader() {
    const pm::Zone& gym = pm::k_zones[pm::zone_stonegym];
    int trainers = 0, leaders = 0, biggest = 0;
    for (int i = 0; i < gym.npc_count; i++) {
        const pm::NpcDef& n = gym.npcs[i];
        if (pm::NpcKind(n.kind) != pm::NpcKind::Trainer) continue;
        trainers++;
        if (n.sight == 0) leaders++;
        if (n.party_count > biggest) biggest = n.party_count;
        check(n.party_count > 0, "every gym trainer has something to send out");
        check(n.flag != pm::k_no_flag, "and stops challenging once beaten");
    }
    check(trainers == 4, "the gym is three minions and a leader");
    check(leaders == 1, "exactly one of them is walked up to rather than tripped");
    check(biggest >= 3, "the leader brings more than a minion does");

    // The minions are not optional: each one's sight line covers the only
    // tile its chamber can be entered through, so the route past them is
    // visible but not avoidable.
    int watched = 0;
    for (int i = 0; i < gym.npc_count; i++) {
        const pm::NpcDef& n = gym.npcs[i];
        if (n.sight == 0) continue;
        static const int dx[4] = {0, 1, 0, -1};
        static const int dy[4] = {-1, 0, 1, 0};
        for (int s = 1; s <= n.sight; s++) {
            const int x = n.x + dx[n.facing] * s;
            const int y = n.y + dy[n.facing] * s;
            if (!pm::tile_walkable(gym, x, y)) break;
            if (pm::tile_at(gym, x, y) == pm::tile_floor) watched++;
        }
    }
    check(watched >= 6, "the minions' sight lines cover real ground");
}

void test_the_badge_opens_the_cave() {
    const pm::Zone& gym = pm::k_zones[pm::zone_stonegym];
    uint8_t badge = pm::k_no_flag;
    for (int i = 0; i < gym.npc_count; i++) {
        if (gym.npcs[i].sight == 0 &&
            pm::NpcKind(gym.npcs[i].kind) == pm::NpcKind::Trainer) {
            badge = gym.npcs[i].flag;
        }
    }
    check(badge != pm::k_no_flag, "the leader hands out a flag");
    check(badge == pm::flag_badge_stone, "and it is the badge");

    // The guard stands on the one tile the cave warp can be reached from, so
    // the gate is a gate and not a suggestion.
    const pm::Zone& route = pm::k_zones[pm::zone_route1];
    const pm::NpcDef* guard = nullptr;
    for (int i = 0; i < route.npc_count; i++) {
        if (route.npcs[i].cond & pm::k_cond_hide) guard = &route.npcs[i];
    }
    check(guard != nullptr, "route 1 has a gate that disappears");
    if (!guard) return;
    check((guard->cond & ~pm::k_cond_hide) == pm::flag_badge_stone,
          "and the badge is what disappears it");

    pm::World w = start_world(4242);
    w.zone = pm::zone_route1;
    check(pm::npc_present(w, *guard), "the gate is up before the badge");
    pm::flag_set(w, pm::flag_badge_stone);
    check(!pm::npc_present(w, *guard), "and gone after it");

    // Walk into it from below with no badge: the player must not get through.
    pm::World blocked = start_world(4242);
    blocked.zone = pm::zone_route1;
    blocked.tx = guard->x;
    blocked.ty = uint8_t(guard->y + 1);
    blocked.mode = pm::Mode::Overworld;
    blocked.fade = 0;
    for (int i = 0; i < 200; i++) pm::world_tick(blocked, hold(0));
    check(blocked.zone == pm::zone_route1,
          "no badge, no cave, however long the player leans on the d pad");

    // And the other half, which matters more: a gate that never opens is a
    // worse bug than one that never closes, and closing the top of Route 1
    // down to a single tile is exactly how that would happen.
    pm::World through = start_world(4242);
    through.zone = pm::zone_route1;
    through.tx = guard->x;
    through.ty = uint8_t(guard->y + 1);
    through.mode = pm::Mode::Overworld;
    through.fade = 0;
    pm::flag_set(through, pm::flag_badge_stone);
    for (int i = 0; i < 200 && through.zone == pm::zone_route1; i++) {
        pm::world_tick(through, hold(0));
    }
    check(through.zone == pm::zone_hollowcave, "with the badge, the cave opens");
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
    test_a_whiteout_goes_home_and_says_so();
    test_resting_at_a_centre_moves_home();
    test_the_nurse_actually_heals();
    test_a_challenge_does_not_follow_you_between_zones();
    test_home_survives_a_save();
    test_the_shop_takes_money_and_gives_goods();
    test_the_gym_is_three_minions_and_a_leader();
    test_the_badge_opens_the_cave();
    test_state_fits_its_budget();

    if (g_failures) {
        std::printf("%d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("picomon sim tests passed\n");
    return 0;
}
