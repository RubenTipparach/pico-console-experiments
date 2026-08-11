// Joker Reels: the SDK facing half.
//
// Everything here is text, input, and the two screens that are nothing but
// text. The machine itself is drawn in render.cpp against a pse::RenderTarget,
// which is why the preview harness can render it on a host with no device.
// What is in this file is exactly what the preview cannot see, so a change to
// any of it is unverified until the game has actually been run.

#include "32blit.hpp"

#include "pse/blit_target.hpp"
#include "pse/game.hpp"

#include "render.hpp"
#include "sim.hpp"

using namespace blit;

namespace {

jr::World g_world;

// The sim's RAM footprint is a promise, checked by the compiler.
static_assert(sizeof(jr::World) <= 640, "the run state grew past its budget");

// Any button acts. With nothing on screen naming one, no press can be the
// wrong guess, which is rule 9's reason for having no button prompts at all.
constexpr uint32_t k_any_button =
    Button::A | Button::B | Button::X | Button::Y |
    Button::DPAD_UP | Button::DPAD_DOWN | Button::DPAD_LEFT |
    Button::DPAD_RIGHT;

const Pen k_ink(0, 0, 0);
const Pen k_paper(255, 241, 232);
const Pen k_dim(95, 87, 79);
const Pen k_gold(255, 236, 39);
const Pen k_chip(41, 173, 255);
const Pen k_mult(255, 0, 77);
const Pen k_good(0, 228, 54);
const Pen k_warn(255, 0, 77);
const Pen k_amber(255, 163, 0);

// Every string is measured rather than placed by eye. A hand tuned x is only
// correct for the exact string it was tuned against, so the first wording
// change prints through the edge of its own panel and nothing catches it.
void text_at(const char* line, int x, int y, Pen pen) {
    screen.pen = pen;
    screen.text(line, minimal_font, Point(x, y));
}

void text_right(const char* line, int right, int y, Pen pen) {
    const Size size = screen.measure_text(line, minimal_font);
    text_at(line, right - size.w, y, pen);
}

void text_centred(const char* line, int y, Pen pen) {
    const Size size = screen.measure_text(line, minimal_font);
    text_at(line, (screen.bounds.w - size.w) / 2, y, pen);
}

// Centred inside a box rather than on the screen, for the speed dial and the
// score box. Same rule, different box.
void text_in(const char* line, int x, int w, int y, Pen pen) {
    const Size size = screen.measure_text(line, minimal_font);
    text_at(line, x + (w - size.w) / 2, y, pen);
}

char* write_int(char* out, int32_t value) {
    if (value < 0) { *out++ = '-'; value = -value; }
    char digits[12];
    int n = 0;
    do { digits[n++] = static_cast<char>('0' + value % 10); value /= 10; }
    while (value > 0);
    while (n > 0) *out++ = digits[--n];
    return out;
}

// One scratch line, because a game does not allocate and does not carry a
// std::string anywhere near a render.
char g_line[64];

const char* line_of(const char* prefix, int32_t value, const char* suffix = "") {
    char* out = g_line;
    while (*prefix) *out++ = *prefix++;
    out = write_int(out, value);
    while (*suffix) *out++ = *suffix++;
    *out = '\0';
    return g_line;
}

const char* bank_line(int32_t banked, int32_t target) {
    char* out = write_int(g_line, banked);
    *out++ = '/';
    out = write_int(out, target);
    *out = '\0';
    return g_line;
}

void draw_hud() {
    const int top = jrr::k_window_h;

    text_at(line_of("ANTE ", g_world.ante, "/8"), 4, top + 4, k_paper);
    text_right(bank_line(g_world.banked, g_world.target), 176, top + 4,
               g_world.banked >= g_world.target ? k_good : k_dim);
    text_right(line_of("G", g_world.gold), 236, top + 4, k_gold);
    text_right(line_of("SPINS ", g_world.spins), 236, top + 13,
               g_world.spins > 1 ? k_dim : k_warn);

    // Chips and mult, separately and large, because watching one of them grow
    // is the whole feel this is borrowing from.
    text_at(line_of("", g_world.chips), 8, top + 38, k_chip);
    text_at("X", 58, top + 38, k_paper);
    text_at(line_of("", g_world.mult), 70, top + 38, k_mult);
    text_at(line_of("SCORE ", g_world.chips * g_world.mult), 8, top + 48,
            k_amber);

    text_at("SPEED", 120, top + 32, k_dim);
    for (int i = 0; i < jr::k_speeds; i++) {
        const bool on = i == g_world.speed;
        text_in(jr::speed_name(static_cast<uint8_t>(i)), 120 + i * 38, 35,
                top + 45, on ? k_ink : k_dim);
    }
    const int bonus = jr::speed_mult(g_world.speed);
    text_right(bonus > 0 ? line_of("+", bonus, " MULT") : "READABLE",
               236, top + 60, bonus > 0 ? k_amber : k_good);

    // One tally line at a time. The count is the animation.
    if (g_world.tally_step > 0 && g_world.tally_step <= g_world.tally_len) {
        const jr::TallyEntry& e = g_world.tally[g_world.tally_step - 1];
        text_at(e.what, 4, top + 70, e.joker ? k_gold : k_paper);
        const Size size = screen.measure_text(e.what, minimal_font);
        if (e.mult == -1) {
            text_at("X2 MULT", 4 + size.w + 6, top + 70, k_mult);
        } else if (e.chips) {
            text_at(line_of("+", e.chips, " CHIPS"), 4 + size.w + 6, top + 70,
                    k_chip);
        } else if (e.mult) {
            text_at(line_of("+", e.mult, " MULT"), 4 + size.w + 6, top + 70,
                    k_mult);
        }
    }

    for (int i = 0; i < g_world.joker_count; i++) {
        text_at(jr::joker_name(g_world.jokers[i]), 8 + i * 46, top + 97, k_dim);
    }

    if (g_world.msg) {
        text_centred(g_world.msg, top + 114, k_good);
    } else if (g_world.state == jr::kCount) {
        text_centred(jr::hand_name(g_world.hand_index), top + 114, k_gold);
    }
}

void draw_shop() {
    text_centred("THE BACK ROOM", 6, k_paper);
    text_at(line_of("GOLD ", g_world.gold), 6, 18, k_gold);
    text_right(line_of("ANTE ", g_world.ante, " CLEAR"), 234, 18, k_good);

    for (int i = 0; i < g_world.shop_len; i++) {
        const jr::ShopItem& item = g_world.shop[i];
        const int y = 34 + i * 42;
        const bool sel = i == g_world.shop_sel;
        const char* title = "";
        const char* body = "";
        if (item.kind == jr::kShopJoker) {
            title = jr::joker_name(item.which);
            body = jr::joker_text(item.which);
        } else if (item.kind == jr::kShopHand) {
            title = jr::hand_name(item.which);
            body = "LEVEL IT UP";
        } else {
            title = "OPEN A DRUM";
            body = "SWAP ONE FACE FOR ANY SYMBOL";
        }
        text_at(title, 12, y + 8, item.sold ? k_dim : k_paper);
        text_at(body, 12, y + 22, k_dim);
        text_right(item.sold ? "SOLD" : line_of("G", item.cost), 228, y + 8,
                   item.sold ? k_dim
                             : (g_world.gold >= item.cost ? k_gold : k_warn));
    }

    const int y = 34 + g_world.shop_len * 42;
    const bool sel = g_world.shop_sel >= g_world.shop_len;
    text_in("NEXT ANTE", 70, 100, y + 6, sel ? k_paper : k_dim);
}

void draw_swap() {
    const int top = jrr::k_window_h;
    text_centred("OPEN A DRUM", top + 4, k_gold);
    char* out = g_line;
    const char* p = "DRUM ";
    while (*p) *out++ = *p++;
    out = write_int(out, g_world.swap_drum + 1);
    *out++ = ' '; *out++ = ' ';
    p = "SYMBOL ";
    while (*p) *out++ = *p++;
    out = write_int(out, g_world.swap_face + 1);
    *out++ = '/';
    out = write_int(out, g_world.strip_len[g_world.swap_drum]);
    *out = '\0';
    text_at(g_line, 6, top + 18, k_dim);

    const uint8_t now = g_world.strip[g_world.swap_drum][g_world.swap_face];
    const uint8_t to = g_world.swap_to;
    text_at("NOW", 6, top + 34, k_dim);
    text_at(jr::symbol_name(now), 6, top + 46, k_paper);
    text_at(line_of("", jr::symbol_chips(now), " CHIPS"), 6, top + 58, k_dim);

    text_centred("TO", top + 46, k_paper);

    text_right("NEW", 234, top + 34, k_dim);
    text_right(jr::symbol_name(to), 234, top + 46, k_gold);
    text_right(line_of("", jr::symbol_chips(to), " CHIPS"), 234, top + 58,
               k_chip);

    text_at("A DRUM LANDS ONLY ON WHAT IS ON IT", 6, top + 108, k_dim);
}

void draw_end() {
    const bool won = g_world.state == jr::kWin;
    text_centred(won ? "YOU BROKE THE BANK" : "OUT OF SPINS", 60,
                 won ? k_good : k_warn);
    text_centred(line_of("ANTE ", g_world.ante, " OF 8"), 84, k_paper);
    text_centred(bank_line(g_world.banked, g_world.target), 98, k_dim);
    int y = 124;
    for (int i = 0; i < g_world.joker_count; i++) {
        text_centred(jr::joker_name(g_world.jokers[i]), y, k_gold);
        y += 12;
    }
}

void draw_title() {
    const int top = jrr::k_window_h;
    screen.pen = k_gold;
    text_centred("JOKER REELS", top + 24, k_gold);
    text_centred("EIGHT ANTES, FIVE SPINS EACH", top + 56, k_dim);
    text_centred("A DRUM LANDS ON WHAT YOU PUT ON IT", top + 70, k_dim);
}

// ---------------------------------------------------------------------------

void game_init() {
    // Hires. This is the one 3D game here that is not lores, and it can be
    // because the 3D covers the top 112 rows rather than the whole screen: see
    // render.hpp for the arithmetic.
    set_screen_mode(ScreenMode::hires);
    jr::world_init(g_world, 0x5EED5EEDu);
}

void game_update(uint32_t time) {
    (void)time;
    jr::Buttons btn{};
    btn.a = buttons.pressed & Button::A;
    btn.b = buttons.pressed & Button::B;
    btn.up = buttons.pressed & Button::DPAD_UP;
    btn.down = buttons.pressed & Button::DPAD_DOWN;
    btn.left = buttons.pressed & Button::DPAD_LEFT;
    btn.right = buttons.pressed & Button::DPAD_RIGHT;
    btn.any = (buttons.pressed & k_any_button) != 0;
    jr::world_tick(g_world, btn);
}

void game_render(uint32_t time) {
    (void)time;
    const pse::RenderTarget target = pse::target_from_screen();

    if (g_world.state == jr::kShop) {
        jrr::render_shop(g_world, target);
        draw_shop();
        return;
    }
    if (g_world.state == jr::kOver || g_world.state == jr::kWin) {
        jrr::render_end(g_world, target);
        draw_end();
        return;
    }

    jrr::render_machine(g_world, target);
    jrr::render_panel(g_world, target);

    if (g_world.state == jr::kTitle) { draw_title(); return; }
    if (g_world.state == jr::kSwap) { draw_swap(); return; }
    draw_hud();
}

}  // namespace

PSE_GAME(jokerreels, game_init, game_update, game_render);
