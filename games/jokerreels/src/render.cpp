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
#include "jokerreels/extras.hpp"
#include "jokerreels/facet.hpp"
#include "jokerreels/hands.hpp"
#include "jokerreels/items.hpp"
#include "jokerreels/jokers.hpp"
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

/* A joker's icon, cut out of the one sheet.
 *
 * The cell IS the enum value. Eight separate sprites would need a table here
 * listing them in the Joker order, which is a fourth place that order is
 * written down and a fourth place it can go wrong; with a sheet there is
 * nothing to misorder, only an offset.
 *
 * The HUD slot carries the picture and no name. It used to carry six
 * characters of one, which is what a 43 px box holds, and GREASER, RATCHET,
 * COLLECTOR, METRONOME and UNDERSTUDY all arrive at that box too long: a row
 * of truncations is a row of words a player has to decode rather than
 * recognise. A silhouette is read at a glance and does not get shorter.
 */
void draw_cell(const pse::RenderTarget& t, const pse::Sprite& sheet,
               int index, int x, int y) {
    pse::blit_sprite(t, sheet, index * k_joker_icon, 0, k_joker_icon,
                     k_joker_icon, x, y);
}

void draw_joker(const pse::RenderTarget& t, uint8_t joker, int x, int y) {
    draw_cell(t, models::jokerreels::jokers, joker % jr::k_jokers, x, y);
}

void draw_item(const pse::RenderTarget& t, uint8_t item, int x, int y) {
    draw_cell(t, models::jokerreels::items, item % jr::k_items, x, y);
}

// A hand's icon is its PATTERN: five bars, the reels that matched standing
// tall in the group's colour and the ones that took no part sitting low in
// grey. Generated from the hand rather than drawn, so the picture beside
// TWO PAIR cannot be three of a kind's.
void draw_hand(const pse::RenderTarget& t, uint8_t hand, int x, int y) {
    draw_cell(t, models::jokerreels::hands, hand % jr::k_hands, x, y);
}

// The two shop rows that are not a thing you own.
enum Extra : uint8_t { kExtraSwap = 0, kExtraReroll };

void draw_extra(const pse::RenderTarget& t, uint8_t extra, int x, int y) {
    draw_cell(t, models::jokerreels::extras, extra, x, y);
}

// Whatever a shop row is a picture of. Every kind has one: a card with a
// gutter where the others have art reads as a card that failed to load rather
// than as a card of a different sort.
void draw_shop_icon(const pse::RenderTarget& t, const jr::ShopItem& item,
                    int x, int y) {
    switch (item.kind) {
        case jr::kShopJoker: draw_joker(t, item.which, x, y); break;
        case jr::kShopHand:  draw_hand(t, item.which, x, y); break;
        case jr::kShopItem:  draw_item(t, item.which, x, y); break;
        default:             draw_extra(t, kExtraSwap, x, y); break;
    }
}

/* The line that says WHY a hand scored.
 *
 * One at a time, and the one the count is paying for right now. Five paylines
 * drawn at once is a cat's cradle; one drawn while its own tally entry is on
 * screen is a player being told, in order, what each shape was worth.
 *
 * The path is the payline's own rows, so a V goes through the top of the outer
 * reels, the payline row of reels two and four, and the bottom of the middle.
 * Markers go on the cells that actually made the hand, which on TWO PAIR is
 * four of the five.
 */
void draw_payline(const jr::World& world, const pse::RenderTarget& screen) {
    if (world.state != jr::kCount) return;
    if (world.tally_step == 0 || world.tally_step > world.tally_len) return;
    const jr::TallyEntry& e = world.tally[world.tally_step - 1];
    if (e.line == jr::k_no_line) return;

    const uint8_t* rows = jr::payline_rows(e.line);
    uint8_t symbols[jr::k_drums];
    jr::line_symbols(world, e.line, symbols);
    uint8_t groups[jr::k_drums];
    jr::hand_groups(symbols, groups);

    const Rgb c = k_group_colour[e.line % jr::k_max_groups];

    int px = 0, py = 0;
    for (int d = 0; d < jr::k_drums; d++) {
        int left, right, cy, h;
        drum_window(d, left, right);
        row_band(rows[d], cy, h);
        const int cx = (left + right) / 2;

        // The joining line. Straight between cell centres, which on a diagonal
        // is a real diagonal and not a staircase of horizontals.
        if (d > 0) pse::draw_line(screen, px, py, cx, cy, c.r, c.g, c.b);
        px = cx;
        py = cy;
    }

    // Markers second, so a line never draws over one.
    for (int d = 0; d < jr::k_drums; d++) {
        int left, right, cy, h;
        drum_window(d, left, right);
        row_band(rows[d], cy, h);
        const int cx = (left + right) / 2;
        const bool made_it = groups[d] != jr::k_no_group;
        const int r = made_it ? 4 : 2;
        fill(screen, cx - r, cy - r, cx + r, cy + r, c);
        fill(screen, cx - r + 2, cy - r + 2, cx + r - 2, cy + r - 2, k_ink);
    }
}

