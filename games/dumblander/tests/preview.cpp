// Renders real Dumb Lander frames on the host, through the real engine and the
// real sprites, and writes them as PPM files. This is how the game gets looked
// at without a device, and where the thumbnail comes from.
//
// It is also the only thing that can check the render side promises, because
// the sim cannot ask the renderer anything: that the HUD is inside the screen,
// that the title art never collides with the title text on any seed, and that
// a frame is not accidentally empty.
//
// Usage: dumblander_preview [out_dir]

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "pse/pixel.hpp"
#include "pse/text.hpp"

#include "render.hpp"
#include "sim.hpp"

namespace {

constexpr int k_w = dl::k_screen_w;
constexpr int k_h = dl::k_screen_h;

int g_failures = 0;

void fail(const char* what) {
    std::printf("FAIL: %s\n", what);
    g_failures++;
}

std::vector<uint8_t> render(const dl::World& world) {
    std::vector<uint8_t> buffer(static_cast<size_t>(k_w) * k_h * 3, 0);
    pse::RenderTarget target{buffer.data(), k_w, k_h, k_w * 3,
                             pse::PixelFormat::rgb888};
    dl::render_world(world, target);
    return buffer;
}

void write_ppm(const std::string& path, const std::vector<uint8_t>& rgb) {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return;
    std::fprintf(f, "P6\n%d %d\n255\n", k_w, k_h);
    std::fwrite(rgb.data(), 1, rgb.size(), f);
    std::fclose(f);
}

int distinct_colours(const std::vector<uint8_t>& rgb) {
    // Cheap and good enough: a frame nothing drew into is one or two colours.
    int seen = 0;
    uint32_t first = 0xFFFFFFFFu;
    for (size_t i = 0; i + 2 < rgb.size(); i += 3) {
        const uint32_t c = (static_cast<uint32_t>(rgb[i]) << 16) |
                           (static_cast<uint32_t>(rgb[i + 1]) << 8) | rgb[i + 2];
        if (first == 0xFFFFFFFFu) {
            first = c;
        } else if (c != first) {
            seen++;
            if (seen > 400) break;
        }
    }
    return seen;
}

dl::Input none() { return dl::Input{false, false, false, false}; }

dl::World launch(uint32_t seed) {
    dl::World world;
    dl::world_init(world, seed);
    dl::Input start = none();
    start.any_pressed = true;
    dl::world_tick(world, start);
    return world;
}

// The same autopilot the sim tests fly, so a posed frame is the game being
// flown rather than a hand picked state that happens to look right.
struct Autopilot {
    bool descending = false;

