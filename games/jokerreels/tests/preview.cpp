// Renders real Joker Reels frames on the host, through the real engine, the
// real models and the real symbol textures, and writes them as PPM files.
// This is how the machine gets looked at and tuned without a device.
//
// It renders the whole 240x240 screen, not the console's 120x120, because
// this game is hires: the top 112 rows are the 3D window and everything under
// them is the 2D panel. What it cannot show is the text, which is SDK code and
// lives in game.cpp.
//
// Usage: jokerreels_preview [out_dir]

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "pse/pixel.hpp"

#include "render.hpp"
#include "sim.hpp"

namespace {

constexpr int k_w = jrr::k_screen_w;
constexpr int k_h = jrr::k_screen_h;

int g_failures = 0;

void check(bool ok, const char* what) {
    if (!ok) {
        g_failures++;
        std::printf("FAIL %s\n", what);
    }
}

std::vector<uint8_t>& buffer() {
    static std::vector<uint8_t> b(static_cast<size_t>(k_w) * k_h * 3);
    return b;
}

void write_ppm(const std::string& path) {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return;
    std::fprintf(f, "P6\n%d %d\n255\n", k_w, k_h);
    std::fwrite(buffer().data(), 1, static_cast<size_t>(k_w) * k_h * 3, f);
    std::fclose(f);
}

void draw(const jr::World& world) {
    // Marked, so the checks below can tell "drawn black" from "never touched".
    std::fill(buffer().begin(), buffer().end(), 0xAB);
    pse::RenderTarget target{buffer().data(), k_w, k_h, k_w * 3,
                             pse::PixelFormat::rgb888};
    if (world.state == jr::kShop) {
        jrr::render_shop(world, target);
    } else if (world.state == jr::kOver || world.state == jr::kWin) {
        jrr::render_end(world, target);
    } else {
        jrr::render_machine(world, target);
        jrr::render_panel(world, target);
    }
}

void capture(const jr::World& world, const std::string& out, const char* name) {
    draw(world);
    const std::string path = out + "/" + name + ".ppm";
    write_ppm(path);
    const jrr::Stats& s = jrr::stats();
    std::printf("wrote %s (%u tris, %u queued, %u dropped)\n",
                path.c_str(), s.triangles, s.queued, s.dropped);
    check(s.dropped == 0, "no triangle was dropped");
}

void play(jr::World& w, int ticks, int press_at = -1, bool press_a = true) {
    for (int i = 0; i < ticks; i++) {
        jr::Buttons b{};
        if (i == press_at) {
            b.a = press_a;
            b.any = true;
        }
        jr::world_tick(w, b);
    }
}

}  // namespace

int main(int argc, char** argv) {
    const std::string out = argc > 1 ? argv[1] : ".";

    jr::World w;
    jr::world_init(w, 20260811u);
    play(w, 200);
    capture(w, out, "preview_0_title");

    jr::world_init(w, 20260811u);
    play(w, 1, 0);                       // any button leaves the title
    capture(w, out, "preview_1_idle");

    // Mid spin at FAIR, which is what "there but too dark to read" looks like.
    play(w, 1, 0);                       // pull
    play(w, 40);
    capture(w, out, "preview_2_spin_fair");

    // The same spin at WILD, where the symbol is not drawn at all.
    {
        jr::World fast;
        jr::world_init(fast, 4242u);
        play(fast, 1, 0);
        fast.speed = jr::kWild;
        play(fast, 1, 0);
        play(fast, 40);
        capture(fast, out, "preview_3_spin_wild");
    }

    // Let a hands off spin run itself out and land, which is the frame that
    // shows three readable symbols on the payline.
    {
        jr::World landed;
        jr::world_init(landed, 7u);
        play(landed, 1, 0);
        play(landed, 1, 0);
        play(landed, 400);
        capture(landed, out, "preview_4_landed");
        check(landed.state == jr::kCount || landed.state == jr::kIdle ||
                  landed.state == jr::kCleared || landed.state == jr::kOver,
              "a hands off spin ends on its own");
    }

    // Opening a drum: the deckbuilding, and the one screen where the 3D is
    // doing work a 2D layout could not.
    {
        jr::World swap;
        jr::world_init(swap, 11u);
        play(swap, 1, 0);
        swap.state = jr::kSwap;
        swap.swap_drum = 1;
        swap.swap_face = 2;
        swap.swap_to = jr::kCrown;
        play(swap, 40);
        capture(swap, out, "preview_5_swap");
    }

    // The back room, where the run is actually built.
    {
        jr::World shop;
        jr::world_init(shop, 9u);
        play(shop, 1, 0);
        shop.gold = 14;
        jr::world_open_shop(shop);
        shop.shop_sel = 1;
        capture(shop, out, "preview_6_shop");
    }

    /* The window, checked on the pixels rather than asserted in a comment.
     *
     * The whole game is affordable because the 3D covers only the top band and
     * the depth buffer covers only that. If the renderer ever spilled past it,
     * the panel underneath would be scribbled on and the RAM claim would be a
     * lie. The panel is drawn after the machine, so this checks the machine
     * alone.
     */
    {
        jr::World probe;
        jr::world_init(probe, 3u);
        play(probe, 1, 0);
        std::fill(buffer().begin(), buffer().end(), 0xAB);
        pse::RenderTarget target{buffer().data(), k_w, k_h, k_w * 3,
                                 pse::PixelFormat::rgb888};
        jrr::render_machine(probe, target);

        size_t painted = 0, spilled = 0;
        for (int y = 0; y < k_h; y++) {
            for (int x = 0; x < k_w; x++) {
                const size_t i = (static_cast<size_t>(y) * k_w + x) * 3;
                const bool touched = buffer()[i] != 0xAB ||
                                     buffer()[i + 1] != 0xAB ||
                                     buffer()[i + 2] != 0xAB;
                if (!touched) continue;
                if (y < jrr::k_window_h) painted++;
                else spilled++;
            }
        }
        std::printf("window: %zu painted, %zu spilled below row %d\n",
                    painted, spilled, jrr::k_window_h);
        check(painted == static_cast<size_t>(k_w) * jrr::k_window_h,
              "the machine fills its window edge to edge");
        check(spilled == 0, "the machine draws nothing below its window");
    }

    // The bezel has to frame the drums rather than cover them or miss them.
    for (int d = 0; d < jr::k_drums; d++) {
        int left, right;
        jrr::drum_window(d, left, right);
        std::printf("drum %d window: x %d..%d (%d wide)\n", d, left, right,
                    right - left + 1);
        check(left >= 0 && right < k_w, "a drum window is on screen");
        check(right - left > 30, "a drum window is wide enough to read");
    }

    if (g_failures) {
        std::printf("\n%d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("jokerreels_preview: all checks pass\n");
    return 0;
}
