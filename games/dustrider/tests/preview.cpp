// Renders real Dust Rider frames on the host, through the real engine and
// the real generated models, and writes them as PPM files. This is how the
// game gets looked at and tuned without a device in hand.
//
// Usage: dustrider_preview [out_dir]

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "pse/pixel.hpp"

#include "bot.hpp"
#include "render.hpp"
#include "sim.hpp"

namespace {

constexpr int k_w = 120;
constexpr int k_h = 120;

void write_ppm(const char* path, const uint8_t* rgb) {
    FILE* f = std::fopen(path, "wb");
    if (!f) return;
    std::fprintf(f, "P6\n%d %d\n255\n", k_w, k_h);
    std::fwrite(rgb, 1, static_cast<size_t>(k_w) * k_h * 3, f);
    std::fclose(f);
}

void capture(const dr::World& world, uint32_t time_ms,
             const std::string& path) {
    static std::vector<uint8_t> buffer(static_cast<size_t>(k_w) * k_h * 3);
    pse::RenderTarget target{buffer.data(), k_w, k_h, k_w * 3,
                             pse::PixelFormat::rgb888};
    drr::render_scene(world, target, time_ms);
    write_ppm(path.c_str(), buffer.data());
    std::printf("wrote %s (%dm)\n", path.c_str(), dr::distance_m(world));
}

void bot_until(dr::World& world, int max_ticks, bool (*done)(const dr::World&)) {
    for (int i = 0; i < max_ticks && world.alive; i++) {
        dr::world_tick(world, dr::bot_input(world));
        if (done && done(world)) return;
    }
}

}  // namespace

int main(int argc, char** argv) {
    const std::string out = argc > 1 ? argv[1] : ".";
    uint32_t clock = 1000;

    // 1: the start line, exactly what the title screen sits over.
    dr::World world;
    dr::world_init(world, 20260804);
    capture(world, clock, out + "/preview_1_start.ppm");

    // 2: the launch wheelie, nose up under full throttle.
    for (int i = 0; i < 55; i++) {
        dr::Input in{};
        in.throttle = true;
        dr::world_tick(world, in);
    }
    for (int f = 0; f < 12; f++) {
        clock += 33;   // let the eased pitch land where play would show it
        capture(world, clock, out + "/tmp.ppm");
    }
    capture(world, clock, out + "/preview_2_wheelie.ppm");

    // 3: cruising the open desert.
    bot_until(world, 1400, nullptr);
    for (int f = 0; f < 10; f++) { clock += 33; capture(world, clock, out + "/tmp.ppm"); }
    capture(world, clock, out + "/preview_3_cruise.ppm");

    // 4: airborne off a crest.
    bot_until(world, 30000, [](const dr::World& w) { return !w.grounded; });
    for (int i = 0; i < 8 && world.alive; i++) {
        dr::world_tick(world, dr::bot_input(world));
    }
    for (int f = 0; f < 6; f++) { clock += 33; capture(world, clock, out + "/tmp.ppm"); }
    capture(world, clock, out + "/preview_4_jump.ppm");

    // 5: a railed stretch.
    bot_until(world, 60000, [](const dr::World& w) {
        return dr::track_rail_at(w, w.x + (3 << 8));
    });
    for (int f = 0; f < 6; f++) { clock += 33; capture(world, clock, out + "/tmp.ppm"); }
    capture(world, clock, out + "/preview_5_rail.ppm");

    // 6: a cactus dead ahead.
    bot_until(world, 60000, [](const dr::World& w) {
        int32_t cx; bool sand;
        return dr::track_next_cactus(w, w.x, 4 << 8, cx, sand);
    });
    for (int f = 0; f < 6; f++) { clock += 33; capture(world, clock, out + "/tmp.ppm"); }
    capture(world, clock, out + "/preview_6_cactus.ppm");

    // 7: the wreck. Stage a cactus in the riding lane and hit it.
    {
        dr::World doomed;
        dr::world_init(doomed, 99);
        dr::world_test_flat(doomed, true);
        dr::world_test_clear_hazards(doomed);
        dr::Input in{};
        in.throttle = true;
        for (int i = 0; i < 60; i++) dr::world_tick(doomed, in);
        dr::world_test_place_cactus(doomed, doomed.x + (10 << 8), false);
        for (int i = 0; i < 900 && doomed.alive; i++) dr::world_tick(doomed, in);
        for (int i = 0; i < 25; i++) dr::world_tick(doomed, dr::Input{});
        for (int f = 0; f < 10; f++) { clock += 33; capture(doomed, clock, out + "/tmp.ppm"); }
        capture(doomed, clock, out + "/preview_7_wreck.ppm");
    }

    std::remove((out + "/tmp.ppm").c_str());
    return 0;
}
