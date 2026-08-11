#include "render.hpp"

#include "pse/draw2d.hpp"
#include "pse/text.hpp"

#include "catcoin/cat.hpp"
#include "catcoin/coins.hpp"
#include "catcoin/plate.hpp"

namespace cc {
namespace {

struct Rgb {
    uint8_t r, g, b;
};

// PICO-8's palette, which is what the cart was drawn in.
constexpr Rgb k_black{0x00, 0x00, 0x00};
constexpr Rgb k_navy{0x1D, 0x2B, 0x53};
constexpr Rgb k_plum{0x7E, 0x25, 0x53};
constexpr Rgb k_felt{0x00, 0x87, 0x51};
constexpr Rgb k_brown{0xAB, 0x52, 0x36};
constexpr Rgb k_grey{0x5F, 0x57, 0x4F};
constexpr Rgb k_silver{0xC2, 0xC3, 0xC7};
constexpr Rgb k_white{0xFF, 0xF1, 0xE8};
constexpr Rgb k_red{0xFF, 0x00, 0x4D};
constexpr Rgb k_orange{0xFF, 0xA3, 0x00};
constexpr Rgb k_yellow{0xFF, 0xEC, 0x27};
constexpr Rgb k_green{0x00, 0xE4, 0x36};
constexpr Rgb k_blue{0x29, 0xAD, 0xFF};
constexpr Rgb k_lavender{0x83, 0x76, 0x9C};
constexpr Rgb k_pink{0xFF, 0x77, 0xA8};

const Rgb k_special_colour[k_num_specials] = {
    k_red, k_blue, k_silver, k_blue, k_yellow, k_orange,
    k_brown, k_blue, k_green, k_yellow, k_pink
};

const char* const k_names[k_num_specials] = {
    "BOMB", "WHIRL", "B.HOLE", "MAGNET", "GOLD", "MULTI",
    "QUAKE", "ICE", "CLONE", "CROWN", "CASH"
};
const char* const k_effects[k_num_specials] = {
    "BLAST COINS TO EDGE", "SPIN COINS TO CENTER", "SUCK THEN RELEASE",
    "PULL COINS TOGETHER", "WORTH 5X WHEN SCORED", "2X SCORE 6 SECONDS",
    "SHAKE ALL COINS", "FREEZE PUSHER 5 SEC", "SPAWN 5 EXTRA COINS",
    "3X COMBO 8 SECONDS", "SCORE NEARBY COINS"
};

// The three strings the description line can hold. Named, so the preview
// harness can check every one of them fits rather than the two somebody
// happened to list: the longest was 239 px against a 234 px line and printed
// through the right edge of the screen.
const char* const k_buy_description = "FIVE MORE COINS. SETS OFF EVERY FUSE";
const char* const k_end_ready = "TARGET MET. TAKE THE SHOP.";
const char* const k_end_short = "SHORT OF TARGET. THIS ENDS THE RUN.";

constexpr int k_coin_cell = 11;
constexpr int k_cat_w = 33;
constexpr int k_cat_h = 30;

void number_string(int32_t value, char* out, int cap) {
    int n = 0;
    if (value <= 0) {
        if (cap > 1) out[n++] = '0';
    } else {
        char digits[12];
        int d = 0;
        while (value > 0 && d < 11) {
            digits[d++] = static_cast<char>('0' + value % 10);
            value /= 10;
        }
        while (d > 0 && n < cap - 1) out[n++] = digits[--d];
    }
    out[n] = '\0';
}

void append(char* out, int cap, const char* text) {
    int n = 0;
    while (out[n] != '\0' && n < cap - 1) n++;
    for (int i = 0; text[i] != '\0' && n < cap - 1; i++) out[n++] = text[i];
    out[n] = '\0';
}

void label_number(char* out, int cap, const char* prefix, int32_t value) {
    out[0] = '\0';
    append(out, cap, prefix);
    char digits[14];
    number_string(value, digits, sizeof(digits));
    append(out, cap, digits);
}

void text_at(const pse::RenderTarget& t, const char* s, int x, int y, Rgb c, int scale = 1) {
    pse::draw_text(t, s, x, y, c.r, c.g, c.b, scale);
}

void text_right(const pse::RenderTarget& t, const char* s, int right_x, int y, Rgb c) {
    pse::draw_text(t, s, right_x - pse::text_width(s), y, c.r, c.g, c.b);
}

void text_centred(const pse::RenderTarget& t, const char* s, int cx, int y, Rgb c,
                  int scale = 1) {
    pse::draw_text_centred(t, s, cx, y, c.r, c.g, c.b, scale);
}

void text_shadowed(const pse::RenderTarget& t, const char* s, int cx, int y, Rgb c,
                   int scale) {
    pse::draw_text_centred(t, s, cx + scale, y + scale, 0, 0, 0, scale);
    pse::draw_text_centred(t, s, cx, y, c.r, c.g, c.b, scale);
}

void draw_coin(const pse::RenderTarget& t, int32_t x, int32_t y, uint8_t stype) {
    pse::blit_sprite(t, models::catcoin::coins, stype * k_coin_cell, 0, k_coin_cell,
                     k_coin_cell, (x >> k_fp) - k_coin_r, (y >> k_fp) - k_coin_r);
}

void draw_cat(const World& w, const pse::RenderTarget& t, int x, int base) {
    // Three frames in the sheet: open eyed, blinking, ears up. The cart
    // animated exactly these, and they are all it needs to feel alive.
    int frame = 0;
    if (w.cat_blink > 8) frame = 1;
    else if (w.cat_twitch > 0) frame = 2;
    pse::blit_sprite(t, models::catcoin::cat, frame * k_cat_w, 0, k_cat_w, k_cat_h,
                     x - k_cat_w / 2, base - k_cat_h, w.disp_dir < 0);
    if (w.coins_left > 0) {
        draw_coin(t, x * k_one, (base - 3) * k_one, spc_none);
    }
}

void draw_field(const World& w, const pse::RenderTarget& t) {
    pse::fill_rect(t, k_fl - 9, k_ft - 6, k_fw + 18, k_fh + 16, k_grey.r, k_grey.g, k_grey.b);
    pse::fill_rect(t, k_fl - 5, k_ft - 3, k_fw + 10, k_fh + 10, k_lavender.r, k_lavender.g,
                   k_lavender.b);
    pse::fill_rect(t, k_fl, k_ft, k_fw + 1, k_fh + 1, k_felt.r, k_felt.g, k_felt.b);
    // Felt speckle, computed from the index rather than reseeded per frame:
    // the cart called rnd inside _draw, so its table fizzed. Sparse, too, or
    // 245 bright dots compete with the coins.
    for (int i = 0; i < 44; i++) {
        pse::plot_pixel(t, k_fl + 3 + (i * 47) % (k_fw - 6), k_ft + 3 + (i * 71) % (k_fh - 6),
                        k_green.r, k_green.g, k_green.b);
    }
    pse::fill_rect(t, k_fl, k_ft - 3, k_fw + 1, 3, k_brown.r, k_brown.g, k_brown.b);
    pse::h_line(t, k_fl, k_fr, k_ft - 1, k_orange.r, k_orange.g, k_orange.b);
    pse::v_line(t, k_fl - 1, k_ft, k_fbot, k_brown.r, k_brown.g, k_brown.b);
    pse::v_line(t, k_fr + 1, k_ft, k_fbot, k_brown.r, k_brown.g, k_brown.b);
}

void draw_pusher(const World& w, const pse::RenderTarget& t) {
    const int top = (w.push_y >> k_fp) - k_push_h;
    pse::fill_rect(t, k_fl + 3, top + 3, k_fw - 6, k_push_h, k_navy.r, k_navy.g, k_navy.b);
    // The plate is a fixed size object, so it is one picture rather than a
    // dozen draw calls that have to agree with each other. Two frames: the
    // second is the frozen one the ice special leaves behind.
    pse::blit_sprite(t, models::catcoin::plate, w.push_frozen > 0 ? k_fw : 0, 0, k_fw,
                     k_push_h, k_fl, top);
}

void draw_lip(const World& w, const pse::RenderTarget& t) {
    // The payout edge, pulsing so the place where coins turn into score is the
    // brightest thing on the cabinet.
    static const Rgb cycle[4] = {k_red, k_plum, k_red, k_pink};
    const Rgb c = cycle[(w.ticks / 17) % 4];
    pse::fill_rect(t, k_fl, k_fbot + 1, k_fw + 1, 5, c.r, c.g, c.b);
    pse::h_line(t, k_fl, k_fr, k_fbot + 1, k_yellow.r, k_yellow.g, k_yellow.b);
}

void draw_tray(const World& w, const pse::RenderTarget& t) {
    // The well under the lip. Without it the gap between the field and the
    // progress bar is black nothing, and a coin that scores falls out of the
    // world instead of into a tray.
    pse::fill_rect(t, k_fl - 9, k_fbot + 6, k_fw + 18, k_tray_y + 22 - (k_fbot + 6),
                   k_grey.r, k_grey.g, k_grey.b);
    pse::fill_rect(t, k_fl - 5, k_fbot + 8, k_fw + 10, k_tray_y + 20 - (k_fbot + 8),
                   k_navy.r, k_navy.g, k_navy.b);
    pse::h_line(t, k_fl - 5, k_fr + 4, k_fbot + 8, k_black.r, k_black.g, k_black.b);
    for (int i = 0; i < 30; i++) {
        const int x = k_fl + 4 + (i * 67) % (k_fw - 10);
        pse::h_line(t, x, x + 2, k_tray_y + 18, k_orange.r, k_orange.g, k_orange.b);
        pse::plot_pixel(t, x + 1, k_tray_y + 17, k_yellow.r, k_yellow.g, k_yellow.b);
    }

    for (int i = 0; i < w.falling_count; i++) {
        const Falling& f = w.falling[i];
        draw_coin(t, f.x, f.y - f.h, spc_none);
    }
    for (int i = 0; i < w.popup_count; i++) {
        const Popup& p = w.popups[i];
        char line[16];
        line[0] = '+';
        line[1] = '\0';
        char digits[14];
        number_string(p.value, digits, sizeof(digits));
        append(line, sizeof(line), digits);
        const Rgb c = p.value >= 80 ? k_green : (p.value >= 30 ? k_yellow : k_white);
        const int x = (p.x >> k_fp) - pse::text_width(line) / 2;
        text_at(t, line, x + 1, (p.y >> k_fp) + 1, k_black);
        text_at(t, line, x, p.y >> k_fp, c);
    }

    // The combo, where the combo happens rather than in a corner.
    if (w.combo > 1 && w.combo_timer > 0) {
        static const Rgb ladder[5] = {k_white, k_yellow, k_orange, k_red, k_green};
        const Rgb c = ladder[(w.combo > 5 ? 5 : w.combo) - 1];
        char line[10];
        label_number(line, sizeof(line), "X", w.combo);
        if (w.combo >= k_combo_threshold) append(line, sizeof(line), "!");
        constexpr int k_meter_w = 90;
        const int mx = (k_screen_w - k_meter_w) / 2;
        pse::fill_rect(t, mx, k_tray_y + 9, k_meter_w, 3, k_grey.r, k_grey.g, k_grey.b);
        int filled = k_meter_w * w.combo / k_combo_threshold;
        if (filled > k_meter_w) filled = k_meter_w;
        const Rgb fc = w.combo >= k_combo_threshold ? k_green : k_red;
        pse::fill_rect(t, mx, k_tray_y + 9, filled, 3, fc.r, fc.g, fc.b);
        text_centred(t, line, k_screen_w / 2, k_tray_y, c);
    }
}

void draw_hud(const World& w, const pse::RenderTarget& t) {
    pse::fill_rect(t, 0, 0, k_screen_w, k_hud_h, k_black.r, k_black.g, k_black.b);
    char line[16];
    label_number(line, sizeof(line), "R", w.round);
    text_at(t, line, 3, 2, k_white);

    int x = 26;
    if (w.mult_timer > 0) {
        text_at(t, "2X", x, 2, k_orange);
        x += pse::text_width("2X ") + 3;
    }
    if (w.combo_buff_timer > 0) {
        text_at(t, "3C", x, 2, k_yellow);
        x += pse::text_width("3C ") + 3;
    }
    if (w.push_frozen > 0) text_at(t, "ICE", x, 2, k_blue);

    char gold[16];
    label_number(gold, sizeof(gold), "G", w.gold);
    text_right(t, gold, k_screen_w - 3, 2, k_yellow);
    char coins[16];
    label_number(coins, sizeof(coins), "C", w.coins_left);
    text_right(t, coins, k_screen_w - 6 - pse::text_width(gold), 2, k_blue);
    pse::h_line(t, 0, k_screen_w - 1, k_hud_h - 1, k_grey.r, k_grey.g, k_grey.b);
}

void draw_progress(const World& w, const pse::RenderTarget& t) {
    const int x0 = 4;
    const int width = k_screen_w - 9;
    pse::fill_rect(t, x0, k_bar_y, width, k_bar_h, k_navy.r, k_navy.g, k_navy.b);
    pse::draw_rect(t, x0, k_bar_y, width, k_bar_h, k_grey.r, k_grey.g, k_grey.b);
    int filled = w.target > 0
                     ? static_cast<int>((static_cast<int64_t>(w.round_score) * (width - 2)) /
                                        w.target)
                     : 0;
    if (filled > width - 2) filled = width - 2;
    if (filled > 0) {
        const Rgb c = w.round_score >= w.target ? k_green : k_red;
        pse::fill_rect(t, x0 + 1, k_bar_y + 1, filled, k_bar_h - 2, c.r, c.g, c.b);
    }
    // The score is drawn here and nowhere else. The cart printed it twice
    // because at 128 neither copy was quite readable.
    char line[24];
    number_string(w.round_score, line, sizeof(line));
    append(line, sizeof(line), "/");
    char goal[14];
    number_string(w.target, goal, sizeof(goal));
    append(line, sizeof(line), goal);
    const int x = (k_screen_w - pse::text_width(line)) / 2;
    text_at(t, line, x + 1, k_bar_y + 4, k_black);
    text_at(t, line, x, k_bar_y + 3, k_white);
}

void draw_row(const World& w, const pse::RenderTarget& t) {
    Slot slots[k_inv_max + 2];
    const int count = build_slots(w, slots, k_inv_max + 2);
    const int sel_index = w.sel < count ? w.sel : (count > 0 ? count - 1 : 0);
    const Slot* sel = count > 0 ? &slots[sel_index] : nullptr;

    for (int i = 0; i < k_inv_max; i++) {
        const int x = 6 + i * (k_slot_w + k_slot_gap);
        const bool chosen = sel != nullptr && sel->kind == SlotKind::item && sel->index == i;
        pse::fill_rect(t, x, k_row_y, k_slot_w, k_row_h, chosen ? k_navy.r : k_black.r,
                       chosen ? k_navy.g : k_black.g, chosen ? k_navy.b : k_black.b);
        const Rgb edge = chosen ? k_white : k_grey;
        pse::draw_rect(t, x, k_row_y, k_slot_w, k_row_h, edge.r, edge.g, edge.b);
        if (i < w.inv_count) {
            draw_coin(t, (x + k_slot_w / 2) * k_one, (k_row_y + k_row_h / 2) * k_one, w.inv[i]);
        } else {
            pse::h_line(t, x + k_slot_w / 2 - 2, x + k_slot_w / 2 + 2, k_row_y + k_row_h / 2,
                        k_grey.r, k_grey.g, k_grey.b);
        }
    }

    const bool buy_chosen = sel != nullptr && sel->kind == SlotKind::buy;
    pse::fill_rect(t, k_buy_x, k_row_y, k_buy_w, k_row_h, buy_chosen ? k_navy.r : k_black.r,
                   buy_chosen ? k_navy.g : k_black.g, buy_chosen ? k_navy.b : k_black.b);
    {
        const Rgb edge = buy_chosen ? k_white : k_grey;
        pse::draw_rect(t, k_buy_x, k_row_y, k_buy_w, k_row_h, edge.r, edge.g, edge.b);
    }
    char buy[16];
    label_number(buy, sizeof(buy), "BUY ", k_buy_coin_amount);
    text_at(t, buy, k_buy_x + (k_buy_w - pse::text_width(buy)) / 2, k_row_y + 6,
            buy_chosen ? k_white : k_silver);
    char cost[14];
    label_number(cost, sizeof(cost), "G", w.buy_cost);
    text_at(t, cost, k_buy_x + (k_buy_w - pse::text_width(cost)) / 2, k_row_y + 15,
            w.gold >= w.buy_cost ? k_yellow : k_red);

    if (can_end_round(w)) {
        const bool end_chosen = sel != nullptr && sel->kind == SlotKind::end;
        const bool ready = w.round_score >= w.target;
        const Rgb fill = end_chosen ? (ready ? k_felt : k_navy) : k_black;
        pse::fill_rect(t, k_end_x, k_row_y, k_end_w, k_row_h, fill.r, fill.g, fill.b);
        const Rgb edge = end_chosen ? (ready ? k_green : k_white) : k_grey;
        pse::draw_rect(t, k_end_x, k_row_y, k_end_w, k_row_h, edge.r, edge.g, edge.b);
        text_at(t, "END", k_end_x + (k_end_w - pse::text_width("END")) / 2, k_row_y + 6,
                end_chosen ? k_white : k_silver);
        text_at(t, "ROUND", k_end_x + (k_end_w - pse::text_width("ROUND")) / 2, k_row_y + 15,
                ready ? k_green : k_silver);
    }

    // What the selection does, in words about the game rather than words about
    // the controller. This line is what replaces the cart's button prompts.
    if (sel == nullptr) return;
    if (sel->kind == SlotKind::item) {
        const uint8_t stype = w.inv[sel->index];
        text_at(t, special_name(stype), 6, k_desc_y, k_special_colour[stype - 1]);
        text_at(t, special_effect(stype), 6 + pse::text_width(special_name(stype)) + 6,
                k_desc_y, k_silver);
    } else if (sel->kind == SlotKind::buy) {
        text_at(t, k_buy_description, 6, k_desc_y, k_silver);
    } else {
        const bool ready = w.round_score >= w.target;
        text_at(t, ready ? k_end_ready : k_end_short, 6, k_desc_y, ready ? k_green : k_red);
    }
}

void draw_play(const World& w, const pse::RenderTarget& t) {
    pse::fill_rect(t, 0, 0, k_screen_w, k_screen_h, k_black.r, k_black.g, k_black.b);
    draw_field(w, t);
    draw_pusher(w, t);
    for (uint16_t i = 0; i < w.coin_count; i++) {
        const Coin& c = w.coins[i];
        draw_coin(t, c.x, c.y, c.stype);
        if (c.fuse > 0) {
            // A ring that tightens as the fuse burns down, plus a dot per
            // second left, so a fuse is legible without a number.
            const int radius = 6 + (c.fuse * 4) / k_fuse;
            const Rgb rc = k_special_colour[c.stype - 1];
            pse::draw_circle(t, c.x >> k_fp, c.y >> k_fp, radius, rc.r, rc.g, rc.b);
            const int dots = (c.fuse + 49) / 50;
            for (int d = 0; d < dots && d < 4; d++) {
                pse::plot_pixel(t, (c.x >> k_fp) - 2 + d * 3, (c.y >> k_fp) - 9, k_white.r,
                                k_white.g, k_white.b);
            }
        }
    }
    for (int i = 0; i < w.dropping_count; i++) {
        const Dropping& d = w.dropping[i];
        draw_coin(t, d.x, d.y - d.h, d.stype);
    }
    draw_lip(w, t);
    draw_tray(w, t);
    for (int i = 0; i < w.particle_count; i++) {
        const Particle& p = w.particles[i];
        const bool bright = p.life * 2 > p.max_life;
        const Rgb c = bright ? k_yellow : k_grey;
        pse::h_line(t, p.x >> k_fp, (p.x >> k_fp) + 1, p.y >> k_fp, c.r, c.g, c.b);
    }
    draw_cat(w, t, w.disp_x >> k_fp, k_rail_y);
    pse::fill_rect(t, k_fl - 9, k_rail_y + 1, k_fw + 18, 3, k_grey.r, k_grey.g, k_grey.b);
    pse::h_line(t, k_fl - 9, k_fr + 8, k_rail_y + 1, k_lavender.r, k_lavender.g, k_lavender.b);
    draw_hud(w, t);
    draw_progress(w, t);
    draw_row(w, t);
    if (w.flash > 0) {
        // One frame of edge light when a special goes off. A coin pusher with
        // 200 loose coins does not need help looking busy.
        pse::draw_rect(t, k_fl - 1, k_ft - 1, k_fw + 3, k_fh + 3, k_white.r, k_white.g,
                       k_white.b);
    }
}

void draw_title(const World& w, const pse::RenderTarget& t) {
    pse::fill_rect(t, 0, 0, k_screen_w, k_screen_h, k_black.r, k_black.g, k_black.b);
    for (int i = 0; i < 70; i++) {
        static const Rgb stars[4] = {k_navy, k_grey, k_silver, k_lavender};
        const int x = (i * 37 + static_cast<int>(w.ticks / 6)) % k_screen_w;
        const int y = (i * 53 + static_cast<int>(w.ticks / 18)) % k_screen_h;
        const Rgb c = stars[i % 4];
        pse::plot_pixel(t, x, y, c.r, c.g, c.b);
    }
    const int bob = static_cast<int>((w.ticks / 30) % 10) - 5;
    draw_cat(w, t, k_screen_w / 2, 74 + bob);
    text_shadowed(t, "CAT COIN", k_screen_w / 2, 86 + bob, k_yellow, 3);
    text_shadowed(t, "PUSHER", k_screen_w / 2, 114 + bob, k_orange, 3);
    text_shadowed(t, "A CART DEMAKE", k_screen_w / 2, 142 + bob, k_grey, 1);
    const int cy = 180 + (static_cast<int>(w.ticks / 12) % 12) - 6;
    draw_coin(t, (k_screen_w / 2) * k_one, cy * k_one, spc_none);
}

void draw_shop(const World& w, const pse::RenderTarget& t) {
    pse::fill_rect(t, 0, 0, k_screen_w, k_screen_h, k_black.r, k_black.g, k_black.b);
    pse::fill_rect(t, 0, 0, k_screen_w, 21, k_navy.r, k_navy.g, k_navy.b);
    text_centred(t, "CATS SHOP", k_screen_w / 2, 3, k_white);
    char sub[24];
    label_number(sub, sizeof(sub), "ROUND ", w.round);
    append(sub, sizeof(sub), " CLEAR");
    text_centred(t, sub, k_screen_w / 2, 13, k_silver);

    char gold[16];
    label_number(gold, sizeof(gold), "GOLD ", w.gold);
    text_at(t, gold, 6, 26, k_yellow);
    char bag[16];
    label_number(bag, sizeof(bag), "BAG ", w.inv_count);
    append(bag, sizeof(bag), "/");
    char cap[6];
    number_string(k_inv_max, cap, sizeof(cap));
    append(bag, sizeof(bag), cap);
    text_right(t, bag, k_screen_w - 6, 26, k_silver);

    for (int i = 0; i < 3; i++) {
        const ShopItem& item = w.shop[i];
        const int y = 38 + i * 34;
        const bool chosen = i == w.shop_sel;
        pse::fill_rect(t, 6, y, k_screen_w - 12, 31, chosen ? k_navy.r : k_black.r,
                       chosen ? k_navy.g : k_black.g, chosen ? k_navy.b : k_black.b);
        const Rgb edge = chosen ? k_white : k_grey;
        pse::draw_rect(t, 6, y, k_screen_w - 12, 31, edge.r, edge.g, edge.b);
        draw_coin(t, 22 * k_one, (y + 15) * k_one, item.stype);
        const Rgb name_c = item.sold ? k_grey : k_special_colour[item.stype - 1];
        text_at(t, special_name(item.stype), 36, y + 7, name_c);
        text_at(t, special_effect(item.stype), 36, y + 18, item.sold ? k_grey : k_silver);
        if (item.sold) {
            text_right(t, "SOLD", k_screen_w - 12, y + 12, k_grey);
        } else {
            char price[14];
            label_number(price, sizeof(price), "G", item.cost);
            text_right(t, price, k_screen_w - 12, y + 12,
                       w.gold >= item.cost ? k_yellow : k_red);
        }
    }

    const int ry = 146;
    const bool refresh_chosen = w.shop_sel == 3;
    pse::fill_rect(t, 6, ry, k_screen_w - 12, 21, refresh_chosen ? k_navy.r : k_black.r,
                   refresh_chosen ? k_navy.g : k_black.g, refresh_chosen ? k_navy.b : k_black.b);
    {
        const Rgb edge = refresh_chosen ? k_white : k_grey;
        pse::draw_rect(t, 6, ry, k_screen_w - 12, 21, edge.r, edge.g, edge.b);
    }
    text_at(t, "REFRESH ITEMS", 20, ry + 8, refresh_chosen ? k_white : k_silver);
    char refresh[14];
    label_number(refresh, sizeof(refresh), "G", w.refresh_cost);
    text_right(t, refresh, k_screen_w - 12, ry + 8,
               w.gold >= w.refresh_cost ? k_yellow : k_red);

    const int cy = 176;
    const bool go = w.shop_sel == 4;
    pse::fill_rect(t, 56, cy, k_screen_w - 112, 21, go ? k_felt.r : k_navy.r,
                   go ? k_felt.g : k_navy.g, go ? k_felt.b : k_navy.b);
    {
        const Rgb edge = go ? k_green : k_grey;
        pse::draw_rect(t, 56, cy, k_screen_w - 112, 21, edge.r, edge.g, edge.b);
    }
    text_centred(t, "CONTINUE", k_screen_w / 2, cy + 8, go ? k_white : k_silver);
    draw_cat(w, t, k_screen_w / 2, 232);
}

void draw_spinner(const World& w, const pse::RenderTarget& t) {
    pse::fill_rect(t, 0, 0, k_screen_w, k_screen_h, k_black.r, k_black.g, k_black.b);
    pse::fill_rect(t, 0, 0, k_screen_w, 23, k_navy.r, k_navy.g, k_navy.b);
    text_centred(t, "COMBO PRIZE", k_screen_w / 2, 3, k_white);
    char sub[20];
    label_number(sub, sizeof(sub), "X", w.spin_mult);
    append(sub, sizeof(sub), " MULTIPLIER");
    text_centred(t, sub, k_screen_w / 2, 14, k_silver);

    pse::fill_rect(t, 16, 32, k_screen_w - 33, 147, k_navy.r, k_navy.g, k_navy.b);
    pse::draw_rect(t, 16, 32, k_screen_w - 33, 147, k_white.r, k_white.g, k_white.b);
    for (int i = 0; i < 3; i++) {
        const SpinItem& item = w.spin[i];
        const int y = 40 + i * 46;
        const bool live = !w.spin_done && (w.spin_pos >> k_fp) == i;
        const bool won = w.spin_done && w.spin_result == i;
        if (live) {
            pse::fill_rect(t, 20, y, k_screen_w - 41, 39, k_plum.r, k_plum.g, k_plum.b);
        }
        if (won) {
            pse::fill_rect(t, 20, y, k_screen_w - 41, 39, k_felt.r, k_felt.g, k_felt.b);
            const Rgb flash = (w.ticks % 34) < 17 ? k_green : k_yellow;
            pse::draw_rect(t, 20, y, k_screen_w - 41, 39, flash.r, flash.g, flash.b);
        } else {
            pse::draw_rect(t, 20, y, k_screen_w - 41, 39, k_grey.r, k_grey.g, k_grey.b);
        }
        char label[24];
        Rgb colour = k_yellow;
        if (item.kind == 0) {
            draw_coin(t, 40 * k_one, (y + 19) * k_one, spc_none);
            draw_coin(t, 52 * k_one, (y + 19) * k_one, spc_none);
            label_number(label, sizeof(label), "+", item.amount);
            append(label, sizeof(label), " COINS");
        } else if (item.kind == 1) {
            pse::fill_circle(t, 46, y + 19, 8, k_yellow.r, k_yellow.g, k_yellow.b);
            pse::fill_circle(t, 46, y + 19, 6, k_orange.r, k_orange.g, k_orange.b);
            label_number(label, sizeof(label), "+", item.amount);
            append(label, sizeof(label), " GOLD");
            colour = k_orange;
        } else {
            draw_coin(t, 46 * k_one, (y + 19) * k_one, item.stype);
            label[0] = '\0';
            append(label, sizeof(label), special_name(item.stype));
            append(label, sizeof(label), " COIN");
            colour = k_special_colour[item.stype - 1];
        }
        text_at(t, label, 70, y + 16, colour);
    }
    if (w.spin_done && w.spin_result >= 0) {
        text_centred(t, "PRIZE WON", k_screen_w / 2, 190, k_white);
    }
    draw_cat(w, t, k_screen_w / 2, 232);
}

void draw_end(const World& w, const pse::RenderTarget& t, bool won) {
    pse::fill_rect(t, 0, 0, k_screen_w, k_screen_h, k_black.r, k_black.g, k_black.b);
    if (won) {
        for (int i = 0; i < 80; i++) {
            static const Rgb confetti[4] = {k_yellow, k_orange, k_green, k_white};
            const int x = (i * 23 + static_cast<int>(w.ticks / 4)) % k_screen_w;
            const int y = (i * 41 + static_cast<int>(w.ticks / 10)) % k_screen_h;
            const Rgb c = confetti[i % 4];
            pse::plot_pixel(t, x, y, c.r, c.g, c.b);
        }
    }
    draw_cat(w, t, k_screen_w / 2, 62);
    text_shadowed(t, won ? "YOU WIN" : "GAME OVER", k_screen_w / 2, 78,
                  won ? k_green : k_red, 3);

    char line[28];
    if (won) {
        label_number(line, sizeof(line), "ALL ", k_max_rounds);
        append(line, sizeof(line), " ROUNDS CLEAR");
        text_centred(t, line, k_screen_w / 2, 118, k_yellow);
        label_number(line, sizeof(line), "TOTAL GOLD ", w.gold);
        text_centred(t, line, k_screen_w / 2, 134, k_orange);
    } else {
        label_number(line, sizeof(line), "REACHED ROUND ", w.round);
        text_centred(t, line, k_screen_w / 2, 118, k_white);
        label_number(line, sizeof(line), "SCORE ", w.round_score);
        text_centred(t, line, k_screen_w / 2, 132, k_silver);
        label_number(line, sizeof(line), "TARGET ", w.target);
        text_centred(t, line, k_screen_w / 2, 146,
                     w.round_score >= w.target ? k_green : k_red);
    }
    if (w.high_score > 0) {
        label_number(line, sizeof(line), "BEST ", w.high_score);
        text_centred(t, line, k_screen_w / 2, 168, k_grey);
    }
}

}  // namespace

const char* special_name(uint8_t stype) {
    if (stype == 0 || stype > k_num_specials) return "";
    return k_names[stype - 1];
}

const char* description_line(int index) {
    switch (index) {
        case 0: return k_buy_description;
        case 1: return k_end_ready;
        default: return k_end_short;
    }
}

const char* special_effect(uint8_t stype) {
    if (stype == 0 || stype > k_num_specials) return "";
    return k_effects[stype - 1];
}

void render_world(const World& world, const pse::RenderTarget& target) {
    switch (world.state) {
        case State::title: draw_title(world, target); break;
        case State::play: draw_play(world, target); break;
        case State::shop: draw_shop(world, target); break;
        case State::spinner: draw_spinner(world, target); break;
        case State::over: draw_end(world, target, false); break;
        case State::win: draw_end(world, target, true); break;
    }
}

}  // namespace cc