void render_title_panel(const jr::World& w, const pse::RenderTarget& s);
void render_swap_panel(const jr::World& w, const pse::RenderTarget& s);

/* The equation, as named positions rather than as five literals.
 *
 * `chips X mult` is drawn on the panel and the joker pops are aimed at the
 * halves of it, so the two have to agree about where those halves are. This is
 * rule 9's measure-it rule applied to a layout rather than to a string: a pop
 * centred on a hand tuned x is over the right number only until somebody
 * nudges the box it belongs to.
 */
constexpr int k_score_l = 4;
constexpr int k_score_r = 112;
constexpr int k_chips_x = 8;
constexpr int k_times_x = 62;
constexpr int k_mult_x = 74;

/* Which joker is firing right now, and how far through its moment it is.
 *
 * The entry showing is tally[tally_step - 1], and count_wait is how long it
 * has been showing, so phase runs 0 to span across exactly the ticks the rules
 * hold that entry for. The rules own the duration (a joker is held longer than
 * a payline is), and asking them rather than keeping a copy is what stops the
 * animation running past its entry or finishing early.
 */
struct Firing {
    bool on;
    const jr::TallyEntry* entry;
    int phase;
    int span;
};

Firing firing_joker(const jr::World& w) {
    Firing f{false, nullptr, 0, 1};
    if (w.state != jr::kCount) return f;
    if (w.tally_step == 0 || w.tally_step > w.tally_len) return f;
    const jr::TallyEntry& e = w.tally[w.tally_step - 1];
    if (!e.joker || e.slot >= jr::k_max_jokers) return f;
    f.on = true;
    f.entry = &e;
    f.span = jr::tally_hold(w);
    if (f.span < 1) f.span = 1;
    f.phase = static_cast<int>(w.count_wait);
    if (f.phase > f.span) f.phase = f.span;
    return f;
}

// One period of a shake, and it is damped: biggest the instant the joker
// fires and gone by the time the count moves on, so the row is still by the
// time the next entry needs to be read.
const int8_t k_shake[8] = {0, 2, 3, 2, 0, -2, -3, -2};

int shake(const Firing& f, int offset) {
    const int amp = k_shake[(((f.phase + offset) / 2) & 7)];
    return amp * (f.span - f.phase) / f.span;
}

/* The number a joker just put into the score, over the half of the equation it
 * went into.
 *
 * Rising and drawn last, over whatever is underneath. A number that appeared
 * where the score already is would be one more thing in a box that is already
 * two numbers; one that lifts out of the box is read as having come FROM it,
 * which is the whole point: this is the answer to "why did the mult jump".
 *
 * Backed and outlined rather than drawn bare, because it crosses the progress
 * bar on its way up and a bare number over a bar is neither.
 *
 * It starts ABOVE the box rather than on it. Starting on it put a +120 in a
 * blue box exactly over the chips, so the one frame that explained why the
 * number changed was also the one frame you could not read the number in.
 */
