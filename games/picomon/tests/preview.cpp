// Renders real Picomon frames on the host, through the real engine, the real
// models and the real level data, and writes them as PPM files. This is how the
// game gets looked at and tuned without a device in hand: everything here is
// the exact code the PicoSystem runs.
//
// Usage: picomon_preview [out_dir]

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "pse/pixel.hpp"

#include "render.hpp"
#include "sim.hpp"

namespace {

constexpr int k_w = 120;
constexpr int k_h = 120;

void write_ppm(const std::string& path, const uint8_t* rgb) {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return;
    std::fprintf(f, "P6\n%d %d\n255\n", k_w, k_h);
    std::fwrite(rgb, 1, size_t(k_w) * k_h * 3, f);
    std::fclose(f);
    std::printf("wrote %s\n", path.c_str());
}

void capture(const pm::World& w, uint32_t time_ms, const std::string& path) {
    static std::vector<uint8_t> buffer(size_t(k_w) * k_h * 3);
    pse::RenderTarget target{buffer.data(), k_w, k_h, k_w * 3,
                             pse::PixelFormat::rgb888};
    pmr::render_scene(w, target, time_ms);
    write_ppm(path, buffer.data());
}

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

void run(pm::World& w, const pm::Input& in, int ticks) {
    for (int i = 0; i < ticks; i++) pm::world_tick(w, in);
}

// The battle from the inside: every state it can be in, each shot from a copy
// of the same encounter so the sequence reads as one fight rather than five
// unrelated ones. Copies, because a battle only goes one way: the capture line
// and the attack line cannot both be walked from the same world.
void battle_shots(const pm::World& at_menu, const std::string& out) {
    // The menu itself, with the foe's plate and the four commands.
    {
        pm::World w = at_menu;
        w.battle.state = pm::BattleState::Menu;
        capture(w, 5000, out + "/battle_1_menu.ppm");
    }

    // Fight, then the lunge. Attack holds for a handful of ticks, so take it
    // partway through rather than on its first frame.
    {
        pm::World w = at_menu;
        w.battle.state = pm::BattleState::Moves;
        w.battle.move_cursor = 0;
        pm::world_tick(w, press_a());
        for (int i = 0; i < 40 && w.battle.state != pm::BattleState::Attack; i++)
            pm::world_tick(w, pm::Input{});
        capture(w, 5300, out + "/battle_2_attack.ppm");
        // Every tick of the beat, because the whole point of it is motion and
        // a single frame of a flash proves nothing about the flash.
        for (int i = 0; w.battle.state == pm::BattleState::Attack && i < 20; i++) {
            char name[64];
            std::snprintf(name, sizeof name, "/beat_%02d.ppm", i);
            capture(w, 5400 + uint32_t(i) * 30, out + name);
            pm::world_tick(w, pm::Input{});
        }
        capture(w, 5800, out + "/battle_3_damage.ppm");
    }

    // The capture line: bag, ball, air, wobble, caught.
    {
        pm::World w = at_menu;
        w.battle.state = pm::BattleState::Menu;
        w.battle.cursor = 1;                 // BAG
        pm::world_tick(w, press_a());        // into the bag
        w.menu_pocket = 0;                   // balls
        w.menu_cursor = 0;
        capture(w, 6000, out + "/battle_4_bag.ppm");
        pm::world_tick(w, press_a());        // throw it
        run(w, pm::Input{}, 8);
        capture(w, 6400, out + "/battle_5_throw.ppm");
        for (int i = 0; i < 60 && w.battle.state != pm::BattleState::Wobble; i++)
            pm::world_tick(w, pm::Input{});
        run(w, pm::Input{}, 6);
        capture(w, 6800, out + "/battle_6_wobble.ppm");
        // Force the good ending: whether a real throw sticks is the RNG's
        // business, and a preview shot should not be at its mercy.
        for (int i = 0; i < 400 && w.battle.state != pm::BattleState::Caught; i++) {
            if (w.battle.state == pm::BattleState::Wobble) w.battle.wobbles = 3;
            pm::world_tick(w, press_a());
        }
        if (w.battle.state == pm::BattleState::Caught) {
            run(w, pm::Input{}, 4);
            capture(w, 7200, out + "/battle_7_caught.ppm");
            // And the beat after it, which is the one that mattered: the
            // creature used to be standing back on its mound here, under a
            // line of text saying it had just been caught.
            for (int i = 0; i < 60 && w.battle.state == pm::BattleState::Caught; i++)
                pm::world_tick(w, pm::Input{});
            capture(w, 7600, out + "/battle_8_gotcha.ppm");
        } else {
            std::printf("the ball never stuck, skipping the caught shot\n");
        }
    }
}

// Walk the player somewhere and let the fade finish, so a shot of a room is
// a shot of the room and not of the black beat between zones.
void go(pm::World& w, uint8_t zone, uint8_t x, uint8_t y, uint8_t facing) {
    w.mode = pm::Mode::Overworld;
    w.zone = zone;
    w.tx = x;
    w.ty = y;
    w.facing = facing;
    w.step = 0;
    w.fade = 0;
}

// The town, the mart and the gym: what the first map actually holds.
void town_shots(const std::string& out) {
    pm::World w;
    pm::world_init(w, 20260805);
    pm::world_tick(w, press_a());

    // The south end of Hometown, with the mart on the left and the gym on
    // the right. Both doors in one shot.
    go(w, pm::zone_hometown, 11, 16, 0);
    run(w, pm::Input{}, 2);
    capture(w, 6000, out + "/town_1_doors.ppm");

    // Inside the mart, at the counter, then the counter open.
    go(w, pm::zone_picomart, 5, 4, 0);
    run(w, pm::Input{}, 2);
    capture(w, 6400, out + "/town_2_mart.ppm");
    for (int i = 0; i < 20 && w.mode != pm::Mode::Shop; i++) {
        pm::world_tick(w, press_a());
    }
    capture(w, 6800, out + "/town_3_counter.ppm");
    // And what a shelf the player cannot afford looks like.
    w.money = 250;
    capture(w, 7000, out + "/town_4_counter_broke.ppm");

    // The gym: the entrance hall, with the first minion's sight line on the
    // ground, and the leader at the top.
    go(w, pm::zone_stonegym, 6, 14, 0);
    run(w, pm::Input{}, 2);
    capture(w, 7200, out + "/town_5_gym.ppm");

    go(w, pm::zone_stonegym, 10, 13, 0);
    run(w, pm::Input{}, 2);
    capture(w, 7400, out + "/town_6_gym_minion.ppm");

    go(w, pm::zone_stonegym, 6, 3, 0);
    run(w, pm::Input{}, 2);
    capture(w, 7600, out + "/town_7_gym_leader.ppm");
    pm::world_tick(w, press_a());
    capture(w, 7800, out + "/town_8_gym_challenge.ppm");
    // Walk away from a challenge that has been issued and not yet started,
    // which the shots below do, so leave it in a state a player could be in.
    w.battle = pm::Battle{};
    w.battle.trainer_npc = 0xFF;

    // Standing on the GREAT BALL lying on Route 1 and picking it up. This
    // used to be a sound effect and nothing else, so a find could be walked
    // over without ever knowing what had been found.
    go(w, pm::zone_route1, 5, 6, 2);
    run(w, pm::Input{}, 2);
    pm::world_tick(w, press_a());
    capture(w, 7900, out + "/town_15_found_item.ppm");
    run(w, press_a(), 2);

    // The gate on Route 1 that the badge opens.
    go(w, pm::zone_route1, 11, 4, 0);
    run(w, pm::Input{}, 2);
    capture(w, 8000, out + "/town_9_gate.ppm");

    // The DAY CARE on Route 1, which is the last building on the map that
    // opens rather than being scenery.
    go(w, pm::zone_route1, 16, 13, 0);
    run(w, pm::Input{}, 2);
    capture(w, 8100, out + "/town_13_daycare_door.ppm");
    go(w, pm::zone_daycare, 4, 5, 0);
    run(w, pm::Input{}, 2);
    capture(w, 8150, out + "/town_14_daycare.ppm");

    // The Centre: the nurse, and what a whiteout looks like from the other
    // side of it.
    go(w, pm::zone_healcentre, 6, 3, 0);
    run(w, pm::Input{}, 2);
    capture(w, 8200, out + "/town_10_centre.ppm");
    pm::world_tick(w, press_a());
    capture(w, 8400, out + "/town_11_nurse.ppm");
    run(w, press_a(), 4);

    // Lose everything out on the route, and wake up back at that counter.
    go(w, pm::zone_route1, 11, 20, 0);
    run(w, pm::Input{}, 2);
    w.mode = pm::Mode::Battle;
    w.battle = pm::Battle{};
    w.battle.foe = pm::make_mon(2, 30);
    w.battle.wild = true;
    w.battle.trainer_npc = 0xFF;
    w.battle.state = pm::BattleState::Message;
    for (int i = 0; i < w.party_count; i++) w.party[i].hp = 0;
    for (int i = 0; i < 200 && w.mode == pm::Mode::Battle; i++) {
        pm::world_tick(w, press_a());
    }
    capture(w, 8600, out + "/town_12_whiteout.ppm");
}

}  // namespace

