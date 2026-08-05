// The trophy shot has to fit the band it is drawn in.
//
// The catch card renders the fish through the real engine into the top
// viewport, and the top viewport ends at the split. Nothing about that is
// checked by the sim tests, which is how a trophy came to be drawn a metre
// below the camera's aim point: every species looked fine except the big ones,
// whose bellies were cut off by the split. The species worth photographing
// were the only ones photographed badly.
//
// So this renders a real frame per species at its maximum size and measures
// where the fish actually lands. It is a rendering test, so it needs the real
// engine and the real models, and it runs alongside the preview harness for
// that reason.

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

}  // namespace

int main() {
    test_every_trophy_fits_the_band();
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
