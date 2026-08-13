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
void force_grid(jr::World& w, const uint8_t want[jr::k_drums][jr::k_rows]) {
    for (int d = 0; d < jr::k_drums; d++) {
        for (int r = 0; r < jr::k_rows; r++) {
            w.facet[d][jr::facet_in_row(w, d, r)] = want[d][r];
        }
    }
    // Read it back the way the game does, off the drums, so what is checked
    // below is what the drums are actually showing.
    for (int d = 0; d < jr::k_drums; d++) {
        for (int r = 0; r < jr::k_rows; r++) {
            w.grid[d][r] = jr::face_at(w, d, jr::facet_in_row(w, d, r));
        }
        w.landed[d] = w.grid[d][1];
    }
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
            char name[32];
            std::snprintf(name, sizeof(name), "preview_8_learn%d", page);
            capture(learn, out, name);
        }
    }

    /* A payline being paid, which is the whole point of three rows.
     *
     * Forced rather than waited for: a two pair on the V does not turn up on
     * demand, and a frame that shows the feature has to exist for the feature
     * to have been looked at. The GRID is painted onto the drums and read back
     * off them, so the line the screen draws crosses the symbols the rules
     * scored. Writing landed[] directly instead produced a frame whose markers
     * pointed at reels showing something else, which is the same
     * desynchronisation the geometry tests exist to prevent.
     */
    {
        jr::World lines;
        jr::world_init(lines, 77u);
        to_table(lines);
        // Top row a pair of sevens, the payline row three cherries, the V a
        // run: three lines paying at once, drawn one at a time.
        const uint8_t grid[jr::k_drums][jr::k_rows] = {
            {jr::kSeven,  jr::kCherry, jr::kBell},
            {jr::kSeven,  jr::kCherry, jr::kPlum},
            {jr::kCrown,  jr::kCherry, jr::kBar},
            {jr::kBar,    jr::kCherry, jr::kClover},
            {jr::kDiamond, jr::kCherry, jr::kCrown},
        };
        play(lines, 1, 0);                 // pull
        play(lines, 700);                  // let it land, and count it out

        /* Paint the grid onto the resting drums, then land them AGAIN.
         *
         * Painting after the spin has been scored changes what is drawn and
         * not what was counted, which is a frame that lies. Putting the drums
         * back into a spin whose auto stop has already passed makes the very
         * next tick snap every reel and score it, through the same path a real
         * spin takes, with the angles already where they were.
         */
        force_grid(lines, grid);
        lines.state = jr::kSpin;
        lines.spin_ticks = 9999;
        for (int d = 0; d < jr::k_drums; d++) {
            lines.spinning[d] = true;
            lines.stopped_at[d] = -1;
        }
        play(lines, 1);
        check(lines.state == jr::kCount, "the forced grid was scored");
        check(lines.grid[0][1] == jr::kCherry, "and the grid took");

        // Step to the frame where a payline is being paid, which is the one
        // worth photographing.
        for (int guard = 0; guard < 60 && lines.state == jr::kCount; guard++) {
            if (lines.tally_step > 0 &&
                lines.tally[lines.tally_step - 1].line != jr::k_no_line) {
                break;
            }
            play(lines, 1);
        }
        check(lines.state == jr::kCount, "still counting when photographed");
        check(lines.tally[lines.tally_step - 1].line != jr::k_no_line,
              "and paying a line, so there is one to draw");
        capture(lines, out, "preview_9_payline");

        // And on to a DIAGONAL, which is the line a straight row cannot show
        // and the one most likely to be drawn wrong.
        for (int guard = 0; guard < 200 && lines.state == jr::kCount; guard++) {
            play(lines, 1);
            if (lines.tally_step == 0) continue;
            const uint8_t line = lines.tally[lines.tally_step - 1].line;
            if (line == jr::kVee || line == jr::kCaret) break;
        }
        if (lines.state == jr::kCount && lines.tally_step > 0 &&
            (lines.tally[lines.tally_step - 1].line == jr::kVee ||
             lines.tally[lines.tally_step - 1].line == jr::kCaret)) {
            capture(lines, out, "preview_10_diagonal");
        } else {
            std::printf("FAIL no diagonal line was paid, so none was drawn\n");
            g_failures++;
        }
    }

    /* A payline crosses the cells the rules scored.
     *
     * The render half of the desync check: the sim tests prove hand_of and
     * hand_groups agree with the symbols on a line, and this proves the line
     * the SCREEN draws reads those same cells off the same drums. Run over
     * every line rather than the one that happened to come up.
     */
    {
        jr::World probe;
        jr::world_init(probe, 61u);
        to_table(probe);
        const uint8_t grid[jr::k_drums][jr::k_rows] = {
            {jr::kBell, jr::kCherry, jr::kCrown},
            {jr::kBell, jr::kCherry, jr::kCrown},
            {jr::kBell, jr::kCherry, jr::kCrown},
            {jr::kBell, jr::kCherry, jr::kCrown},
            {jr::kBell, jr::kCherry, jr::kCrown},
        };
        force_grid(probe, grid);
        for (uint8_t line = 0; line < jr::k_lines; line++) {
            uint8_t symbols[jr::k_drums];
            jr::line_symbols(probe, line, symbols);
            const uint8_t* rows = jr::payline_rows(line);
            for (int d = 0; d < jr::k_drums; d++) {
                check(symbols[d] == grid[d][rows[d]],
                      "a payline reads the cell it crosses");
                int cy, h, left, right;
                jrr::row_band(rows[d], cy, h);
                jrr::drum_window(d, left, right);
                check(cy >= 0 && cy < jrr::k_window_h,
                      "and that cell is on screen");
                check(h > 12, "and big enough to read");
            }
            // Each ROW is one symbol repeated, so the three straight lines are
            // five of a kind. The diagonals cross rows and are not, which is
            // the point of having them: a grid where every line scores the
            // same is a grid with one line.
            const bool straight = line == jr::kMiddle || line == jr::kTop ||
                                  line == jr::kBottom;
            if (straight) {
                check(jr::hand_of(symbols) == jr::kFive,
                      "a straight line is five of one symbol here");
            } else {
                check(jr::hand_of(symbols) != jr::kFive,
                      "a diagonal crosses rows, so it is not");
            }
        }
        // The three rows have to be in the order the paylines assume, or the V
        // and the caret are each other.
        int top_y, mid_y, bot_y, h;
        jrr::row_band(0, top_y, h);
        jrr::row_band(1, mid_y, h);
        jrr::row_band(2, bot_y, h);
        check(top_y < mid_y && mid_y < bot_y,
              "row 0 is above row 1 is above row 2");
        check(mid_y == jrr::k_window_h / 2, "and row 1 is the payline");
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

        /* And on to the frame where one of them FIRES.
         *
         * COLLECTOR is in the row on purpose: it pays per different symbol on
         * the grid, so it fires on every spin there has ever been, and a frame
         * that shows the shake and the pop can be captured without waiting for
         * a hand that may not come. Stopped part way through the hold rather
         * than at its start, so the pop has actually risen off the score box
         * and the shake is somewhere other than zero.
         */
        bool fired = false;
        for (int guard = 0; guard < 1200 && mid.state == jr::kCount; guard++) {
            play(mid, 1);
            if (mid.tally_step == 0) continue;
            const jr::TallyEntry& e = mid.tally[mid.tally_step - 1];
            if (e.joker && e.slot < jr::k_max_jokers && mid.count_wait >= 12) {
                fired = true;
                break;
            }
        }
        check(fired, "a joker fired, so there is a shake to photograph");
        if (fired) {
            const jr::TallyEntry& e = mid.tally[mid.tally_step - 1];
            check(mid.jokers[e.slot] == jr::kTwin ||
                      mid.jokers[e.slot] == jr::kUnderstudy ||
                      mid.jokers[e.slot] == jr::kCollector,
                  "and the slot it names is one this run actually holds");
            check(jr::tally_hold(mid) > 22,
                  "and the count holds longer on it than on a payline");
            std::printf("joker fired: slot %d, %+d chips %+d mult, "
                        "tick %u of %d\n",
                        e.slot, e.chips, e.mult, mid.count_wait,
                        jr::tally_hold(mid));
            capture(mid, out, "preview_11_joker");
        }
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
            fits(jr::joker_name(which), jrr::k_shop_text_x, 200,
                 "shop joker name");
            fits(jr::joker_text(which), jrr::k_shop_text_x, 228,
                 "shop joker text");
            // The instructions page puts both on one row, past the icon.
            fits(jr::joker_name(which), 32, 234, "joker page name");
            fits(jr::joker_text(which), 32, 234, "joker page text");
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
        /* Every kind of shop card names itself, and no two kinds say the same
         * thing.
         *
         * Measured off shop_title and shop_body rather than off literals here,
         * so this reads what the screen draws. The hand card lost its title
         * once to a refactor that added an icon to the joker branch and took
         * the branch underneath with it, and the resulting shop looked
         * perfectly normal: four cards, four prices, two of them offering to
         * open a drum.
         */
        const char* seen[3] = {nullptr, nullptr, nullptr};
        for (int kind = 0; kind < 3; kind++) {
            jr::ShopItem item{static_cast<uint8_t>(kind),
                              static_cast<uint8_t>(kind == jr::kShopHand
                                                       ? jr::kTwoPair : 0),
                              5, false};
            const char* title = jrr::shop_title(item);
            const char* body = jrr::shop_body(item);
            fits(title, jrr::k_shop_text_x, 200, "shop card title");
            fits(body, jrr::k_shop_text_x, 228, "shop card body");
            for (int other = 0; other < kind; other++) {
                check(std::strcmp(seen[other], title) != 0,
                      "two kinds of shop card say the same thing");
            }
            seen[kind] = title;
        }
        // The instructions are the longest lines in the game and they are the
        // ones nobody would notice overflowing, because a page of prose that
        // loses its last two characters still reads.
        fits("FIVE REELS STOP, THREE ROWS DEEP.", 8, k_w, "learn 1");
        fits("EVERY LINE THAT MAKES A HAND PAYS.", 8, k_w, "learn 2");
        fits("OPENING A DRUM CHANGES ITS SYMBOLS.", 8, k_w, "learn 3");
        fits("THE ONE FIRING SHAKES, AND ITS", 8, k_w, "learn 4");
        fits("+5 MULT A REEL YOU STOP", 58, k_w, "speed row");
        fits("READABLE, AND WORTH NOTHING", 58, k_w, "speed row 2");
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

    /* The joker row: five icons, on screen, and not on top of each other.
     *
     * The same measure-it rule the strings get, applied to a picture. A slot
     * that grew, an icon that grew, or a sixth joker would all put art off the
     * right edge of a 240 wide panel, and every one of those still compiles.
     */
    {
        int prev_right = -1;
        for (int i = 0; i < jr::k_max_jokers; i++) {
            int x, y;
            jrr::joker_slot(i, x, y);
            // Room for the shake, which moves a slot up to three pixels.
            check(x - 3 >= 0 && x + jrr::k_slot_w + 3 <= k_w,
                  "a joker slot is on screen, shake included");
            check(y + jrr::k_slot_h <= k_h, "and inside the panel");
            check(jrr::k_joker_icon <= jrr::k_slot_w - 4 &&
                      jrr::k_joker_icon <= jrr::k_slot_h - 4,
                  "and its icon fits it");
            check(x > prev_right, "and the slots do not overlap");
            prev_right = x + jrr::k_slot_w - 1;
        }
        std::printf("joker row: %d slots of %d, last ends at %d\n",
                    jr::k_max_jokers, jrr::k_slot_w, prev_right);
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