void draw_pop(const pse::RenderTarget& s, const char* text, int centre,
              int base_y, const Firing& f, Rgb colour) {
    const int rise = 12 * f.phase / f.span;
    const int w = pse::text_width(text, 2);
    const int h = pse::text_height() * 2;
    const int x = centre - w / 2;
    const int y = base_y - rise;
    fill(s, x - 4, y - 3, x + w + 3, y + h + 2, k_ink);
    pse::draw_rect(s, x - 4, y - 3, w + 8, h + 6, colour.r, colour.g, colour.b);
    text_centred(s, text, centre, y, colour, 2);
}

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
        text(s, "FIVE REELS STOP, THREE ROWS DEEP.", 8, 34, k_paper);
        text(s, "FIVE LINES CROSS THE GRID, AND", 8, 46, k_paper);
        text(s, "EVERY LINE THAT MAKES A HAND PAYS.", 8, 58, k_paper);

        text(s, "A HAND IS CHIPS AND A MULT.", 8, 74, k_dim);
        // The worked example, using the hand a player lands most.
        fill(s, 8, 88, k_screen_w - 9, 124, k_panel);
        pse::draw_rect(s, 8, 88, k_screen_w - 16, 37, k_dim.r, k_dim.g,
                       k_dim.b);
        text(s, "TWO PAIR", 14, 94, k_select);
        // Laid out left to right by measuring each piece as it is placed. The
        // first version positioned the CHIPS label at the width of the literal
        // "45", which is correct for exactly one value of one hand at one
        // level, and every one of those three can change.
        {
            const int level = w.hand_level[jr::kTwoPair];
            const int chips = jr::hand_chips(jr::kTwoPair, level);
            const int mult = jr::hand_mult(jr::kTwoPair, level);
            int x = 14;
            text(s, num_line("", chips, ""), x, 108, k_chip);
            x += pse::text_width(g_line) + 5;
            text(s, "CHIPS", x, 108, k_dim);
            x += pse::text_width("CHIPS") + 7;
            text(s, "X", x, 108, k_paper);
            x += pse::text_width("X") + 7;
            text(s, num_line("", mult, ""), x, 108, k_mult);
            x += pse::text_width(g_line) + 5;
            text(s, "MULT", x, 108, k_dim);
            text_right(s, num_line("", chips * mult, ""), k_screen_w - 16, 108,
                       k_payline);
        }

        text(s, "THE BEST LINE SETS THE MULT.", 8, 138, k_dim);
        text(s, "EVERY OTHER PAYING LINE ADDS", 8, 150, k_dim);
        text(s, "ITS CHIPS TO THE SAME PILE.", 8, 162, k_dim);
        text(s, "BANK THE ANTE'S TARGET IN", 8, 186, k_paper);
        text(s, "FIVE SPINS, EIGHT TIMES OVER.", 8, 198, k_paper);
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
    } else if (page == 2) {
        text_centred(s, "PAYLINES", k_screen_w / 2, 8, k_paper);
        text(s, "A HAND CAN BE MADE ALONG ANY", 8, 30, k_dim);
        text(s, "OF FIVE LINES. ALL OF THEM PAY.", 8, 42, k_dim);

        // Each line as the grid it crosses. A picture of the shape, because
        // the shape is the thing: "0 1 2 1 0" is not a V to anybody.
        for (int line = 0; line < jr::k_lines; line++) {
            const int gx = 24 + (line % 2) * 112;
            const int gy = 62 + (line / 2) * 58;
            const uint8_t* rows = jr::payline_rows(static_cast<uint8_t>(line));
            const Rgb c = k_group_colour[line % jr::k_max_groups];

            for (int col = 0; col < jr::k_drums; col++) {
                for (int row = 0; row < jr::k_rows; row++) {
                    const int x = gx + col * 12;
                    const int y = gy + row * 12;
                    const bool on = rows[col] == row;
                    fill(s, x, y, x + 10, y + 10, on ? c : k_panel);
                }
            }
            // The path itself, joining cell centres, so a diagonal reads as a
            // diagonal rather than as a staircase.
            for (int col = 1; col < jr::k_drums; col++) {
                pse::draw_line(s, gx + (col - 1) * 12 + 5,
                               gy + rows[col - 1] * 12 + 5,
                               gx + col * 12 + 5, gy + rows[col] * 12 + 5,
                               k_paper.r, k_paper.g, k_paper.b);
            }
            text(s, jr::payline_name(static_cast<uint8_t>(line)), gx,
                 gy + 40, k_dim);
        }
    } else if (page == 3) {
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
        text(s, "THE ONE FIRING SHAKES, AND ITS", 8, 172, k_dim);
        text(s, "NUMBER POPS OVER THE SIDE OF", 8, 184, k_dim);
        text(s, "THE SUM IT WENT INTO.", 8, 196, k_dim);
        text(s, "OPENING A DRUM CHANGES ITS SYMBOLS.", 8, 214, k_paper);
    } else if (page == 4) {
        /* Every joker, its picture and what it does.
         *
         * This page is why the HUD row can be five pictures with no names
         * under them. A silhouette is only quicker to read than a word once
         * you have been shown it, and there is exactly one place in the game
         * with the room to show all eight at once.
         */
        text_centred(s, "JOKERS", k_screen_w / 2, 8, k_paper);
        for (int j = 0; j < jr::k_jokers; j++) {
            const uint8_t which = static_cast<uint8_t>(j);
            const int y = 30 + j * 24;
            fill(s, 4, y, k_screen_w - 5, y + 22, (j & 1) ? k_ink : k_panel);
            draw_joker(s, which, 6, y + 1);
            text(s, jr::joker_name(which), 32, y + 3, k_select);
            text(s, jr::joker_text(which), 32, y + 13, k_dim);
        }
    } else {
        // The consumables, and the two buttons that spend them. A joker is
        // what the run is; one of these is what you do about the spin in front
        // of you, and that difference is the whole reason they are a separate
        // page rather than six more rows on the last one.
        text_centred(s, "CONSUMABLES", k_screen_w / 2, 8, k_paper);
        for (int i = 0; i < jr::k_items; i++) {
            const uint8_t which = static_cast<uint8_t>(i);
            const int y = 28 + i * 24;
            fill(s, 4, y, k_screen_w - 5, y + 22, (i & 1) ? k_ink : k_panel);
            draw_item(s, which, 6, y + 1);
            text(s, jr::item_name(which), 32, y + 3, k_select);
            text(s, jr::item_text(which), 32, y + 13, k_dim);
        }
        text(s, "X SPENDS THE ONE PICKED,", 8, 180, k_paper);
        text(s, "Y PICKS THE OTHER. TWO SLOTS,", 8, 192, k_dim);
        text(s, "AND THE SHOP SELLS THEM.", 8, 204, k_dim);
    }

    // Which page, as dots. Three characters of text would say the same thing
    // and take a line to do it.
    for (int i = 0; i < jr::k_learn_pages; i++) {
        const int cx = k_screen_w / 2 - (jr::k_learn_pages - 1) * 5 + i * 10;
        const Rgb c = i == page ? k_paper : k_dim;
        fill(s, cx - 2, k_screen_h - 10, cx + 2, k_screen_h - 6, c);
    }
}

