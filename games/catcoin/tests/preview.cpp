// Renders real Cat Coin Pusher frames on the host, through the real engine and
// the real sprites, and writes them as PPM files. This is how the game gets
// looked at without a device, and where the thumbnail comes from.
//
// It also checks the render side promises the sim cannot: that the panel rows
// do not overlap, that every string the panel can draw fits the box it is
// drawn in, and that no frame comes out effectively blank.
//
// Usage: catcoin_preview [out_dir]

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "pse/pixel.hpp"
#include "pse/text.hpp"

#include "render.hpp"
#include "sim.hpp"

namespace {

constexpr int k_w = cc::k_screen_w;
constexpr int k_h = cc::k_screen_h;

int g_failures = 0;

void fail(const char* what) {
    std::printf("FAIL: %s\n", what);
    g_failures++;
}

cc::Input none() {
    cc::Input in;
    in.drop_pressed = false;
    in.use_pressed = false;
    in.left_pressed = false;
    in.right_pressed = false;
    in.up_pressed = false;
    in.down_pressed = false;
    in.any_pressed = false;
    return in;
}

std::vector<uint8_t> render(const cc::World& world) {
    std::vector<uint8_t> buffer(static_cast<size_t>(k_w) * k_h * 3, 0);
    pse::RenderTarget target{buffer.data(), k_w, k_h, k_w * 3, pse::PixelFormat::rgb888};
    cc::render_world(world, target);
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
    int seen = 0;
    uint32_t first = 0xFFFFFFFFu;
    for (size_t i = 0; i + 2 < rgb.size(); i += 3) {
        const uint32_t c = (static_cast<uint32_t>(rgb[i]) << 16) |
                           (static_cast<uint32_t>(rgb[i + 1]) << 8) | rgb[i + 2];
        if (first == 0xFFFFFFFFu) first = c;
        else if (c != first) {
            seen++;
            if (seen > 400) break;
        }
    }
    return seen;
}

void capture(const cc::World& world, const std::string& out, const char* name) {
    const std::vector<uint8_t> rgb = render(world);
    if (distinct_colours(rgb) < 40) {
        std::printf("  %s: only %d distinct colours\n", name, distinct_colours(rgb));
        fail("a frame came out effectively blank");
    }
    if (!out.empty()) write_ppm(out + "/" + name + ".ppm", rgb);
}

cc::World begin(uint32_t seed) {
    cc::World world;
    cc::world_init(world, seed);
    cc::Input start = none();
    start.any_pressed = true;
    cc::world_tick(world, start);
    return world;
}

void play(cc::World& w, int ticks, int every) {
    for (int i = 0; i < ticks; i++) {
        cc::Input in = none();
        if (w.state == cc::State::spinner) in.any_pressed = true;
        else if (every > 0 && (i % every) == 0) in.drop_pressed = true;
        cc::world_tick(w, in);
    }
}

// The panel is a stack of rows with fixed tops. If two of them ever overlap,
// something prints through something else, and that is not visible in a test
// that only checks the sim.
void test_the_panel_rows_do_not_overlap() {
    struct Row {
        const char* name;
        int top, bottom;
    };
    const Row rows[] = {
        {"hud", 0, cc::k_hud_h},
        {"cat and rail", cc::k_hud_h, cc::k_ft - 1},
        {"field", cc::k_ft, cc::k_fbot},
        {"lip", cc::k_fbot + 1, cc::k_fbot + 6},
        {"tray", cc::k_fbot + 6, cc::k_tray_y + 22},
        {"progress", cc::k_bar_y, cc::k_bar_y + cc::k_bar_h},
        {"row", cc::k_row_y, cc::k_row_y + cc::k_row_h},
        {"description", cc::k_desc_y, cc::k_desc_y + pse::text_height(1)},
    };
    const int n = static_cast<int>(sizeof(rows) / sizeof(rows[0]));
    for (int i = 0; i + 1 < n; i++) {
        if (rows[i].bottom > rows[i + 1].top) {
            std::printf("  %s ends at %d, %s starts at %d\n", rows[i].name, rows[i].bottom,
                        rows[i + 1].name, rows[i + 1].top);
            fail("two panel rows overlap");
        }
    }
    if (rows[n - 1].bottom > k_h) fail("the panel runs off the bottom of the screen");
}

// Every string the panel can draw has to fit the box it is drawn in. A hand
// picked x is only correct for the string it was tuned against, which is
// exactly what rule 9 is about.
void test_every_panel_string_fits() {
    for (uint8_t stype = 1; stype <= cc::k_num_specials; stype++) {
        const char* name = cc::special_name(stype);
        const char* effect = cc::special_effect(stype);
        if (!pse::text_is_drawable(name) || !pse::text_is_drawable(effect)) {
            std::printf("  %s uses a glyph the font does not carry\n", name);
            fail("a special's text is not drawable");
        }
        const int width = 6 + pse::text_width(name) + 6 + pse::text_width(effect);
        if (width > k_w - 4) {
            std::printf("  %s %s is %d px, screen is %d\n", name, effect, width, k_w);
            fail("a special's description line runs off the screen");
        }
        // And the same line in the shop, which starts further in.
        if (36 + pse::text_width(effect) > k_w - 12) {
            std::printf("  shop line for %s is too wide\n", name);
            fail("a shop description runs into the price");
        }
    }

    const char* chips[4] = {"BUY 5", "G9999", "END", "ROUND"};
    const int widths[4] = {cc::k_buy_w, cc::k_buy_w, cc::k_end_w, cc::k_end_w};
    for (int i = 0; i < 4; i++) {
        if (pse::text_width(chips[i]) > widths[i] - 4) {
            std::printf("  %s is %d px in a %d px chip\n", chips[i],
                        pse::text_width(chips[i]), widths[i]);
            fail("a chip label does not fit its chip");
        }
    }

    // Every line the description can hold, from the renderer's own table, so
    // this cannot check two of three again. The one that was missed printed
    // through the right edge of the screen and the test said nothing.
    for (int i = 0; i < cc::k_description_count; i++) {
        const char* line = cc::description_line(i);
        if (cc::k_desc_x + pse::text_width(line) > k_w) {
            std::printf("  \"%s\" is %d px from x=%d, screen is %d\n", line,
                        pse::text_width(line), cc::k_desc_x, k_w);
            fail("a description line runs off the screen");
        }
        if (!pse::text_is_drawable(line)) fail("a description line is not drawable");
    }

    // The bag row plus both chips has to fit the screen width.
    const int row_right = cc::k_end_x + cc::k_end_w;
    if (row_right > k_w - 4) fail("the selectable row runs off the right of the screen");
    const int bag_right = 6 + cc::k_inv_max * (cc::k_slot_w + cc::k_slot_gap);
    if (bag_right > cc::k_buy_x) fail("the bag slots run into the BUY chip");
    if (cc::k_buy_x + cc::k_buy_w > cc::k_end_x) fail("the BUY chip runs into END ROUND");
}

}  // namespace

