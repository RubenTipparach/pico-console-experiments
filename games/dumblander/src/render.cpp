#include "render.hpp"

#include "pse/draw2d.hpp"
#include "pse/text.hpp"

#include "dumblander/lander.hpp"
#include "dumblander/rocks.hpp"

namespace dl {
namespace {

struct Rgb {
    uint8_t r, g, b;
};

// PICO-8's palette, which is what the cart was drawn in, plus two the cart
// could not afford at 128x128 and this can: one red a step under colour 8 and
// one for the deep shadow the ground gets when it is 60 px tall instead of 20.
constexpr Rgb k_black{0x00, 0x00, 0x00};
constexpr Rgb k_dark_blue{0x1D, 0x2B, 0x53};
constexpr Rgb k_dark_purple{0x7E, 0x25, 0x53};
constexpr Rgb k_grey{0x5F, 0x57, 0x4F};
constexpr Rgb k_light_grey{0xC2, 0xC3, 0xC7};
constexpr Rgb k_white{0xFF, 0xF1, 0xE8};
constexpr Rgb k_red{0xFF, 0x00, 0x4D};
constexpr Rgb k_orange{0xFF, 0xA3, 0x00};
constexpr Rgb k_yellow{0xFF, 0xEC, 0x27};
constexpr Rgb k_green{0x00, 0xE4, 0x36};
constexpr Rgb k_mid_red{0xB0, 0x00, 0x3A};

const Rgb k_flame[3] = {k_yellow, k_orange, k_red};
const Rgb k_debris[5] = {k_red, k_orange, k_yellow, {0x83, 0x76, 0x9C}, k_white};

// The one place the HUD's geometry is written down. Everything else measures.
constexpr int k_hud_x = 5;
constexpr int k_bar_y = 13;
constexpr int k_bar_w = 66;
constexpr int k_bar_h = 7;

int shake_offset(const World& world) {
    if (world.shake == 0) return 0;
    const int magnitude = world.shake / 5;
    return ((world.ticks & 1) ? magnitude : -magnitude);
}

void draw_sky(const World& world, const pse::RenderTarget& t) {
    pse::fill_rect(t, 0, 0, k_screen_w, k_screen_h, k_black.r, k_black.g, k_black.b);
    for (int i = 0; i < k_max_stars; i++) {
        const Star& s = world.stars[i];
        const Rgb c = s.bright ? k_light_grey : k_grey;
        pse::plot_pixel(t, s.x, s.y, c.r, c.g, c.b);
    }
}

void draw_terrain(const World& world, const pse::RenderTarget& t, int shake) {
    // The cart's solid red, with a lit rim on top and two steps of shade
    // below. At 128 the ground was 20 px tall and one colour was all it could
    // carry; here it is 60 and a flat slab reads as a hole.
    for (int x = 0; x < k_screen_w; x++) {
        const int y = (world.terrain[x] >> 8) + shake;
        pse::v_line(t, x, y + 44, k_screen_h - 1, k_dark_purple.r, k_dark_purple.g,
                    k_dark_purple.b);
        pse::v_line(t, x, y + 18, y + 43, k_mid_red.r, k_mid_red.g, k_mid_red.b);
        pse::v_line(t, x, y + 2, y + 17, k_red.r, k_red.g, k_red.b);
        pse::v_line(t, x, y, y + 1, k_orange.r, k_orange.g, k_orange.b);
    }
    for (int i = 0; i < world.rock_count; i++) {
        const Rock& rock = world.rocks[i];
        constexpr int k_cell = 9;
        pse::blit_sprite(t, models::dumblander::rocks, rock.kind * k_cell, 0, k_cell, 7,
                         rock.x - k_cell / 2, (rock.y >> 8) + shake - 3);
    }
}

void draw_pad(const pse::RenderTarget& t, const Pad& pad, int shake, Rgb body, Rgb rim) {
    const int y = (pad.y >> 8) + shake;
    pse::fill_rect(t, pad.x, y, pad.w, 4, body.r, body.g, body.b);
    pse::h_line(t, pad.x, pad.x + pad.w - 1, y, rim.r, rim.g, rim.b);
    // Two posts, so the deck has an edge you can aim between.
    pse::v_line(t, pad.x, y - 4, y - 1, rim.r, rim.g, rim.b);
    pse::v_line(t, pad.x + pad.w - 1, y - 4, y - 1, rim.r, rim.g, rim.b);
    pse::plot_pixel(t, pad.x, y - 5, rim.r, rim.g, rim.b);
    pse::plot_pixel(t, pad.x + pad.w - 1, y - 5, rim.r, rim.g, rim.b);
}

void draw_lander(const World& world, const pse::RenderTarget& t, int shake) {
    const int x = world.x >> k_fp;
    const int y = (world.y >> k_fp) + shake;

    for (int i = 0; i < world.flame_count; i++) {
        const Particle& p = world.flame[i];
        const Rgb c = k_flame[p.life > 9 ? 0 : (p.life > 5 ? 1 : 2)];
        pse::plot_pixel(t, p.x >> k_fp, (p.y >> k_fp) + shake, c.r, c.g, c.b);
    }
    if (world.thrusting) {
        // A tapering plume out of the bell, three bands, with the length
        // wobbling on the tick so it reads as burning rather than as a shape.
        const int len = 7 + static_cast<int>(world.ticks % 3);
        for (int i = 0; i < len; i++) {
            const int half = i < 3 ? 2 : (i < 6 ? 1 : 0);
            const Rgb c = i < 3 ? k_white : (i < 5 ? k_yellow : k_orange);
            pse::h_line(t, x - half, x + half, y - 2 + i, c.r, c.g, c.b);
        }
    }
    if (world.jet != 0) {
        // Gas leaves the side the hull is not accelerating toward.
        const int sx = x - world.jet * 8;
        for (int i = 0; i < 4; i++) {
            const Rgb c = i < 2 ? k_white : k_light_grey;
            pse::plot_pixel(t, sx - world.jet * i, y - 12 + (i & 1), c.r, c.g, c.b);
        }
    }

    // The sprite's last row is the row the feet stand on, which is the row the
    // sim puts on the ground, so there is no offset to remember here.
    pse::blit_sprite(t, models::dumblander::lander, x - k_hull_w / 2, y - (k_hull_h - 1));
}

void draw_debris(const World& world, const pse::RenderTarget& t) {
    for (int i = 0; i < world.debris_count; i++) {
        const Particle& p = world.debris[i];
        const Rgb c = k_debris[p.colour % 5];
        // Two pixels a side: one pixel of debris on a 240 wide screen is
        // indistinguishable from a star.
        pse::fill_rect(t, p.x >> k_fp, p.y >> k_fp, 2, 2, c.r, c.g, c.b);
    }
}

void draw_number(const pse::RenderTarget& t, int value, int x, int y, Rgb c) {
    char buffer[12];
    int n = 0;
    if (value <= 0) {
        buffer[n++] = '0';
    } else {
        char digits[12];
        int d = 0;
        while (value > 0 && d < 11) {
            digits[d++] = static_cast<char>('0' + value % 10);
            value /= 10;
        }
        while (d > 0) buffer[n++] = digits[--d];
    }
    buffer[n] = '\0';
    pse::draw_text(t, buffer, x, y, c.r, c.g, c.b);
}

int number_width(int value) {
    int digits = 1;
    while (value >= 10) {
        value /= 10;
        digits++;
    }
    return digits * pse::k_glyph_advance - 1;
}

void draw_hud(const World& world, const pse::RenderTarget& t) {
    pse::draw_text(t, "FUEL", k_hud_x, 4, k_light_grey.r, k_light_grey.g, k_light_grey.b);

    pse::draw_rect(t, k_hud_x, k_bar_y, k_bar_w, k_bar_h, k_grey.r, k_grey.g, k_grey.b);
    const int fuel_pct = static_cast<int>(world.fuel / (k_tank / 100));
    const int filled = (fuel_pct * (k_bar_w - 2)) / 100;
    if (filled > 0) {
        // Amber at a third and red at an eighth, so the moment to stop being
        // ambitious arrives before the tank is empty rather than after.
        const Rgb c = fuel_pct > 33 ? k_green : (fuel_pct > 12 ? k_orange : k_red);
        pse::fill_rect(t, k_hud_x + 1, k_bar_y + 1, filled, k_bar_h - 2, c.r, c.g, c.b);
    }

    // Speed in pixels per second, which is a number a person can hold, and it
    // is the only number with a colour because it is the whole game read at a
    // glance.
    const int px_per_second = (world.speed * 100) >> k_fp;
    const bool safe = world.speed <= k_safe;
    pse::draw_text(t, "SPD", k_hud_x, 25, k_light_grey.r, k_light_grey.g, k_light_grey.b);
    draw_number(t, px_per_second, k_hud_x + pse::text_width("SPD "), 25,
                safe ? k_green : k_red);

    // Right aligned off a measured width, never a hand picked x: rule 9.
    const int leg_w = pse::text_width("LEG ") + number_width(world.leg);
    pse::draw_text(t, "LEG", k_screen_w - 5 - leg_w, 4, k_light_grey.r, k_light_grey.g,
                   k_light_grey.b);
    draw_number(t, world.leg, k_screen_w - 5 - number_width(world.leg), 4, k_light_grey);
}

void draw_centred(const pse::RenderTarget& t, const char* text, int y, Rgb c, int scale) {
    pse::draw_text_centred(t, text, k_screen_w / 2 + scale, y + scale, 0, 0, 0, scale);
    pse::draw_text_centred(t, text, k_screen_w / 2, y, c.r, c.g, c.b, scale);
}

void draw_message(const World& world, const pse::RenderTarget& t) {
    if (world.state == State::landed) {
        draw_centred(t, "LANDED", 92, k_green, 3);
        return;
    }
    if (world.state != State::over) return;
    draw_centred(t, world.ending == Ending::stranded ? "STRANDED" : "CRASHED", 92, k_red, 3);
    // Legs completed, which is the score. Nothing here names a button: any
    // button starts another run, so no press can be the wrong guess.
    char line[16] = "LEGS ";
    int n = 5;
    int legs = world.leg - 1;
    if (legs <= 0) {
        line[n++] = '0';
    } else {
        char digits[8];
        int d = 0;
        while (legs > 0 && d < 7) {
            digits[d++] = static_cast<char>('0' + legs % 10);
            legs /= 10;
        }
        while (d > 0) line[n++] = digits[--d];
    }
    line[n] = '\0';
    draw_centred(t, line, 124, k_light_grey, 1);
}

void draw_title(const World& world, const pse::RenderTarget& t) {
    draw_sky(world, t);
    draw_terrain(world, t, 0);
    draw_pad(t, world.start, 0, k_grey, k_light_grey);
    draw_pad(t, world.goal, 0, k_orange, k_yellow);

    // The lander bobs on a pad while the title sits. Which pad is not a matter
    // of taste: the terrain is generated, so on some seeds a pad sits high
    // enough that a lander on it would be drawn through the title. Take the
    // lower of the two, and if even that does not clear the text, show no
    // lander rather than a collision. The bottom of the text block is measured
    // from the text, not guessed.
    constexpr int k_title_y = 14;
    constexpr int k_sub_y = 80;
    const int text_bottom = k_sub_y + pse::text_height(1);
    const Pad& pad = world.start.y > world.goal.y ? world.start : world.goal;
    const int bob = static_cast<int>((world.ticks / 40) % 8) - 4;
    const int lander_y = (pad.y >> 8) - 14 + bob;
    if (lander_y - k_hull_h > text_bottom + 6) {
        pse::blit_sprite(t, models::dumblander::lander,
                         pad.x + pad.w / 2 - k_hull_w / 2, lander_y - (k_hull_h - 1));
    }

    draw_centred(t, "DUMB", k_title_y, k_yellow, 4);
    draw_centred(t, "LANDER", 48, k_orange, 4);
    draw_centred(t, "A CART DEMAKE", k_sub_y, k_grey, 1);
}

}  // namespace

void render_world(const World& world, const pse::RenderTarget& target) {
    if (world.state == State::title) {
        draw_title(world, target);
        return;
    }
    const int shake = shake_offset(world);
    draw_sky(world, target);
    draw_terrain(world, target, shake);
    draw_pad(target, world.start, shake, k_grey, k_light_grey);
    draw_pad(target, world.goal, shake, k_orange, k_yellow);
    if (world.state != State::over || world.ending == Ending::stranded) {
        draw_lander(world, target, shake);
    }
    draw_debris(world, target);
    draw_hud(world, target);
    draw_message(world, target);
}

}  // namespace dl