void row_band(int row, int& centre_y, int& height) {
    // The facet one step either side of the front one, projected. A row is not
    // a third of the window: the outer two are further away and turned away,
    // so they are both smaller and closer to the middle than a flat grid would
    // put them.
    const float step = 2.0f * k_pi / jr::k_facets;
    const float a = (row - 1) * step;
    const float wy = -k_drum_radius * std::sin(a);
    const float wz = -k_drum_radius * std::cos(a);
    const float depth = k_cam_dist + wz;
    const float focal = 1.0f / std::tan(radians(k_fov_degrees) * 0.5f);
    const float scale = focal * (k_screen_w - 1) * 0.5f / depth;
    centre_y = static_cast<int>(k_window_h * 0.5f - wy * scale + 0.5f);
    height = static_cast<int>(std::fabs(std::cos(a)) * k_facet_size * scale +
                              0.5f);
}

const Stats& stats() { return g_stats; }

// Five slots across the panel, laid out from the screen rather than typed:
// five boxes and the gaps between them come to 240 exactly, so a slot that
// grew would fail to fit here rather than run off the right edge on a device.
/* What a shop card says it is, as a function rather than as a chain inside the
 * drawing loop.
 *
 * It was that chain, and adding the joker icon to the top branch of it dropped
 * the branch underneath: the hand card fell through to the swap card's default
 * and the shop offered OPEN A DRUM twice, at two different prices, one of
 * which levelled a hand. The frame still looked exactly like a shop. Pulled
 * out here so the preview can ask each kind what it says and notice when two
 * of them say the same thing.
 */
const char* shop_title(const jr::ShopItem& item) {
    if (item.kind == jr::kShopJoker) return jr::joker_name(item.which);
    if (item.kind == jr::kShopHand) return jr::hand_name(item.which);
    if (item.kind == jr::kShopItem) return jr::item_name(item.which);
    return "OPEN A DRUM";
}

const char* shop_body(const jr::ShopItem& item) {
    if (item.kind == jr::kShopJoker) return jr::joker_text(item.which);
    if (item.kind == jr::kShopHand) return "LEVEL IT UP";
    if (item.kind == jr::kShopItem) return jr::item_text(item.which);
    return "SWAP ONE FACE FOR ANY SYMBOL";
}

// Seven slots across the panel, laid out from the screen rather than typed:
// five jokers, a gap wide enough to read as a divider, and two consumables,
// all of it inside 240 with room for the shake at either end.
namespace {
constexpr int k_slot_margin = 4;
constexpr int k_slot_pitch = 33;
constexpr int k_slot_row_y = k_window_h + 83;
constexpr int k_items_x = k_slot_margin + jr::k_max_jokers * k_slot_pitch + 7;
}  // namespace

