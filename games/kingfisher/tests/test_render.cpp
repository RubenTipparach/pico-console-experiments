// What the fight and the catch card actually put on screen.
//
// Everything here renders a real frame through the real engine and the real
// models and reads the pixels back, because these are claims about what a
// player sees and there is no other way to check one.
//
// The trophy shot has to fit the band it is drawn in. The catch card renders
// the fish into the top viewport, and the top viewport ends at the split.
// Nothing about that was checked by the sim tests, which is how a trophy came
// to be drawn a metre below the camera's aim point: every species looked fine
// except the big ones, whose bellies were cut off by the split. The species
// worth photographing were the only ones photographed badly.
//
// The stamina meter has to say which state the fish is in. It goes dark blue
// while a spent fish takes its second wind, and that window is the one time
// pulling is free, so a player reading the bar at a glance has to be able to
// tell it apart from the bright bar that means stop.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "pse/pixel.hpp"

#include "render.hpp"
#include "sim.hpp"

namespace {

constexpr int k_w = 120;
constexpr int k_h = 120;
constexpr int k_split = 60;   // must match render.cpp's band split

int g_checks = 0;
int g_failures = 0;

void check(bool ok, const char* what) {
    g_checks++;
    if (!ok) {
        g_failures++;
        std::printf("FAIL %s\n", what);
    }
}

// Where the trophy lands, found by rendering the same frame twice and
// differencing: once with the fish and once with card_species cleared, which
// is the one thing in render.cpp that the trophy mesh hangs off. Everything
// else in the band, the shoreline and its trees and the drifting motes, is
// identical between the two and cancels.
//
// Reading the frame on its own does not work, and the first version of this
// test proved it by passing with the bug still in: the shoreline differs from
// its background too, so every species measured as starting at row 9 and the
// fish's own extent was lost in it.
struct Span { int top; int bottom; int pixels; };

Span trophy_span(const uint8_t* with_fish, const uint8_t* without) {
    Span span{-1, -1, 0};
    for (int y = 0; y < k_split; y++) {
        int row = 0;
        for (int x = 0; x < k_w; x++) {
            int d = 0;
            for (int c = 0; c < 3; c++) {
                const int i = (y * k_w + x) * 3 + c;
                d += std::abs(static_cast<int>(with_fish[i]) -
                              static_cast<int>(without[i]));
            }
            if (d > 24) row++;
        }
        if (row >= 2) {
            if (span.top < 0) span.top = y;
            span.bottom = y;
            span.pixels += row;
        }
    }
    return span;
}

void render_card(kf::World& world, int species, int size, uint8_t* out) {
    kf::world_init(world, 5);
    world.mode = kf::Mode::Landed;
    world.card_species = static_cast<int8_t>(species);
    world.card_size = static_cast<int16_t>(size);
    world.card_timer = 200;
    pse::RenderTarget target{out, k_w, k_h, k_w * 3, pse::PixelFormat::rgb888};
    kfr::render_scene(world, target, 99000);
}

void test_every_trophy_fits_the_band() {
    std::vector<uint8_t> shown(static_cast<size_t>(k_w) * k_h * 3);
    std::vector<uint8_t> bare(static_cast<size_t>(k_w) * k_h * 3);

    for (int s = 0; s < kf::k_species_count; s++) {
        const int size = kf::k_species[s].size_max;
        kf::World a, b;
        render_card(a, s, size, shown.data());
        render_card(b, -1, size, bare.data());

        const Span span = trophy_span(shown.data(), bare.data());
        if (span.top < 0) {
            std::printf("  %-13s nothing drawn\n", kf::k_species[s].name);
            check(false, "the trophy is drawn at all");
            continue;
        }

        // The camera aims at the fish, so the fish belongs in the middle of
        // the band the camera is shooting into. This is the check that
        // matters: a trophy pushed down the band is one about to lose its
        // belly to the split, and it catches that before the mesh is big
        // enough to actually touch the edge.
        const int centre = (span.top + span.bottom) / 2;
        const int drift = std::abs(centre - k_split / 2);

        // Reaching either edge means the mesh ran off the band and lost
        // whatever was past it. A clipped fish stops dead at the boundary
        // rather than where its body ends, so every species ends on the same
        // row however big it is: that was the tell when this was broken.
        const bool cut_low = span.bottom >= k_split - 2;
        const bool cut_high = span.top <= 1;

        std::printf("  %-13s %3d cm  rows %2d..%-2d  centre %2d  %s\n",
                    kf::k_species[s].name, size, span.top, span.bottom, centre,
                    (cut_low || cut_high || drift > 8) ? "BAD" : "clear");
        check(!cut_low, "the trophy clears the split");
        check(!cut_high, "the trophy clears the top of the frame");
        check(drift <= 8, "the trophy is centred in its viewport");
    }
}

// The bar is four pixels wide at x=3 under the split, and it fills from the
// bottom, so its lowest cell is the one that is lit soonest.
constexpr int k_bar_x = 4;
constexpr int k_bar_bottom = k_split + 9 + 40 - 3;

void test_the_stamina_bar_marks_the_second_wind() {
    std::vector<uint8_t> recovering(static_cast<size_t>(k_w) * k_h * 3);
    std::vector<uint8_t> fighting(static_cast<size_t>(k_w) * k_h * 3);

    kf::World world;
    kf::world_init(world, 12);
    kf::world_test_hook(world, 10, 160);          // a sturgeon, worth a fight
    kf::Input hook{};
    hook.a_pressed = true;
    kf::world_tick(world, hook);

    // Wear it down the way a player does, until the second wind opens, then
    // let it refill a little: at the instant it opens the fish is on zero and
    // the bar has nothing lit to read.
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
    check(world.mode == kf::Mode::Fight, "the fish is still on for the shot");
    check(world.spent_timer > 0, "the second wind is open");
    for (int t = 0; t < 60 && world.mode == kf::Mode::Fight &&
                    world.spent_timer > 0; t++) {
        kf::world_tick(world, kf::Input{});
    }
    check(world.stamina > 0, "the bar has something lit to read");

    // Render the same world twice, once as it stands and once with the
    // recovery flag cleared and nothing else touched. Any pixel that differs
    // between the two is the bar, which is the only thing the flag drives.
    // Reading a colour straight off one frame does not work: the water behind
    // the bar is a bright gradient, and the first version of this test scored
    // the water and passed with an empty bar.
    kf::World shown = world;
    pse::RenderTarget a{recovering.data(), k_w, k_h, k_w * 3,
                        pse::PixelFormat::rgb888};
    kfr::render_scene(shown, a, 50000);

    kf::World lit = world;
    lit.spent_timer = 0;
    pse::RenderTarget b{fighting.data(), k_w, k_h, k_w * 3,
                        pse::PixelFormat::rgb888};
    kfr::render_scene(lit, b, 50000);

    const int i = (k_bar_bottom * k_w + k_bar_x) * 3;
    const int rr = recovering[i], rg = recovering[i + 1], rb = recovering[i + 2];
    const int fr = fighting[i], fg = fighting[i + 1], fb = fighting[i + 2];
    std::printf("  stamina bar  fighting rgb(%d,%d,%d)  recovering rgb(%d,%d,%d)\n",
                fr, fg, fb, rr, rg, rb);

    check(rr != fr || rg != fg || rb != fb,
          "the recovery state changes the bar at all");
    // Dark and blue: blue has to lead, and the whole thing has to be darker
    // than the bar it replaces, or the two states read the same at a glance.
    check(rb > rg && rb > rr, "the recovering bar is blue");
    check(rr + rg + rb < fr + fg + fb, "the recovering bar is darker");
    check(rg < fg, "the recovering bar is not the bright one");
}

}  // namespace

