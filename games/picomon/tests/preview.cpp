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

    return 0;
}
