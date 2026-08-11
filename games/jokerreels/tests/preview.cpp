// Renders real Joker Reels frames on the host, through the real engine, the
// real models and the real symbol textures, and writes them as PPM files.
// This is how the machine gets looked at and tuned without a device.
//
// It renders the whole 240x240 screen, not the console's 120x120, because
// this game is hires: the top 112 rows are the 3D window and everything under
// them is the 2D panel. It renders the TEXT as well, because this game draws
// its HUD with pse::draw_text rather than the SDK's, so these frames are the
// whole game rather than the game with every number missing.
//
// Usage: jokerreels_preview [out_dir]

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "pse/pixel.hpp"
#include "pse/text.hpp"

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
    // The same call the game makes, so what this writes out is the frame the
    // device draws and not an approximation of it.
    jrr::render_frame(world, target);
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

/* Land a chosen hand by turning the DRUMS to it, not by writing landed[].
 *
 * Writing landed[] directly is one line shorter and it produces a frame that
 * lies: the markers point at reels showing different symbols, which is exactly
 * the "the drum shows a BAR and the panel says PLUM" desynchronisation the
 * geometry tests exist to prevent. Painting the front facet and then asking
 * the rules what landed goes through the same path a real spin does.
 */
void force_hand(jr::World& w, const uint8_t want[jr::k_drums]) {
    for (int d = 0; d < jr::k_drums; d++) {
        w.facet[d][jr::front_facet(w, d)] = want[d];
        w.landed[d] = jr::face_at(w, d, jr::front_facet(w, d));
    }
    w.hand_index = jr::hand_of(w.landed);
    w.group_count = jr::hand_groups(w.landed, w.group_of);
}