// The underwater viewport is the water column under the lure: the surface
// along its top edge and the bed along its bottom. So a hook wound all the way
// down has to be drawn near the bottom of the band, at every cast distance.
//
// This is a claim about the camera, and the camera was wrong twice. It drew
// the hook at a fixed depth, and then it framed the deepest column everywhere,
// which left a hook resting on the bed of the shallows a third of the way down
// a mostly empty band, looking like it would not sink.
void render_lure(kf::World& world, int32_t z, int32_t depth, uint8_t* out) {
    kf::world_init(world, 7);
    world.mode = kf::Mode::Sinking;
    world.lure_x = 0;
    world.lure_z = z;
    world.lure_y = depth;
    world.lure_target_y = depth;
    // The fish are not what is being measured, and a fish crossing the centre
    // column would read as line.
    for (auto& f : world.fish) f.state = kf::FishState::Gone;
    pse::RenderTarget target{out, k_w, k_h, k_w * 3, pse::PixelFormat::rgb888};
    // The drawn hook depth is eased toward the sim's, 8 percent a frame, and
    // it is a static that carries over between renders. One frame measures
    // that ease rather than where the hook belongs, which is exactly what the
    // first version of this test did: every depth came out on the same row.
    for (int i = 0; i < 200; i++) kfr::render_scene(world, target, 99000);
}