void joker_slot(int index, int& x, int& y) {
    x = k_slot_margin + index * k_slot_pitch;
    y = k_slot_row_y;
}

void item_slot(int index, int& x, int& y) {
    x = k_items_x + index * k_slot_pitch;
    y = k_slot_row_y;
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
    draw_payline(world, screen);
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
    fill(screen, k_score_l, top + 35, k_score_r, top + 57, k_panel);
    pse::draw_rect(screen, k_score_l, top + 35, k_score_r - k_score_l - 3, 23,
                   k_dim.r, k_dim.g, k_dim.b);
    text(screen, num_line("", world.chips, ""), k_chips_x, top + 39, k_chip, 2);
    text(screen, "X", k_times_x, top + 42, k_paper);
    text(screen, num_line("", world.mult, ""), k_mult_x, top + 39, k_mult, 2);
    text(screen, num_line("SCORE ", world.chips * world.mult, ""), k_score_l,
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

    /* Five joker slots, each showing what it IS rather than what it does.
     *
     * What a joker does is on its shop card, on the instructions page, and in
     * the tally line when it fires, which are the three moments you need it.
     * The slot itself has 44 px and a picture is what fits in 44 px.
     *
     * The one that is firing shakes. It is the only reason a player has to
     * look at this row mid count: five slots that never move are furniture,
     * and one of them jumping is the game pointing at the thing that just
     * changed the number above it.
     */
    const Firing fire = firing_joker(world);
    for (int i = 0; i < jr::k_max_jokers; i++) {
        int bx, by;
        joker_slot(i, bx, by);
        const bool has = i < world.joker_count;
        const bool firing = fire.on && fire.entry->slot == i;
        if (firing) {
            bx += shake(fire, 0);
            by += shake(fire, 4) / 2;
        }
        fill(screen, bx, by, bx + k_slot_w - 1, by + k_slot_h - 1,
             has ? k_panel : k_ink);
        const Rgb edge = firing ? k_select : (has ? Rgb{0xC2, 0xC3, 0xC7}
                                                  : k_dim);
        pse::draw_rect(screen, bx, by, k_slot_w, k_slot_h,
                       edge.r, edge.g, edge.b);
        if (!has) continue;
        draw_joker(screen, world.jokers[i], bx + (k_slot_w - k_joker_icon) / 2,
                   by + (k_slot_h - k_joker_icon) / 2);
    }

    /* And the consumables, past a divider.
     *
     * Same size and same shape as a joker slot, because they answer the same
     * question: what am I holding. The divider is what says the two halves are
     * spent differently, a joker firing on its own and a consumable only when
     * you press X.
     *
     * The picked one is ringed, and only while there is more than one to pick
     * between: a ring round the single thing you own is decoration.
     */
    {
        int dx, dy;
        item_slot(0, dx, dy);
        // Two pixels of chrome, the full height of a slot. One dim pixel was
        // not a divider, it was a scratch: the row read as seven identical
        // boxes and nothing said the last two were spent by hand.
        fill(screen, dx - 6, dy, dx - 5, dy + k_slot_h - 1, k_chrome);
    }
    for (int i = 0; i < jr::k_max_items; i++) {
        int bx, by;
        item_slot(i, bx, by);
        const bool has = i < world.item_count;
        const bool picked = has && world.item_count > 1 && i == world.item_sel;
        fill(screen, bx, by, bx + k_slot_w - 1, by + k_slot_h - 1,
             has ? k_panel : k_ink);
        const Rgb edge = picked ? k_select : (has ? Rgb{0xC2, 0xC3, 0xC7}
                                                  : k_dim);
        pse::draw_rect(screen, bx, by, k_slot_w, k_slot_h,
                       edge.r, edge.g, edge.b);
        if (!has) continue;
        draw_item(screen, world.items[i], bx + (k_slot_w - k_joker_icon) / 2,
                  by + (k_slot_h - k_joker_icon) / 2);
    }

    if (world.msg) {
        text_centred(screen, world.msg, k_screen_w / 2, top + 113, k_good);
    } else if (world.state == jr::kCount) {
        text_centred(screen, jr::hand_name(world.hand_index), k_screen_w / 2,
                     top + 113, k_select);
    }

    /* And the number it put in, over the half of the equation it went into.
     *
     * Last, so it lifts over the bar and the score box rather than under them,
     * and on whichever side it actually touched: a joker can add chips, add
     * mult, or double the mult, and one of those three is not the other two.
     */
    if (fire.on) {
        if (fire.entry->chips != 0) {
            draw_pop(screen, num_line("+", fire.entry->chips, ""),
                     (k_chips_x + k_times_x) / 2, top + 19, fire, k_chip);
        }
        if (fire.entry->mult == -1) {
            draw_pop(screen, "X2", (k_mult_x + k_score_r) / 2, top + 19, fire,
                     k_mult);
        } else if (fire.entry->mult != 0) {
            draw_pop(screen, num_line("+", fire.entry->mult, ""),
                     (k_mult_x + k_score_r) / 2, top + 19, fire, k_mult);
        }
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
// Five cards and the reroll row, which is what 240 rows holds once the header
// has taken 30 of them. Named here so the preview can measure a card rather
// than assume one.
constexpr int k_card_y0 = 32;
constexpr int k_card_h = 32;
constexpr int k_card_pitch = 34;

void shop_card_icon(int index, int& x, int& y) {
    x = 10;
    y = k_card_y0 + index * k_card_pitch + 6;
}

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
        const int y = k_card_y0 + i * k_card_pitch;
        const bool sel = i == world.shop_sel;
        fill(screen, 6, y, k_screen_w - 7, y + k_card_h - 1,
             sel ? k_panel : k_ink);
        pse::draw_rect(screen, 6, y, k_screen_w - 12, k_card_h,
                       sel ? 0xC2 : k_dim.r, sel ? 0xC3 : k_dim.g,
                       sel ? 0xC7 : k_dim.b);

        // EVERY card carries a picture, whatever kind it is. It was jokers
        // only, and a card with a gutter where the others have art reads as
        // one that failed to load rather than as one of a different sort.
        // A hand's icon is its own pattern, so the shelf teaches the shape it
        // is selling a level in.
        draw_shop_icon(screen, item, 10, y + 6);
        text(screen, shop_title(item), k_shop_text_x, y + 5,
             item.sold ? k_dim : k_paper);
        text(screen, shop_body(item), k_shop_text_x, y + 17, k_dim);
        text_right(screen, item.sold ? "SOLD" : num_line("G", item.cost, ""),
                   228, y + 5,
                   item.sold ? k_dim
                             : (world.gold >= item.cost ? k_select : k_warn));
    }

    /* The two rows that are not a thing to buy.
     *
     * REROLL beside NEXT ANTE, both selectable, both part of the same one
     * dimensional list the cards are in: pressing down off the last card
     * reaches the reroll and pressing down again reaches the way out. A
     * separate axis for "the buttons" would be a second thing to learn on a
     * screen that already has one.
     */
    const int y = k_card_y0 + world.shop_len * k_card_pitch + 2;
    const bool on_reroll = world.shop_sel == jr::shop_reroll_index(world);
    const bool on_next = world.shop_sel == jr::shop_next_index(world);
    const int cost = jr::reroll_cost(world);
    const bool afford = world.gold >= cost;

    fill(screen, 6, y, 114, y + 21, on_reroll ? k_panel : k_ink);
    pse::draw_rect(screen, 6, y, 109, 22,
                   on_reroll ? k_select.r : k_dim.r,
                   on_reroll ? k_select.g : k_dim.g,
                   on_reroll ? k_select.b : k_dim.b);
    draw_extra(screen, kExtraReroll, 9, y + 1);
    text(screen, "REROLL", 33, y + 7, on_reroll ? k_paper : k_dim);
    text_right(screen, num_line("G", cost, ""), 110, y + 7,
               afford ? k_select : k_warn);

    fill(screen, 126, y, 234, y + 21, on_next ? k_cabinet : k_panel);
    pse::draw_rect(screen, 126, y, 109, 22,
                   on_next ? k_good.r : k_dim.r, on_next ? k_good.g : k_dim.g,
                   on_next ? k_good.b : k_dim.b);
    text_centred(screen, "NEXT ANTE", 180, y + 7, on_next ? k_paper : k_dim);
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
    // What the run was built out of, icon beside name. Centred as one block
    // rather than as two, so the picture and the word it belongs to sit
    // together whatever the word is.
    int y = 126;
    for (int i = 0; i < world.joker_count; i++) {
        const char* name = jr::joker_name(world.jokers[i]);
        const int block = k_joker_icon + 5 + pse::text_width(name);
        const int x = (k_screen_w - block) / 2;
        draw_joker(screen, world.jokers[i], x, y - 6);
        text(screen, name, x + k_joker_icon + 5, y, k_select);
        y += 22;
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