// Title, then the how to play pages, then the table. A press is a press: the
// opening is several screens now and a frame that wants the machine has to
// walk through them rather than assume one button gets there.
void to_table(jr::World& w) {
    for (int i = 0; i < 2 + jr::k_learn_pages && w.state != jr::kIdle; i++) {
        play(w, 1, 0);
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
    to_table(w);
    capture(w, out, "preview_1_idle");

    // Mid spin at FAIR, which is what "there but too dark to read" looks like.
    play(w, 1, 0);                       // pull
    play(w, 40);
    capture(w, out, "preview_2_spin_fair");

    // The same spin at WILD, where the symbol is not drawn at all.
    {
        jr::World fast;
        jr::world_init(fast, 4242u);
        to_table(fast);
        fast.speed = jr::kWild;
        play(fast, 1, 0);                  // pull
        play(fast, 40);
        capture(fast, out, "preview_3_spin_wild");
    }

    // Let a hands off spin run itself out and land, which is the frame that
    // shows three readable symbols on the payline.
    {
        jr::World landed;
        jr::world_init(landed, 7u);
        to_table(landed);
        play(landed, 1, 0);                // pull
        play(landed, 600);
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
        to_table(swap);
        swap.state = jr::kSwap;
        swap.swap_drum = 1;
        swap.swap_face = 2;
        swap.swap_to = jr::kCrown;
        play(swap, 40);
        capture(swap, out, "preview_5_swap");
    }

    // The three how to play pages, which are the answer to "how do I score".
    {
        for (int page = 0; page < jr::k_learn_pages; page++) {
            jr::World learn;
            jr::world_init(learn, 5u);
            play(learn, 1, 0);              // title -> learn
            learn.hand_level[jr::kPair] = 3;   // so a levelled row is drawn
            learn.learn_page = static_cast<uint8_t>(page);
            char name[24];
            std::snprintf(name, sizeof(name), "preview_8_learn%d", page);
            capture(learn, out, name);
        }
    }

    /* A hand with lines through it, which is the whole point of five reels.
     *
     * Forced rather than waited for: a two pair does not turn up on demand,
     * and a frame that shows the feature has to exist for the feature to have
     * been looked at. The landed symbols are set and the scorer is asked for
     * the groups exactly as a real spin would.
     */
    {
        jr::World two_pair;
        jr::world_init(two_pair, 77u);
        to_table(two_pair);
        play(two_pair, 1, 0);              // pull
        play(two_pair, 600);               // let it land and count
        const uint8_t forced[jr::k_drums] = {
            jr::kCherry, jr::kSeven, jr::kCherry, jr::kCrown, jr::kSeven,
        };
        force_hand(two_pair, forced);
        check(two_pair.hand_index == jr::kTwoPair, "that is a two pair");
        check(two_pair.group_count == 2, "and it is two groups");
        for (int d = 0; d < jr::k_drums; d++) {
            check(two_pair.landed[d] == forced[d],
                  "the drum is showing the symbol the line points at");
        }
        capture(two_pair, out, "preview_9_twopair");
    }

    /* A marker sits on a reel that made the group, and the reels in a group
     * show the same symbol.
     *
     * This is the render half of the desync check: the sim tests prove
     * hand_groups agrees with the landed symbols, and this proves the frame
     * the player sees is drawn from the same landed symbols. Run over every
     * shape rather than the one that happened to come up.
     */
    {
        const uint8_t shapes[][jr::k_drums] = {
            {jr::kBell, jr::kBell, jr::kBell, jr::kBell, jr::kBell},
            {jr::kBell, jr::kBell, jr::kCrown, jr::kBell, jr::kBell},
            {jr::kBell, jr::kCrown, jr::kBell, jr::kCrown, jr::kBell},
            {jr::kCherry, jr::kBell, jr::kPlum, jr::kBar, jr::kClover},
            {jr::kBell, jr::kBell, jr::kCrown, jr::kCrown, jr::kCherry},
            {jr::kCherry, jr::kPlum, jr::kClover, jr::kDiamond, jr::kCrown},
        };
        for (const auto& shape : shapes) {
            jr::World probe;
            jr::world_init(probe, 61u);
            to_table(probe);
            force_hand(probe, shape);
            for (int d = 0; d < jr::k_drums; d++) {
                check(probe.landed[d] == shape[d],
                      "the drum shows what the rules scored");
                if (probe.group_of[d] == jr::k_no_group) continue;
                if (probe.hand_index == jr::kRun) continue;
                for (int e = 0; e < jr::k_drums; e++) {
                    if (e == d || probe.group_of[e] != probe.group_of[d]) continue;
                    check(probe.landed[e] == probe.landed[d],
                          "a line joins reels showing the same symbol");
                }
            }
        }
    }

    // The back room, where the run is actually built.
    {
        jr::World shop;
        jr::world_init(shop, 9u);
        to_table(shop);
        shop.gold = 14;
        jr::world_open_shop(shop);
        shop.shop_sel = 1;
        capture(shop, out, "preview_6_shop");
    }

    /* A run part way in: jokers held, a hand counting, gold spent.
     *
     * Every other frame here starts a fresh run, so every other frame has an
     * empty joker row and a zero score. That is what the first ante looks like
     * and it is not what the game looks like, and it meant the row that draws
     * a held joker had never been looked at at all: the six character
     * truncation, the filled slot, and the tally line under it were all
     * unrendered until this frame existed.
     */
    {
        jr::World mid;
        jr::world_init(mid, 31u);
        to_table(mid);
        mid.ante = 3;
        mid.target = jr::target_for_ante(3);
        mid.banked = mid.target * 2 / 3;
        mid.gold = 11;
        mid.spins = 2;
        mid.joker_count = 3;
        mid.jokers[0] = jr::kTwin;
        mid.jokers[1] = jr::kUnderstudy;   // the longest name there is
        mid.jokers[2] = jr::kCollector;
        play(mid, 1, 0);                   // pull
        play(mid, 400);                    // let it land and start counting
        capture(mid, out, "preview_7_midrun");
        check(mid.joker_count == 3, "the jokers survived the spin");
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
        to_table(probe);
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

    /* Rule 9's other half: measure text, never place it by eye.
     *
     * This walks every string the game can draw and fails if one would not fit
     * the box it is drawn in. Built from the joker, hand, symbol and speed
     * tables rather than a list somebody typed, because a list by hand is how
     * the catcoin build shipped exactly this bug: it checked two of three
     * strings and the third printed through the edge of the screen.
     *
     * It only became possible when the text moved out of game.cpp. Nothing can
     * measure a string the host cannot draw.
     */
    {
        int measured = 0;
        auto fits = [&](const char* text, int x, int limit, const char* where) {
            measured++;
            const int w = pse::text_width(text);
            if (x + w <= limit) return;
            std::printf("FAIL %s: \"%s\" is %d px from x=%d, limit %d\n",
                        where, text, w, x, limit);
            g_failures++;
        };
        auto fits_centred = [&](const char* text, int centre, int left,
                                int right, const char* where) {
            measured++;
            const int w = pse::text_width(text);
            if (centre - w / 2 >= left && centre + w / 2 <= right) return;
            std::printf("FAIL %s: \"%s\" is %d px centred at %d, box %d..%d\n",
                        where, text, w, centre, left, right);
            g_failures++;
        };

        for (int j = 0; j < jr::k_jokers; j++) {
            const uint8_t which = static_cast<uint8_t>(j);
            fits(jr::joker_name(which), 12, 200, "shop joker name");
            fits(jr::joker_text(which), 12, 228, "shop joker text");
            fits(jrr::joker_slot_name(which), 0, 39, "joker slot");
        }
        for (int h = 0; h < jr::k_hands; h++) {
            const uint8_t which = static_cast<uint8_t>(h);
            fits(jr::hand_name(which), 12, 200, "shop hand name");
            fits_centred(jr::hand_name(which), k_w / 2, 0, k_w, "hand banner");
            fits(jr::hand_name(which), 4, 130, "tally hand name");
        }
        for (int sym = 0; sym < jr::k_symbols; sym++) {
            const uint8_t which = static_cast<uint8_t>(sym);
            fits(jr::symbol_name(which), 6, 104, "swap NOW name");
            fits(jr::symbol_name(which),
                 234 - pse::text_width(jr::symbol_name(which)), 234,
                 "swap NEW name");
            fits(jr::symbol_name(which), 4, 130, "tally symbol name");
        }
        for (int sp = 0; sp < jr::k_speeds; sp++) {
            fits_centred(jr::speed_name(static_cast<uint8_t>(sp)), 18, 0, 36,
                         "speed dial");
        }
        fits("A DRUM LANDS ONLY ON WHAT IS ON IT", 6, k_w, "swap hint");
        fits("SWAP ONE FACE FOR ANY SYMBOL", 12, 228, "swap card");
        fits("LEVEL IT UP", 12, 228, "hand card");
        fits("DRUM 3  SYMBOL 24/24", 6, k_w, "swap counter");
        fits("ANTE 8/8", 4, 100, "ante");
        fits("SPINS 5", 236 - pse::text_width("SPINS 5"), 236, "spins");
        fits_centred("YOU BROKE THE BANK", k_w / 2, 0, k_w, "win banner");
        fits_centred("EIGHT ANTES, FIVE SPINS EACH", k_w / 2, 0, k_w, "title");
        fits_centred("A DRUM LANDS ON WHAT YOU PUT ON IT", k_w / 2, 0, k_w,
                     "title 2");
        fits_centred("THE BACK ROOM", k_w / 2, 0, k_w, "shop heading");
        fits_centred("NEXT ANTE", 120, 70, 170, "next ante");
        std::printf("text: %d strings measured against their boxes\n", measured);
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
