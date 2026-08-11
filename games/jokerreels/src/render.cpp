#include "render.hpp"

#include <cmath>

#include "pse/draw2d.hpp"
#include "pse/parallel.hpp"
#include "pse/raster.hpp"
#include "pse/renderer3d.hpp"
#include "pse/text.hpp"

#include "jokerreels/bar.hpp"
#include "jokerreels/bell.hpp"
#include "jokerreels/cap.hpp"
#include "jokerreels/cherry.hpp"
#include "jokerreels/clover.hpp"
#include "jokerreels/crown.hpp"
#include "jokerreels/diamond.hpp"
#include "jokerreels/facet.hpp"
#include "jokerreels/plum.hpp"
#include "jokerreels/seven.hpp"

namespace jrr {
namespace {

// Its own, sized to the window. Not the shared one: that carries the default
// 120x120 buffer, and this game draws a 240x112 band.
pse::OwnedRasterizer<k_screen_w, k_window_h> g_raster;
pse::Renderer3D g_renderer(g_raster);
pse::FrameQueue g_queue;
Stats g_stats{0, 0, 0};

// The eight symbol textures, in sim.hpp's Symbol order. A ScreenTriangle's
// `tex` is 1 based, so a symbol's texture index is its enum value plus one.
const pse::Texture k_textures[jr::k_symbols] = {
    models::jokerreels::cherry,  models::jokerreels::bell,
    models::jokerreels::plum,    models::jokerreels::bar,
    models::jokerreels::clover,  models::jokerreels::seven,
    models::jokerreels::diamond, models::jokerreels::crown,
};

const float k_drum_x[jr::k_drums] = {
    -2.0f * k_drum_gap, -k_drum_gap, 0.0f, k_drum_gap, 2.0f * k_drum_gap,
};


// Palette, matching the mockup and the gallery's own colours. RGB triples
// rather than indices because the engine takes colours, not a palette.
struct Rgb { uint8_t r, g, b; };
constexpr Rgb k_frame       = {0x1D, 0x2B, 0x53};
constexpr Rgb k_chrome      = {0x83, 0x76, 0x9C};
constexpr Rgb k_payline     = {0xFF, 0xA3, 0x00};
constexpr Rgb k_payline_dim = {0xAB, 0x52, 0x36};
constexpr Rgb k_cabinet     = {0x00, 0x87, 0x51};
constexpr Rgb k_ink         = {0x00, 0x00, 0x00};
constexpr Rgb k_panel       = {0x1D, 0x2B, 0x53};
constexpr Rgb k_dim         = {0x5F, 0x57, 0x4F};
constexpr Rgb k_good        = {0x00, 0xE4, 0x36};
constexpr Rgb k_warn        = {0xFF, 0x00, 0x4D};
constexpr Rgb k_select      = {0xFF, 0xEC, 0x27};
constexpr Rgb k_paper       = {0xFF, 0xF1, 0xE8};
constexpr Rgb k_chip        = {0x29, 0xAD, 0xFF};
constexpr Rgb k_mult        = {0xFF, 0x00, 0x4D};

// One colour per match group, so two pairs read as two lines and not as one
// puzzle. Both are already on the panel, so nothing new has to be learned.
constexpr Rgb k_group_colour[jr::k_max_groups] = {
    {0xFF, 0xA3, 0x00}, {0x29, 0xAD, 0xFF},
};
// One colour, not two. Banding the facets light and dark was meant to sell the
// motion and does the opposite twice over: in a still frame only two bands are
// ever visible, so it reads as a dark hole with a lip, and in motion at 100 Hz
// twelve alternating bands crossing the payline is a strobe. A reel turning too
// fast to read looks like a plain cylinder, so this draws one.
constexpr Rgb k_blur_face   = {0xC2, 0xC3, 0xC7};

float radians(float degrees) { return degrees * k_pi / 180.0f; }

// A facet's angle in radians, from the sim's fixed point.
float facet_angle(int32_t angle, int facet) {
    return static_cast<float>(jr::facet_mid(angle, facet)) *
           (2.0f * k_pi / jr::k_turn);
}

/* How much to darken a facet, and why the game does this at all.
 *
 * The engine lights every face from one fixed direction and says so: a game
 * that wants different lighting should pass different colours, not
 * reconfigure the engine (renderer3d.cpp). That light comes from above and
 * slightly toward the camera, which on a drum makes the facet ABOVE the
 * payline the brightest face on the machine and pulls the eye off the only
 * face being scored.
 *
 * So the tint corrects for it. Tint can darken and never brighten, which is
 * exactly what is needed: leave the payline facet alone and pull the ones
 * above it down to just under it. The result peaks where the player is
 * looking, using the documented mechanism and touching no engine code.
 */
uint8_t facet_tint(float angle) {
    // The engine's own light and ambient, and its lambert for a facet whose
    // outward normal is (0, -sin a, -cos a).
    const float lambert = -0.82f * std::sin(angle) + 0.40f * std::cos(angle);
    const float lit = lambert > 0.0f ? lambert : 0.0f;
    const float intensity = 0.45f + 0.55f * lit;
    // The payline facet's own intensity is 0.67. Anything brighter than a
    // shade under that gets pulled back to it.
    constexpr float k_ceiling = 0.62f;
    if (intensity <= k_ceiling) return 255;
    const int tint = static_cast<int>(255.0f * k_ceiling / intensity);
    return static_cast<uint8_t>(tint < 0 ? 0 : (tint > 255 ? 255 : tint));
}

pse::RenderTarget window_of(const pse::RenderTarget& screen) {
    pse::RenderTarget window = screen;
    window.height = k_window_h;
    return window;
}

void fill(const pse::RenderTarget& t, int x0, int y0, int x1, int y1, Rgb c) {
    pse::fill_rect(t, x0, y0, x1 - x0 + 1, y1 - y0 + 1, c.r, c.g, c.b);
}

void hline(const pse::RenderTarget& t, int x0, int x1, int y, Rgb c) {
    pse::h_line(t, x0, x1, y, c.r, c.g, c.b);
}

/* Text, drawn by the engine rather than by the SDK.
 *
 * The obvious place for a HUD is game.cpp with screen.text, which is what
 * every other game here does and what this one did first. It is the wrong
 * place: game.cpp is the one file no host build compiles, so every number and
 * label in the game was invisible to the preview harness and unverified until
 * somebody ran it on hardware. The screenshots showed a machine with no score
 * on it.
 *
 * pse::draw_text exists for exactly this and says so in its header: the
 * console's menu is drawn with it so that it can be looked at without a
 * device. A HUD has the same problem, so it gets the same answer. game.cpp is
 * now input and one call.
 */
constexpr int k_text_h = pse::text_height();

void text(const pse::RenderTarget& t, const char* s, int x, int y, Rgb c,
          int scale = 1) {
    pse::draw_text(t, s, x, y, c.r, c.g, c.b, scale);
}

// Measured, never placed by eye: a hand tuned x is only correct for the exact
// string it was tuned against, so the first wording change prints through the
// edge of its own box and nothing catches it.
void text_right(const pse::RenderTarget& t, const char* s, int right, int y,
                Rgb c, int scale = 1) {
    text(t, s, right - pse::text_width(s, scale), y, c, scale);
}

void text_centred(const pse::RenderTarget& t, const char* s, int centre, int y,
                  Rgb c, int scale = 1) {
    pse::draw_text_centred(t, s, centre, y, c.r, c.g, c.b, scale);
}

char* put_int(char* out, int32_t value) {
    if (value < 0) { *out++ = '-'; value = -value; }
    char digits[12];
    int n = 0;
    do { digits[n++] = static_cast<char>('0' + value % 10); value /= 10; }
    while (value > 0);
    while (n > 0) *out++ = digits[--n];
    return out;
}

char* put_str(char* out, const char* s) {
    while (*s) *out++ = *s++;
    return out;
}

// One scratch line. A game does not allocate, and nothing here outlives the
// call that draws it.
char g_line[48];

const char* num_line(const char* prefix, int32_t value, const char* suffix) {
    char* out = put_str(g_line, prefix);
    out = put_int(out, value);
    out = put_str(out, suffix);
    *out = '\0';
    return g_line;
}

const char* pair_line(int32_t a, const char* mid, int32_t b) {
    char* out = put_int(g_line, a);
    out = put_str(out, mid);
    out = put_int(out, b);
    *out = '\0';
    return g_line;
}

// A joker slot is 43 px, which is seven characters of a 5x7 font. Truncated
// rather than shrunk: a name at half scale is a name nobody can read.
const char* short_name(const char* name, int chars) {
    int n = 0;
    while (name[n] && n < chars && n < 15) { g_line[n] = name[n]; n++; }
    g_line[n] = '\0';
    return g_line;
}

/* The lines that say WHY a hand scored.
 *
 * A tally line says PAIR and a number. It does not say which two reels paired,
 * and on five reels that is the whole question: a player who cannot see why
 * they scored cannot aim at scoring more. So each group of matching symbols
 * gets a line drawn through the reels that made it, with a marker on each one.
 *
 * Drawn over the reels rather than under them, because that is what a payline
 * is, and one pixel of line across a 42 px symbol costs nothing to read.
 */
void draw_match_lines(const jr::World& world, const pse::RenderTarget& screen) {
    if (world.group_count == 0) return;
    const int mid = k_window_h / 2;

    for (int g = 0; g < world.group_count && g < jr::k_max_groups; g++) {
        // Two groups sit either side of the payline so they cannot be mistaken
        // for one line with a gap in it.
        const int y = world.group_count == 1 ? mid
                                             : mid + (g == 0 ? -9 : 9);
        const Rgb c = k_group_colour[g];

        int first = -1, last = -1;
        for (int d = 0; d < jr::k_drums; d++) {
            if (world.group_of[d] != g) continue;
            if (first < 0) first = d;
            last = d;
        }
        if (first < 0) continue;

        int fl, fr, ll, lr;
        drum_window(first, fl, fr);
        drum_window(last, ll, lr);
        hline(screen, (fl + fr) / 2, (ll + lr) / 2, y, c);

        // A marker on each reel in the group. The line alone runs straight
        // past a reel that is not in it, which on TWO PAIR is exactly the
        // reel a player is asking about.
        for (int d = 0; d < jr::k_drums; d++) {
            if (world.group_of[d] != g) continue;
            int l, r;
            drum_window(d, l, r);
            const int cx = (l + r) / 2;
            fill(screen, cx - 3, y - 3, cx + 3, y + 3, c);
            fill(screen, cx - 1, y - 1, cx + 1, y + 1, k_ink);
        }
    }
}

void render_title_panel(const jr::World& w, const pse::RenderTarget& s);
void render_swap_panel(const jr::World& w, const pse::RenderTarget& s);

void draw_drums(const jr::World& world) {
    for (int d = 0; d < jr::k_drums; d++) {
        const int32_t angle = world.angle[d];
        const int blur = world.spinning[d] ? jr::speed_blur(world.speed) : 0;
        const bool opened = world.state == jr::kSwap && d == world.swap_drum;
        const int open_face = world.swap_face % jr::k_facets;

        for (int f = 0; f < jr::k_facets; f++) {
            const float a = facet_angle(angle, f);
            // Positive pitch takes the model's +Z toward +Y, so a panel whose
            // normal is -Z ends up at (0, -sin a, -cos a) and its centre sits
            // that far along its own normal from the axis.
            const float y = -k_drum_radius * std::sin(a);
            const float z = -k_drum_radius * std::cos(a);
            const uint8_t shade = facet_tint(a);

            if (blur >= 2) {
                // WILD does not draw the symbol at all, which is what buys the
                // +5 mult. Untextured, so it costs no texture fetch either.
                g_renderer.draw_mesh(models::jokerreels::facet,
                                     k_drum_x[d], y, z, 0.0f, k_facet_size,
                                     k_blur_face.r, k_blur_face.g,
                                     k_blur_face.b, a, 0, 0.0f, 0);
                continue;
            }

            uint8_t tr = shade, tg = shade, tb = shade;
            if (opened && f == open_face) {
                // The face being edited, tinted through the texture's white
                // background so the whole facet reads as selected.
                tr = 255; tg = 236; tb = 39;
            } else if (blur == 1) {
                // FAIR still draws it, too dark to read comfortably. That is
                // what "you can time a stop but not read it" looks like.
                tr = static_cast<uint8_t>(shade * 2 / 5);
                tg = tr;
                tb = static_cast<uint8_t>(shade * 3 / 5);
            }
            const uint8_t tex = static_cast<uint8_t>(world.facet[d][f] + 1);
            g_renderer.draw_mesh(models::jokerreels::facet,
                                 k_drum_x[d], y, z, 0.0f, k_facet_size,
                                 tr, tg, tb, a, 0, 0.0f, tex);
        }

        // The flat ends. Twelve sided like the drum, stood up with a quarter
        // turn of yaw so each faces outward, and rolled with the drum so the
        // cap's corners meet the facets' rather than sliding against them.
        const float roll = facet_angle(angle, 0) - k_pi / jr::k_facets;
        const float half = k_facet_size * 0.5f;
        for (int side = 0; side < 2; side++) {
            const float sx = k_drum_x[d] + (side ? half : -half);
            const float yaw = side ? radians(-90.0f) : radians(90.0f);
            g_renderer.draw_mesh(models::jokerreels::cap, sx, 0.0f, 0.0f,
                                 yaw, k_drum_radius, 255, 255, 255, 0.0f, 0,
                                 roll, 0);
        }
    }
}

/* The window the drums are seen through, in 2D over the 3D band.
 *
 * A cabinet built in 3D needs side walls, and side walls project into two dark
 * wedges that read as wings. It is also the wrong tool: the frame is flat on
 * to the camera and never moves, so it is fill_rect work rather than triangle
 * work, and it costs no geometry at all.
 */
void draw_bezel(const pse::RenderTarget& screen) {
    int bars[2 * (jr::k_drums + 1)];
    int n = 0;
    int left, right;
    bars[n++] = 0;
    for (int d = 0; d < jr::k_drums; d++) {
        drum_window(d, left, right);
        bars[n++] = left - 1;
        bars[n++] = right + 1;
    }
    bars[n++] = k_screen_w - 1;

    const int mid = k_window_h / 2;
    for (int i = 0; i < n; i += 2) {
        if (bars[i] > bars[i + 1]) continue;
        fill(screen, bars[i], 0, bars[i + 1], k_window_h - 1, k_frame);
        // A chrome edge down each side of every opening, which is what makes
        // the gap read as a frame in front of the drums rather than a hole
        // between them.
        if (bars[i] > 0) pse::v_line(screen, bars[i], 0, k_window_h - 1,
                                     k_chrome.r, k_chrome.g, k_chrome.b);
        if (bars[i + 1] < k_screen_w - 1) {
            pse::v_line(screen, bars[i + 1], 0, k_window_h - 1,
                        k_chrome.r, k_chrome.g, k_chrome.b);
        }
        // The payline: a tick into the frame either side of every window, at
        // the height a drum comes to rest. It names the face being read
        // without a pixel of it crossing a symbol.
        hline(screen, bars[i], bars[i + 1], mid - 1, k_payline);
        hline(screen, bars[i], bars[i + 1], mid, k_payline);
        hline(screen, bars[i], bars[i + 1], mid + 1, k_payline_dim);
    }

    fill(screen, 0, 0, k_screen_w - 1, k_bezel_top - 1, k_frame);
    fill(screen, 0, k_bezel_bottom, k_screen_w - 1, k_window_h - 1, k_frame);
    hline(screen, 0, k_screen_w - 1, k_bezel_top - 1, k_chrome);
    hline(screen, 0, k_screen_w - 1, k_bezel_bottom, k_chrome);
}

void render_title_panel(const jr::World& world, const pse::RenderTarget& s) {
    (void)world;
    text_centred(s, "JOKER REELS", k_screen_w / 2, k_window_h + 22, k_select, 2);
    text_centred(s, "EIGHT ANTES, FIVE SPINS EACH", k_screen_w / 2,
                 k_window_h + 56, k_dim);
    text_centred(s, "A DRUM LANDS ON WHAT YOU PUT ON IT", k_screen_w / 2,
                 k_window_h + 70, k_dim);
}

void render_swap_panel(const jr::World& w, const pse::RenderTarget& s) {
    const int top = k_window_h;
    text_centred(s, "OPEN A DRUM", k_screen_w / 2, top + 4, k_select);

    char* out = put_str(g_line, "DRUM ");
    out = put_int(out, w.swap_drum + 1);
    out = put_str(out, "  SYMBOL ");
    out = put_int(out, w.swap_face + 1);
    *out++ = '/';
    out = put_int(out, w.strip_len[w.swap_drum]);
    *out = '\0';
    text(s, g_line, 6, top + 18, k_dim);

    const uint8_t now = w.strip[w.swap_drum][w.swap_face];
    const uint8_t to = w.swap_to;
    text(s, "NOW", 6, top + 36, k_dim);
    text(s, jr::symbol_name(now), 6, top + 48, k_paper);
    text(s, num_line("", jr::symbol_chips(now), " CHIPS"), 6, top + 60, k_dim);

    text_centred(s, "TO", k_screen_w / 2, top + 48, k_paper);

    text_right(s, "NEW", 234, top + 36, k_dim);
    text_right(s, jr::symbol_name(to), 234, top + 48, k_select);
    text_right(s, num_line("", jr::symbol_chips(to), " CHIPS"), 234, top + 60,
               k_chip);

    text(s, "A DRUM LANDS ONLY ON WHAT IS ON IT", 6, top + 106, k_dim);
}

}  // namespace

void drum_window(int drum, int& left, int& right) {
    // Projected, not measured off a screenshot, so moving a drum moves its
    // window with it. The front face is the widest the drum ever projects: a
    // cylinder tapers with depth, so anything further back is narrower.
    const float depth = k_cam_dist - k_drum_radius;
    const float focal = 1.0f / std::tan(radians(k_fov_degrees) * 0.5f);
    const float scale = focal * (k_screen_w - 1) * 0.5f / depth;
    const float half = k_facet_size * 0.5f;
    const float centre = k_screen_w * 0.5f + k_drum_x[drum] * scale;
    left = static_cast<int>(centre - half * scale + 0.5f);
    right = static_cast<int>(centre + half * scale + 0.5f);
}

/* How to play, and how a hand becomes a score.
 *
 * Rule 9 keeps text off the screen by default and says in as many words that
 * an explicit request for more text wins. This is that request, and it is a
 * fair one: a scoring system nobody can see is not sparse, it is opaque. Three
 * pages, shown once on the way out of the title and reachable again from
 * between spins, which is the moment a player wants to know what they are
 * aiming at.
 *
 * Any button turns the page. Nothing on screen names one, so no press is the
 * wrong guess, which is the half of rule 9 that still applies.
 */
void render_learn(const jr::World& w, const pse::RenderTarget& s) {
    fill(s, 0, 0, k_screen_w - 1, k_screen_h - 1, k_ink);
    fill(s, 0, 0, k_screen_w - 1, 22, k_panel);
    hline(s, 0, k_screen_w - 1, 23, k_dim);

    const int page = w.learn_page < jr::k_learn_pages ? w.learn_page : 0;

    if (page == 0) {
        text_centred(s, "HOW TO SCORE", k_screen_w / 2, 8, k_paper);
        text(s, "FIVE REELS STOP ON FIVE SYMBOLS.", 8, 34, k_paper);
        text(s, "WHAT THEY MAKE IS A HAND.", 8, 46, k_paper);

        text(s, "A HAND IS CHIPS AND A MULT.", 8, 68, k_dim);
        // The worked example, using the hand a player lands most.
        fill(s, 8, 82, k_screen_w - 9, 118, k_panel);
        pse::draw_rect(s, 8, 82, k_screen_w - 16, 37, k_dim.r, k_dim.g,
                       k_dim.b);
        text(s, "TWO PAIR", 14, 88, k_select);
        // Laid out left to right by measuring each piece as it is placed. The
        // first version positioned the CHIPS label at the width of the literal
        // "45", which is correct for exactly one value of one hand at one
        // level, and every one of those three can change.
        {
            const int level = w.hand_level[jr::kTwoPair];
            const int chips = jr::hand_chips(jr::kTwoPair, level);
            const int mult = jr::hand_mult(jr::kTwoPair, level);
            int x = 14;
            text(s, num_line("", chips, ""), x, 102, k_chip);
            x += pse::text_width(g_line) + 5;
            text(s, "CHIPS", x, 102, k_dim);
            x += pse::text_width("CHIPS") + 7;
            text(s, "X", x, 102, k_paper);
            x += pse::text_width("X") + 7;
            text(s, num_line("", mult, ""), x, 102, k_mult);
            x += pse::text_width(g_line) + 5;
            text(s, "MULT", x, 102, k_dim);
            text_right(s, num_line("", chips * mult, ""), k_screen_w - 16, 102,
                       k_payline);
        }

        text(s, "EACH SYMBOL ADDS ITS OWN CHIPS", 8, 132, k_dim);
        text(s, "WHILE THE HAND COUNTS.", 8, 144, k_dim);
        text(s, "BANK THE ANTE'S TARGET IN", 8, 168, k_paper);
        text(s, "FIVE SPINS, EIGHT TIMES OVER.", 8, 180, k_paper);
    } else if (page == 1) {
        text_centred(s, "HANDS", k_screen_w / 2, 8, k_paper);
        text_right(s, "CHIPS   MULT", k_screen_w - 8, 8, k_dim);
        for (int h = 0; h < jr::k_hands; h++) {
            const int y = 32 + h * 25;
            const uint8_t which = static_cast<uint8_t>(h);
            const int level = w.hand_level[h];
            fill(s, 6, y, k_screen_w - 7, y + 21, (h & 1) ? k_ink : k_panel);
            text(s, jr::hand_name(which), 10, y + 3, k_paper);
            if (level > 1) {
                text(s, num_line("LV", level, ""), 10, y + 13, k_good);
            }
            text_right(s, num_line("", jr::hand_chips(which, level), ""), 194,
                       y + 8, k_chip);
            text_right(s, num_line("", jr::hand_mult(which, level), ""),
                       k_screen_w - 10, y + 8, k_mult);
        }
    } else {
        text_centred(s, "THE DIAL AND THE JOKERS", k_screen_w / 2, 8, k_paper);
        text(s, "THE SPEED DIAL IS THE RISK.", 8, 34, k_paper);
        for (int i = 0; i < jr::k_speeds; i++) {
            const int y = 52 + i * 16;
            const uint8_t which = static_cast<uint8_t>(i);
            fill(s, 8, y, 50, y + 12, k_panel);
            text_centred(s, jr::speed_name(which), 29, y + 3, k_paper);
            const int m = jr::speed_mult(which);
            text(s, m > 0 ? num_line("+", m, " MULT A REEL YOU STOP")
                          : "READABLE, AND WORTH NOTHING",
                 58, y + 3, m > 0 ? k_payline : k_good);
        }
        text(s, "STOPPING A REEL IS OPTIONAL.", 8, 112, k_dim);
        text(s, "LEAVE THEM AND THEY STOP", 8, 124, k_dim);
        text(s, "THEMSELVES, FOR NOTHING.", 8, 136, k_dim);

        text(s, "JOKERS FIRE WHILE IT COUNTS.", 8, 160, k_paper);
        text(s, "OPENING A DRUM CHANGES WHAT", 8, 184, k_paper);
        text(s, "THAT REEL CAN EVER LAND ON.", 8, 196, k_paper);
    }

    // Which page, as dots. Three characters of text would say the same thing
    // and take a line to do it.
    for (int i = 0; i < jr::k_learn_pages; i++) {
        const int cx = k_screen_w / 2 - (jr::k_learn_pages - 1) * 5 + i * 10;
        const Rgb c = i == page ? k_paper : k_dim;
        fill(s, cx - 2, k_screen_h - 10, cx + 2, k_screen_h - 6, c);
    }
}

const Stats& stats() { return g_stats; }

// Six characters, which is 35 px of a 39 px slot. Truncated rather than
// shrunk: a name at half scale is a name nobody can read.
const char* joker_slot_name(uint8_t joker) {
    return short_name(jr::joker_name(joker), 6);
}

void render_machine(const jr::World& world, const pse::RenderTarget& screen) {
    const pse::RenderTarget window = window_of(screen);

    g_raster.set_textures(k_textures, jr::k_symbols);
    g_raster.begin_frame_collect(window, g_queue);
    // Square pixels: with a viewport band the engine scales the vertical by
    // the target's WIDTH, so a 112 row band is a crop of the view rather than
    // a squash of it.
    g_renderer.set_viewport(0, k_window_h);
    g_renderer.set_fov(k_fov_degrees);
    /* The near plane is much nearer than the nearest thing in the scene, and
     * that is not slack, it is the engine's actual clip.
     *
     * Renderer3D::project rejects a point whose NDC depth is <= 0, and NDC
     * depth reaches 0 at the HARMONIC mean of the range, 2fn/(f+n), not at the
     * near plane. Setting near just in front of the drums put that crossover
     * at 34.8 and the front of every drum is at 25.8, so the entire face of
     * the machine was clipped and only its back half drew: fourteen triangles
     * of a hundred and forty four, and a green box with a couple of slivers
     * in it. 10 and 92 put the crossover at 18.0, well in front of anything.
     */
    g_renderer.set_depth_range(10.0f, k_cam_dist + k_drum_radius + 18.0f);
    g_renderer.set_camera(0.0f, 0.0f, -k_cam_dist, 0.0f, 0.0f);

    draw_drums(world);

    // Core 1 takes the bottom half of the band. Both cores own disjoint rows
    // of the framebuffer and of the depth buffer, so there is no lock and
    // nothing to synchronise.
    pse::run_split(g_raster, g_queue,
                   pse::SkyGradient{k_ink.r, k_ink.g, k_ink.b,
                                    k_cabinet.r, k_cabinet.g, k_cabinet.b});
    g_raster.end_collect();

    g_stats.triangles = g_raster.triangles_drawn();
    g_stats.queued = g_queue.count;
    g_stats.dropped = g_queue.dropped;

    draw_bezel(screen);
    draw_match_lines(world, screen);
    if (world.flash > 0) {
        pse::draw_rect(screen, 0, 0, k_screen_w, k_window_h, 255, 255, 255);
    }
}

void render_panel(const jr::World& world, const pse::RenderTarget& screen) {
    const int top = k_window_h;
    fill(screen, 0, top, k_screen_w - 1, k_screen_h - 1, k_ink);
    hline(screen, 0, k_screen_w - 1, top, k_dim);

    if (world.state == jr::kSwap) { render_swap_panel(world, screen); return; }
    if (world.state == jr::kTitle) { render_title_panel(world, screen); return; }

    // Ante, bank against target, spins, gold. Four numbers, which is what a
    // run needs and no more.
    text(screen, num_line("ANTE ", world.ante, "/8"), 4, top + 4, k_paper);
    text_right(screen, pair_line(world.banked, "/", world.target), 176,
               top + 4, world.banked >= world.target ? k_good : k_dim);
    text_right(screen, num_line("G", world.gold, ""), 236, top + 4, k_select);
    text_right(screen, num_line("SPINS ", world.spins, ""), 236, top + 14,
               world.spins > 1 ? k_dim : k_warn);

    // Progress toward the ante's target.
    const int bar_w = k_screen_w - 8;
    fill(screen, 4, top + 26, 4 + bar_w, top + 30, k_panel);
    if (world.target > 0 && world.banked > 0) {
        int32_t filled = static_cast<int32_t>(
            static_cast<int64_t>(bar_w) * world.banked / world.target);
        if (filled > bar_w) filled = bar_w;
        fill(screen, 4, top + 26, 4 + filled, top + 30,
             world.banked >= world.target ? k_good : k_warn);
    }

    // Chips and mult, separately and large. Watching one of them grow is the
    // whole feel this is borrowing, and a single total would throw it away.
    fill(screen, 4, top + 35, 112, top + 57, k_panel);
    pse::draw_rect(screen, 4, top + 35, 109, 23, k_dim.r, k_dim.g, k_dim.b);
    text(screen, num_line("", world.chips, ""), 8, top + 39, k_chip, 2);
    text(screen, "X", 62, top + 42, k_paper);
    text(screen, num_line("", world.mult, ""), 74, top + 39, k_mult, 2);
    text(screen, num_line("SCORE ", world.chips * world.mult, ""), 4,
         top + 61, k_payline);

    // The speed dial: the only thing you set before pulling, and the whole
    // risk of the game on one control.
    text(screen, "SPEED", 120, top + 35, k_dim);
    for (int i = 0; i < jr::k_speeds; i++) {
        const bool on = i == world.speed;
        const int bx = 120 + i * 39;
        fill(screen, bx, top + 45, bx + 35, top + 57, on ? k_payline : k_panel);
        pse::draw_rect(screen, bx, top + 45, 36, 13,
                       on ? k_select.r : k_dim.r, on ? k_select.g : k_dim.g,
                       on ? k_select.b : k_dim.b);
        text_centred(screen, jr::speed_name(static_cast<uint8_t>(i)),
                     bx + 18, top + 48, on ? k_ink : k_dim);
    }
    const int bonus = jr::speed_mult(world.speed);
    text_right(screen, bonus > 0 ? num_line("+", bonus, " MULT") : "READABLE",
               236, top + 61, bonus > 0 ? k_payline : k_good);

    // One tally line at a time. The count is the animation.
    if (world.tally_step > 0 && world.tally_step <= world.tally_len) {
        const jr::TallyEntry& e = world.tally[world.tally_step - 1];
        text(screen, e.what, 4, top + 73, e.joker ? k_select : k_paper);
        const int x = 4 + pse::text_width(e.what) + 6;
        if (e.mult == -1) {
            text(screen, "X2 MULT", x, top + 73, k_mult);
        } else if (e.chips) {
            text(screen, num_line("+", e.chips, " CHIPS"), x, top + 73, k_chip);
        } else if (e.mult) {
            text(screen, num_line("+", e.mult, " MULT"), x, top + 73, k_mult);
        }
    }

    // Five joker slots, showing what you have rather than what it does. What
    // a joker does is on its shop card and in the tally when it fires, which
    // is the two moments you need it.
    for (int i = 0; i < jr::k_max_jokers; i++) {
        const int bx = 4 + i * 46;
        const bool has = i < world.joker_count;
        fill(screen, bx, top + 85, bx + 42, top + 105, has ? k_panel : k_ink);
        pse::draw_rect(screen, bx, top + 85, 43, 21,
                       has ? 0xC2 : k_dim.r, has ? 0xC3 : k_dim.g,
                       has ? 0xC7 : k_dim.b);
        if (!has) continue;
        text(screen, joker_slot_name(world.jokers[i]), bx + 3, top + 92,
             k_select);
    }

    if (world.msg) {
        text_centred(screen, world.msg, k_screen_w / 2, top + 113, k_good);
    } else if (world.state == jr::kCount) {
        text_centred(screen, jr::hand_name(world.hand_index), k_screen_w / 2,
                     top + 113, k_select);
    }
}

/* The two screens that are otherwise all text.
 *
 * Their backgrounds live here rather than in game.cpp for the same reason
 * everything else does: this file is compiled by the host preview harness and
 * that one is not, so a rectangle drawn here is looked at before it ships and
 * a rectangle drawn there is not. It also keeps game.cpp to the one thing only
 * the SDK can do, which is text.
 */
void render_shop(const jr::World& world, const pse::RenderTarget& screen) {
    fill(screen, 0, 0, k_screen_w - 1, k_screen_h - 1, k_ink);
    fill(screen, 0, 0, k_screen_w - 1, 28, k_panel);
    hline(screen, 0, k_screen_w - 1, 29, k_dim);
    text_centred(screen, "THE BACK ROOM", k_screen_w / 2, 5, k_paper);
    text(screen, num_line("GOLD ", world.gold, ""), 6, 17, k_select);
    text_right(screen, num_line("ANTE ", world.ante, " CLEAR"), 234, 17,
               k_good);

    for (int i = 0; i < world.shop_len; i++) {
        const jr::ShopItem& item = world.shop[i];
        const int y = 34 + i * 42;
        const bool sel = i == world.shop_sel;
        fill(screen, 6, y, k_screen_w - 7, y + 37, sel ? k_panel : k_ink);
        pse::draw_rect(screen, 6, y, k_screen_w - 12, 38,
                       sel ? 0xC2 : k_dim.r, sel ? 0xC3 : k_dim.g,
                       sel ? 0xC7 : k_dim.b);

        const char* title = "OPEN A DRUM";
        const char* body = "SWAP ONE FACE FOR ANY SYMBOL";
        if (item.kind == jr::kShopJoker) {
            title = jr::joker_name(item.which);
            body = jr::joker_text(item.which);
        } else if (item.kind == jr::kShopHand) {
            title = jr::hand_name(item.which);
            body = "LEVEL IT UP";
        }
        text(screen, title, 12, y + 8, item.sold ? k_dim : k_paper);
        text(screen, body, 12, y + 22, k_dim);
        text_right(screen, item.sold ? "SOLD" : num_line("G", item.cost, ""),
                   228, y + 8,
                   item.sold ? k_dim
                             : (world.gold >= item.cost ? k_select : k_warn));
    }

    const int y = 34 + world.shop_len * 42;
    const bool sel = world.shop_sel >= world.shop_len;
    fill(screen, 70, y, 169, y + 17, sel ? k_cabinet : k_panel);
    pse::draw_rect(screen, 70, y, 100, 18,
                   sel ? k_good.r : k_dim.r, sel ? k_good.g : k_dim.g,
                   sel ? k_good.b : k_dim.b);
    text_centred(screen, "NEXT ANTE", 120, y + 6, sel ? k_paper : k_dim);
}

void render_end(const jr::World& world, const pse::RenderTarget& screen) {
    fill(screen, 0, 0, k_screen_w - 1, k_screen_h - 1, k_ink);
    const bool won = world.state == jr::kWin;
    const Rgb bar = won ? k_cabinet : Rgb{0x7E, 0x25, 0x53};
    fill(screen, 0, 48, k_screen_w - 1, 76, bar);
    hline(screen, 0, k_screen_w - 1, 47, won ? k_good : k_warn);
    hline(screen, 0, k_screen_w - 1, 77, won ? k_good : k_warn);

    text_centred(screen, won ? "YOU BROKE THE BANK" : "OUT OF SPINS",
                 k_screen_w / 2, 58, k_paper);
    text_centred(screen, num_line("ANTE ", world.ante, " OF 8"),
                 k_screen_w / 2, 90, k_paper);
    text_centred(screen, pair_line(world.banked, " OF ", world.target),
                 k_screen_w / 2, 104, k_dim);
    int y = 130;
    for (int i = 0; i < world.joker_count; i++) {
        text_centred(screen, jr::joker_name(world.jokers[i]), k_screen_w / 2, y,
                     k_select);
        y += 12;
    }
}

// One entry point, so game.cpp is input and a call. Which screen is showing is
// a rendering decision and it belongs on this side of the line.
void render_frame(const jr::World& world, const pse::RenderTarget& screen) {
    if (world.state == jr::kLearn) { render_learn(world, screen); return; }
    if (world.state == jr::kShop) { render_shop(world, screen); return; }
    if (world.state == jr::kOver || world.state == jr::kWin) {
        render_end(world, screen);
        return;
    }
    render_machine(world, screen);
    render_panel(world, screen);
}

}  // namespace jrr
