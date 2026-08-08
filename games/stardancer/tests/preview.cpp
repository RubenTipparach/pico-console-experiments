// Renders real Star Dancer frames on the host, through the real engine, the
// real models and the real HUD, and writes them as PPM files. This is how the
// game gets looked at and tuned without a device in hand.
//
// It shows more than the other games' harnesses do, because Star Dancer draws
// its instruments with pse::draw_text into the RenderTarget rather than with
// the SDK's screen.text: the target box, the lead pip, the bars, the jump
// clock and the menus are all in these frames.
//
// Usage: stardancer_preview [out_dir]

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

uint32_t g_clock = 1000;
uint16_t g_worst_queued = 0;
uint16_t g_worst_dropped = 0;

void write_ppm(const char* path, const uint8_t* rgb) {
    FILE* f = std::fopen(path, "wb");
    if (!f) return;
    std::fprintf(f, "P6\n%d %d\n255\n", k_w, k_h);
    std::fwrite(rgb, 1, static_cast<size_t>(k_w) * k_h * 3, f);
    std::fclose(f);
}

void capture(const sd::World& world, const sdr::Chrome& chrome,
             const std::string& out, const char* name) {
    static std::vector<uint8_t> buffer(static_cast<size_t>(k_w) * k_h * 3);
    pse::RenderTarget target{buffer.data(), k_w, k_h, k_w * 3,
                             pse::PixelFormat::rgb888};
    sdr::render_scene(world, chrome, target, g_clock);

    const std::string path = out + "/" + name;
    write_ppm(path.c_str(), buffer.data());

    const sdr::FrameStats stats = sdr::last_frame_stats();
    if (stats.queued > g_worst_queued) g_worst_queued = stats.queued;
    if (stats.dropped > g_worst_dropped) g_worst_dropped = stats.dropped;
    std::printf("wrote %-28s %3u tris  depth %u..%u  hulls %u/%u%s\n",
                name, static_cast<unsigned>(stats.queued),
                static_cast<unsigned>(stats.near_units),
                static_cast<unsigned>(stats.far_units),
                static_cast<unsigned>(stats.hulls_drawn),
                static_cast<unsigned>(stats.hulls_live),
                stats.dropped ? "  DROPPED" : "");
}


// Render without writing anything, so a camera that eases over several frames
// can be driven to where it settles before the frame that gets kept.
void capture_quiet(const sd::World& world, const sdr::Chrome& chrome) {
    static std::vector<uint8_t> scratch(static_cast<size_t>(k_w) * k_h * 3);
    pse::RenderTarget target{scratch.data(), k_w, k_h, k_w * 3,
                             pse::PixelFormat::rgb888};
    sdr::render_scene(world, chrome, target, g_clock);
}

// Put the sortie straight into a named wave, with nothing else on the field.
void jump_to_wave(sd::World& world, uint8_t wave) {
    world.wave = wave;
    world.phase = sd::Phase::Briefing;
    world.wave_timer = 0;
    sd::Input none{};
    sd::world_tick(world, none);       // spawns it
}

// Fly the ship at whatever is targeted until it sits in a framing band, which
// is what puts a contact in shot rather than off the side of the frame or so
// close it fills it. Stops early when the shot is good, so a screenshot is a
// moment the game actually produces rather than a fixed tick count that
// happened to land somewhere.
void chase_until(sd::World& world, int max_ticks, int32_t near_units,
                 int32_t far_units, bool fire) {
    for (int i = 0; i < max_ticks; i++) {
        sd::Input in{};
        const sd::Ship* ship = sd::target_ship(world);
        if (ship == nullptr) {
            // The contact died. Take the next one rather than flying on with
            // no steering: without this the harness cruised into the arena
            // wall and photographed an empty sky with TURN BACK across it.
            in.cycle_target = true;
            sd::world_tick(world, in);
            g_clock += 10;
            if (sd::target_ship(world) == nullptr) return;
            continue;
        }
        {
            int32_t ax = ship->x, ay = ship->y, az = ship->z;
            const sd::Subsystem* sub = sd::target_subsystem(world);
            if (sub != nullptr) sd::sub_position(*ship, *sub, ax, ay, az);
            int32_t bx, by, bz;
            sd::bearing(world, ax, ay, az, bx, by, bz);
            if (by > 700) in.pitch = 1;
            else if (by < -700) in.pitch = -1;
            if (bx > 700) in.yaw = 1;
            else if (bx < -700) in.yaw = -1;
            in.fire = fire && bz > 15200;

            const int32_t range = sd::range_to(world, *ship) / sd::k_one;
            if (i > 40 && range >= near_units && range <= far_units &&
                bz > 16100) {
                return;
            }
        }
        sd::world_tick(world, in);
        g_clock += 10;
    }
}

void press_target(sd::World& world, int times) {
    for (int i = 0; i < times; i++) {
        sd::Input y{};
        y.cycle_target = true;
        sd::world_tick(world, y);
        g_clock += 10;
    }
}