int main(int argc, char** argv) {
    const std::string out = argc > 1 ? argv[1] : ".";

    pm::World w;
    pm::world_init(w, 20260805);

    // 1: the title, which is also the attract shot.
    capture(w, 1200, out + "/preview_1_title.ppm");

    pm::world_tick(w, press_a());

    // 2: standing in Hometown, the shot the thumbnail is taken from.
    run(w, pm::Input{}, 4);
    capture(w, 2000, out + "/preview_2_town.ppm");

    // 3: talking to somebody.
    w.tx = 14; w.ty = 13; w.facing = 3;
    run(w, pm::Input{}, 2);
    pm::world_tick(w, press_a());
    capture(w, 2400, out + "/preview_3_talk.ppm");
    run(w, press_a(), 6);

    // 4: Route 1, out among the tall grass.
    w.mode = pm::Mode::Overworld;
    w.zone = pm::zone_route1;
    w.tx = 11; w.ty = 17; w.facing = 0;
    w.fade = 0;
    run(w, hold(0), 6);
    capture(w, 3000, out + "/preview_4_route.ppm");

    // 5: a battle. Stand in the tall grass and walk until something comes out.
    w.tx = 4; w.ty = 6;
    for (int i = 0; i < 40000 && w.mode != pm::Mode::Battle; i++) {
        pm::world_tick(w, hold((i / 9) % 2 == 0 ? 1 : 3));
        if (w.mode == pm::Mode::Overworld && w.zone != pm::zone_route1) {
            w.zone = pm::zone_route1;
            w.tx = 4; w.ty = 6;
        }
    }
    if (w.mode == pm::Mode::Battle) {
        capture(w, 3600, out + "/preview_5_encounter.ppm");
        // Clear the intro, then sit on the move list.
        run(w, press_a(), 2);
        if (w.battle.state == pm::BattleState::Menu) {
            pm::world_tick(w, press_a());
        }
        capture(w, 4200, out + "/preview_6_moves.ppm");
        battle_shots(w, out);
    } else {
        std::printf("no encounter in 40000 ticks, skipping the battle shots\n");
    }

    // 7: the bag.
    w.mode = pm::Mode::Bag;
    w.menu_pocket = 0;
    w.menu_cursor = 0;
    capture(w, 4800, out + "/preview_7_bag.ppm");

    // 8: the party.
    w.mode = pm::Mode::Party;
    w.menu_cursor = 0;
    capture(w, 5200, out + "/preview_8_party.ppm");

    town_shots(out);

    return 0;
}