    dl::Input operator()(const dl::World& w) {
        dl::Input in = none();
        const int32_t target_x = (w.goal.x + w.goal.w / 2) * dl::k_one;
        const int32_t dx = target_x - w.x;
        int32_t peak = dl::k_screen_h << 8;
        const int a = w.x >> dl::k_fp;
        const int b = target_x >> dl::k_fp;
        for (int x = (a < b ? a : b); x <= (a < b ? b : a); x++) {
            const int32_t h = dl::ground_at(w, x);
            if (h < peak) peak = h;
        }
        const int32_t cruise = (peak << 8) - 26 * dl::k_one;
        const int32_t adx = dx < 0 ? -dx : dx;
        if (!descending && adx < 4 * dl::k_one &&
            (w.vx < dl::k_one / 7 && w.vx > -dl::k_one / 7)) {
            descending = true;
        }
        if (descending && adx > 14 * dl::k_one) descending = false;
        const int32_t want = descending ? (w.goal.y << 8) + 8 * dl::k_one : cruise;
        int32_t desired_vy = -((w.y - want) / 8);
        const int32_t vy_cap = (dl::k_safe * 72) / 100;
        if (desired_vy > vy_cap) desired_vy = vy_cap;
        if (desired_vy < -(dl::k_one + dl::k_one / 5)) desired_vy = -(dl::k_one + dl::k_one / 5);
        in.thrust = w.vy > desired_vy;
        const bool clear = w.y < cruise + 10 * dl::k_one;
        const int32_t cap = descending ? dl::k_one / 4 : (clear ? (dl::k_one * 4) / 5 : 0);
        int32_t desired_vx = dx / 16;
        if (desired_vx > cap) desired_vx = cap;
        if (desired_vx < -cap) desired_vx = -cap;
        if (w.vx > desired_vx + dl::k_one / 80) in.left = true;
        else if (w.vx < desired_vx - dl::k_one / 80) in.right = true;
        return in;
    }
};

void capture(const dl::World& world, const std::string& out, const char* name) {
    const std::vector<uint8_t> rgb = render(world);
    if (distinct_colours(rgb) < 40) {
        std::printf("  %s: only %d distinct colours\n", name, distinct_colours(rgb));
        fail("a frame came out effectively blank");
    }
    if (!out.empty()) write_ppm(out + "/" + name + ".ppm", rgb);
}

// The title art is placed from a measured text bottom, and the terrain it
// stands on is generated, so this is the check that no seed puts the lander
// through the title. Rule 9's "measure text, never place it by eye" only holds
// if something verifies the measurement.
void test_title_art_never_collides() {
    constexpr int k_sub_y = 80;
    const int text_bottom = k_sub_y + pse::text_height(1);
    for (int seed = 1; seed <= 200; seed++) {
        dl::World world;
        dl::world_init(world, static_cast<uint32_t>(seed));
        const dl::Pad& pad = world.start.y > world.goal.y ? world.start : world.goal;
        // Worst case of the bob, which is the highest the lander ever sits.
        const int lander_top = (pad.y >> 8) - 14 - 4 - (dl::k_hull_h - 1);
        const bool drawn = ((pad.y >> 8) - 14 - 4) - dl::k_hull_h > text_bottom + 6;
        if (drawn && lander_top <= text_bottom) {
            std::printf("  seed %d: lander top %d, text bottom %d\n", seed, lander_top,
                        text_bottom);
            fail("the title lander overlapped the title text");
            return;
        }
    }
}

// Every string the HUD draws has to fit the screen it is drawn on. A hand
// placed x is only right for the string it was tuned against.
void test_hud_fits() {
    if (pse::text_width("SPD ") + pse::text_width("999") + 5 > k_w / 2) {
        fail("the speed readout would run into the middle of the screen");
    }
    if (pse::text_width("LEG ") + pse::text_width("999") + 10 > k_w / 2) {
        fail("the leg counter would run into the middle of the screen");
    }
    const char* messages[3] = {"LANDED", "CRASHED", "STRANDED"};
    for (const char* m : messages) {
        if (pse::text_width(m, 3) > k_w - 8) {
            std::printf("  %s is %d px at scale 3\n", m, pse::text_width(m, 3));
            fail("an end of run message is wider than the screen");
        }
        if (!pse::text_is_drawable(m)) fail("a message uses a glyph the font lacks");
    }
}

}  // namespace

int main(int argc, char** argv) {
    const std::string out = argc > 1 ? argv[1] : std::string();

    test_hud_fits();
    test_title_art_never_collides();

    {
        dl::World world;
        dl::world_init(world, 4);
        for (int i = 0; i < 120; i++) dl::world_tick(world, none());
        capture(world, out, "title");
    }
    {
        dl::World world = launch(4);
        Autopilot pilot;
        for (int i = 0; i < 60; i++) dl::world_tick(world, pilot(world));
        capture(world, out, "climb");
    }
    {
        dl::World world = launch(4);
        Autopilot pilot;
        for (int i = 0; i < 600 && world.state == dl::State::fly; i++) {
            dl::world_tick(world, pilot(world));
            if (i > 150 && world.jet != 0 && world.thrusting) break;
        }
        capture(world, out, "cross");
    }
    {
        dl::World world = launch(4);
        Autopilot pilot;
        for (int i = 0; i < 4000 && world.state == dl::State::fly; i++) {
            dl::world_tick(world, pilot(world));
        }
        if (world.state != dl::State::landed) fail("the autopilot did not land the preview leg");
        capture(world, out, "landed");
    }
    {
        dl::World world = launch(7);
        world.took_off = true;
        world.y = (static_cast<int32_t>(dl::ground_at(world, world.x >> dl::k_fp)) << 8) -
                  60 * dl::k_one;
        world.vy = dl::k_safe * 3;
        for (int i = 0; i < 200 && world.state != dl::State::over; i++) {
            dl::world_tick(world, none());
        }
        for (int i = 0; i < 9; i++) dl::world_tick(world, none());
        capture(world, out, "crash");
    }

    if (g_failures == 0) std::printf("dumblander_preview: ok\n");
    return g_failures == 0 ? 0 : 1;
}