int main(int argc, char** argv) {
    const std::string out = argc > 1 ? argv[1] : std::string();

    test_the_panel_rows_do_not_overlap();
    test_every_panel_string_fits();

    {
        cc::World w;
        cc::world_init(w, 3);
        for (int i = 0; i < 260; i++) cc::world_tick(w, none());
        capture(w, out, "title");
    }
    {
        cc::World w = begin(7);
        play(w, 240, 40);
        capture(w, out, "early");
    }
    {
        // A busier table with a live combo, which is what the game looks like
        // when it is working.
        cc::World w = begin(31);
        for (int i = 0; i < 4000; i++) {
            cc::Input in = none();
            if (w.state == cc::State::spinner) in.any_pressed = true;
            else if ((i % 22) == 0) in.drop_pressed = true;
            cc::world_tick(w, in);
            if (w.state == cc::State::play && w.combo >= 3 && w.combo_timer > 60) break;
        }
        capture(w, out, "combo");
    }
    {
        // A special on its fuse, with the ring and the countdown dots.
        cc::World w = begin(12);
        w.inv_count = 1;
        w.inv[0] = cc::spc_bomb;
        w.sel = 0;
        cc::Input use = none();
        use.use_pressed = true;
        cc::world_tick(w, use);
        for (int i = 0; i < cc::k_drop_time + 40; i++) cc::world_tick(w, none());
        bool fused = false;
        for (uint16_t i = 0; i < w.coin_count; i++) {
            if (w.coins[i].stype == cc::spc_bomb && w.coins[i].fuse > 0) fused = true;
        }
        if (!fused) fail("the fuse frame had no coin on a fuse");
        capture(w, out, "fuse");
    }
    {
        // The reworked row, with a full bag and END ROUND reachable.
        cc::World w = begin(5);
        w.inv_count = 3;
        w.inv[0] = cc::spc_bomb;
        w.inv[1] = cc::spc_ice;
        w.inv[2] = cc::spc_crown;
        w.gold = 40;
        play(w, 120, 30);
        w.round_score = w.target + 20;
        cc::Slot slots[cc::k_inv_max + 2];
        const int n = cc::build_slots(w, slots, cc::k_inv_max + 2);
        w.sel = static_cast<uint8_t>(n - 1);
        capture(w, out, "row");
    }
    {
        cc::World w = begin(9);
        w.gold = 95;
        w.round = 3;
        w.round_score = w.target;
        cc::Slot slots[cc::k_inv_max + 2];
        const int n = cc::build_slots(w, slots, cc::k_inv_max + 2);
        w.sel = static_cast<uint8_t>(n - 1);
        cc::Input use = none();
        use.use_pressed = true;
        cc::world_tick(w, use);
        if (w.state != cc::State::shop) fail("the shop did not open");
        w.shop_sel = 1;
        cc::world_tick(w, none());
        capture(w, out, "shop");
    }
    {
        cc::World w = begin(17);
        w.coins_left = 60;
        // A burst of drops and then a pause, which is how a player actually
        // triggers this: a combo only ends once the lip goes quiet, so
        // dropping for ever never opens the spinner at all.
        for (int i = 0; i < 900 && w.state != cc::State::spinner; i++) {
            cc::Input in = none();
            if (i < 300 && (i % 10) == 0) in.drop_pressed = true;
            cc::world_tick(w, in);
        }
        if (w.state != cc::State::spinner) {
            fail("no combo ever opened the spinner");
        } else {
            for (int i = 0; i < 600 && !w.spin_done; i++) cc::world_tick(w, none());
            for (int i = 0; i < 10; i++) cc::world_tick(w, none());
            capture(w, out, "spinner");
        }
    }

    if (g_failures == 0) std::printf("catcoin_preview: ok\n");
    return g_failures == 0 ? 0 : 1;
}