// The lowest row of line, down the centre column where a lure at x = 0 hangs.
// The line is near white and the water it crosses is dark, so it reads without
// differencing two frames.
int line_bottom(const uint8_t* fb) {
    int last = -1;
    for (int y = k_split + 1; y < k_h; y++) {
        for (int x = 57; x <= 63; x++) {
            const int i = (y * k_w + x) * 3;
            if (fb[i] > 150 && fb[i + 1] > 150 && fb[i + 2] > 150) {
                last = y;
                break;
            }
        }
    }
    return last;
}

void test_the_hook_reaches_the_bottom_of_the_band() {
    std::vector<uint8_t> deep(static_cast<size_t>(k_w) * k_h * 3);
    std::vector<uint8_t> shallow(static_cast<size_t>(k_w) * k_h * 3);

    // One cast in each band: the shallows by the boat, midwater, and the far
    // deep water. Every one of them has a bed, and it belongs at the bottom.
    const int32_t casts[3] = {8 * 256, 24 * 256, 42 * 256};
    const int band_bottom = k_split + ((k_h - k_split) * 2) / 3;

    for (const int32_t z : casts) {
        kf::World probe;
        kf::world_init(probe, 7);
        probe.lure_z = z;
        const int32_t floor_y = kf::water_depth_here(probe);

        kf::World a, b;
        render_lure(a, z, floor_y, deep.data());
        render_lure(b, z, kf::k_lure_min_depth_fp, shallow.data());

        const int low = line_bottom(deep.data());
        const int high = line_bottom(shallow.data());
        std::printf("  cast %2dm  floor %4d fp  deep row %3d  shallow row %3d\n",
                    static_cast<int>(z / 256), static_cast<int>(floor_y),
                    low, high);

        check(low >= 0, "a hook on the bed is drawn at all");
        check(high >= 0, "a hook under the surface is drawn at all");
        check(low >= band_bottom, "a hook on the bed reaches the bottom third");
        check(high < low, "raising the lure raises the hook on screen");
    }
}

// The boat is the player's own position in the scene, and the top viewport
// should not lose it. The fight camera pans its orbit target onto the hooked
// fish, and a fish out at the side of the lake took the shot off the boat
// entirely: the player was left watching open water with a line running out
// of frame.
//
// Found by colour. The boat is the only brown thing above the water: the lake
// and sky are blue, the shore and its trees green. Pinned to the middle of the
// day so a sunset horizon cannot read as timber.
bool boat_in_top_band(const uint8_t* fb) {
    int found = 0;
    for (int y = 0; y < k_split; y++) {
        for (int x = 0; x < k_w; x++) {
            const int i = (y * k_w + x) * 3;
            const int r = fb[i], g = fb[i + 1], b = fb[i + 2];
            if (r > 90 && r > g + 25 && g > b + 15) found++;
        }
    }
    return found >= 8;
}

void test_the_boat_stays_in_shot_during_a_fight() {
    std::vector<uint8_t> frame(static_cast<size_t>(k_w) * k_h * 3);
    pse::RenderTarget target{frame.data(), k_w, k_h, k_w * 3,
                             pse::PixelFormat::rgb888};

    // Every corner of the lake a fish can be hooked in, including hard against
    // the side at the far end, which is the worst case for the pan.
    const int32_t xs[3] = {0, -12 * 256, 12 * 256};
    const int32_t zs[3] = {4 * 256, 24 * 256, 46 * 256};

    for (const int32_t fx : xs) {
        for (const int32_t fz : zs) {
            kf::World world;
            kf::world_init(world, 3);
            world.day_tick = kf::k_day_length / 3;   // full daylight
            const int hooked = kf::world_test_hook(world, 4, 40);
            kf::Input hook{};
            hook.a_pressed = true;
            kf::world_tick(world, hook);
            if (world.mode != kf::Mode::Fight) {
                check(false, "the test hook starts a fight");
                return;
            }
            world.fish[hooked].x = fx;
            world.fish[hooked].z = fz;
            world.lure_x = fx;
            world.lure_z = fz;

            for (int i = 0; i < 200; i++) kfr::render_scene(world, target, 4000);

            const bool seen = boat_in_top_band(frame.data());
            std::printf("  fight at x %4d z %4d  boat %s\n",
                        static_cast<int>(fx / 256), static_cast<int>(fz / 256),
                        seen ? "in shot" : "GONE");
            check(seen, "the boat stays in the top viewport during a fight");
        }
    }
}

int main() {
    test_every_trophy_fits_the_band();
    test_the_hook_reaches_the_bottom_of_the_band();
    test_the_boat_stays_in_shot_during_a_fight();
    test_the_stamina_bar_marks_the_second_wind();
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