// Press Y until a hull of this class is selected, optionally with one of its
// hardpoints. Gives up rather than looping, so a scene that cannot be set up
// shows as a plain frame instead of hanging the harness.
bool select_class(sd::World& world, sd::Hull cls, bool want_sub,
                  sd::Sub kind) {
    for (int i = 0; i < 26; i++) {
        const sd::Ship* ship = sd::target_ship(world);
        const sd::Subsystem* sub = sd::target_subsystem(world);
        if (ship != nullptr && ship->cls == cls) {
            if (!want_sub && sub == nullptr) return true;
            if (want_sub && sub != nullptr && sub->kind == kind) return true;
        }
        press_target(world, 1);
    }
    return false;
}

sdr::Chrome playing() {
    sdr::Chrome chrome{};
    chrome.screen = sdr::Screen::Play;
    chrome.sound_on = true;
    return chrome;
}

}  // namespace

int main(int argc, char** argv) {
    const std::string out = argc > 1 ? argv[1] : ".";

    // 1: the title, over the opening scene. This is the first thing a player
    // meets and the frame the gallery thumbnail comes from.
    sd::World world;
    sd::world_init(world);
    jump_to_wave(world, 1);
    press_target(world, 1);
    chase_until(world, 1200, 26, 52, false);
    {
        sdr::Chrome chrome = playing();
        chrome.screen = sdr::Screen::Title;
        chrome.item = sdr::kLaunch;
        capture(world, chrome, out, "preview_1_title.ppm");
    }

    // 2: a fighter in the reticle, boxed, with the lead pip on it.
    chase_until(world, 900, 9, 20, true);
    capture(world, playing(), out, "preview_2_dogfight.ppm");

    // 3: bombers, which are the wave that tests telling one hull from another.
    sd::World bombers;
    sd::world_init(bombers, 0x00B0B0B0u);
    jump_to_wave(bombers, 3);
    // Closed on the escort first, unarmed, so the bombers are still alive to
    // be photographed rather than dead somewhere behind the camera.
    select_class(bombers, sd::Hull::Bomber, false, sd::Sub::Engines);
    chase_until(bombers, 1600, 10, 26, false);
    select_class(bombers, sd::Hull::Bomber, false, sd::Sub::Engines);
    capture(bombers, playing(), out, "preview_3_bombers.ppm");

    // 4: a gunship with one of its turrets selected, so the box is on the
    // sponson rather than round the whole ship.
    sd::World gun;
    sd::world_init(gun, 0x0C0FFEE1u);
    jump_to_wave(gun, 4);
    select_class(gun, sd::Hull::Gunship, true, sd::Sub::Weapons);
    chase_until(gun, 1600, 16, 40, true);
    capture(gun, playing(), out, "preview_4_gunship_turret.ppm");

    // 5: the frigate with the jump clock running, which is the shot that has
    // to say what the whole sortie is about.
    sd::World cap;
    sd::world_init(cap, 0x0F819A7Eu);
    jump_to_wave(cap, 5);
    select_class(cap, sd::Hull::Frigate, false, sd::Sub::Navigation);
    chase_until(cap, 2000, 44, 96, false);
    capture(cap, playing(), out, "preview_5_frigate.ppm");

    // 6: closed to gun range with the navigation array selected, which is the
    // hardpoint the mission turns on.
    select_class(cap, sd::Hull::Frigate, true, sd::Sub::Navigation);
    chase_until(cap, 2000, 24, 60, true);
    capture(cap, playing(), out, "preview_6_nav_run.ppm");

    // 7: the padlock view. Turned away from the frigate, holding the target
    // button, so the camera has swung off the nose and onto it: the ship is
    // across the frame and the contact is in the middle of it.
    {
        sd::Input yaw{};
        yaw.yaw = 1;
        for (int i = 0; i < 70; i++) { sd::world_tick(cap, yaw); g_clock += 10; }
        sdr::Chrome chrome = playing();
        chrome.look_at_target = true;
        // Several frames, because the whole point is that it eases there.
        for (int f = 0; f < 40; f++) {
            g_clock += 16;
            capture_quiet(cap, chrome);
        }
        capture(cap, chrome, out, "preview_7_padlock.ppm");
    }

    // 8: the pause menu, over the battle it interrupts.
    {
        sdr::Chrome chrome = playing();
        chrome.screen = sdr::Screen::Paused;
        chrome.item = sdr::kPauseSound;
        capture(cap, chrome, out, "preview_8_pause.ppm");
    }

    // 9: the debrief.
    {
        sd::World done = cap;
        done.phase = sd::Phase::Won;
        sdr::Chrome chrome = playing();
        chrome.screen = sdr::Screen::Debrief;
        chrome.best_score = done.score;
        capture(done, chrome, out, "preview_9_debrief.ppm");
    }

    std::printf("worst frame: %u triangles, %u dropped\n",
                static_cast<unsigned>(g_worst_queued),
                static_cast<unsigned>(g_worst_dropped));
    if (g_worst_dropped > 0) {
        std::printf("FAIL: the frame queue overflowed, so a hull was drawn "
                    "with holes in it\n");
        return 1;
    }
    return 0;
}
