// Renders real Kingfisher frames on the host, through the real engine and the
// real generated models, and writes them as PPM files. This is how the game
// gets looked at and tuned without a device in hand: everything except text is
// the exact code the PicoSystem runs.
//
// Usage: kingfisher_preview [out_dir]

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "pse/pixel.hpp"

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

void capture(const kf::World& world, uint32_t time_ms, const std::string& path) {
    static std::vector<uint8_t> buffer(static_cast<size_t>(k_w) * k_h * 3);
    pse::RenderTarget target{buffer.data(), k_w, k_h, k_w * 3,
                             pse::PixelFormat::rgb888};
    kfr::render_scene(world, target, time_ms);
    write_ppm(path.c_str(), buffer.data());
    std::printf("wrote %s\n", path.c_str());
}

void run(kf::World& world, const kf::Input& input, int ticks) {
    for (int i = 0; i < ticks; i++) kf::world_tick(world, input);
}

}  // namespace

int main(int argc, char** argv) {
    const std::string out = argc > 1 ? argv[1] : ".";
    kf::World world;
    kf::world_init(world, 20260804);

    // 1: morning pond, fish wandering.
    run(world, kf::Input{}, 2600);
    capture(world, 86000, out + "/preview_1_day.ppm");

    // 2: aiming, meter mid swing.
    {
        kf::Input press{};
        press.a = true;
        press.a_pressed = true;
        kf::world_tick(world, press);
        kf::Input hold{};
        hold.a = true;
        run(world, hold, 30);
        capture(world, 87000, out + "/preview_2_aim.ppm");
    }

    // 3: cast released, lure sinking, twitched once.
    {
        kf::Input release{};
        release.a_released = true;
        kf::world_tick(world, release);
        run(world, kf::Input{}, 70);          // flight
        kf::Input twitch{};
        twitch.right = true;
        run(world, twitch, 2);
        run(world, kf::Input{}, 240);
        capture(world, 91000, out + "/preview_3_sinking.ppm");
    }

    // 4: a fight against something deep, tension climbing.
    {
        kf::world_test_hook(world, 10, 150);   // a big sturgeon
        kf::Input hook{};
        hook.a_pressed = true;
        kf::world_tick(world, hook);
        // Enough greedy reeling to push the tension bar into the caution
        // colours for the shot, not enough to snap.
        kf::Input reel{};
        reel.a = true;
        run(world, reel, 60);
        capture(world, 95000, out + "/preview_4_fight.ppm");
    }

    // 4b: the same fight with the fish run out, taking its second wind. The
    // stamina bar goes dark blue for this window, which is the one time
    // pulling is free, so it is worth being able to look at.
    {
        for (int t = 0; t < 30000 && world.mode == kf::Mode::Fight &&
                        world.spent_timer == 0; t++) {
            kf::Input input{};
            input.a = world.tension < kf::k_tension_danger - 80;
            if (t % 3 == 0) {
                if ((t / 3) % 2 == 0) input.left_pressed = true;
                else input.right_pressed = true;
            }
            kf::world_tick(world, input);
        }
        // A moment in, so the bar has refilled enough to read.
        run(world, kf::Input{}, 70);
        if (world.mode == kf::Mode::Fight) {
            capture(world, 96000, out + "/preview_4b_spent.ppm");
        } else {
            std::printf("fight ended before the second wind, no spent frame\n");
        }
    }

    // 5: the catch card trophy, landed with the full technique: work the reel
    // whenever the meter is out of the red, wiggle throughout.
    {
        for (int t = 0; t < 30000 && world.mode == kf::Mode::Fight; t++) {
            kf::Input input{};
            input.a = world.tension < kf::k_tension_danger - 80;
            if (t % 3 == 0) {
                if ((t / 3) % 2 == 0) input.left_pressed = true;
                else input.right_pressed = true;
            }
            kf::world_tick(world, input);
        }
        if (world.mode == kf::Mode::Landed) {
            capture(world, 99000, out + "/preview_5_trophy.ppm");
        } else {
            std::printf("fight did not land, no trophy frame\n");
        }
    }

    // 6: night, in the rain. Eight ninths of the day is 10pm; three quarters
    // used to be night and is 7:30pm now that the day ends at midnight, which
    // would have quietly turned this into a dusk frame.
    {
        world.day_tick = 8 * kf::k_day_length / 9;
        world.raining = 1;
        world.weather_timer = 4000;
        run(world, kf::Input{}, 900);
        capture(world, 132000, out + "/preview_6_night.ppm");
    }

    // 7: a bite at maximum distance. The far float projects within rows of
    // the horizon, which is exactly where the old bounds check used to drop
    // the "!" glyph: this frame proves the alert survives at 45 m out, in
    // both halves of the screen.
    {
        world.raining = 0;
        world.day_tick = kf::k_day_length / 8;
        world.mode = kf::Mode::Sinking;
        world.lure_x = 0;
        world.lure_y = kf::k_one;
        world.lure_z = 45 * kf::k_one;
        world.bite_timer = 40;
        kf::Fish& biter = world.fish[0];
        biter.state = kf::FishState::Biting;
        biter.species = 9;   // a catfish, deep water
        biter.size_cm = 80;
        biter.x = world.lure_x;
        biter.z = world.lure_z;
        biter.y = kf::k_one;
        capture(world, 134000, out + "/preview_7_bite_far.ppm");
    }

    return 0;
}
