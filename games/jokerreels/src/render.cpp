#include "render.hpp"

#include <cmath>

#include "pse/draw2d.hpp"
#include "pse/parallel.hpp"
#include "pse/raster.hpp"
#include "pse/renderer3d.hpp"

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

const float k_drum_x[jr::k_drums] = {-k_drum_gap, 0.0f, k_drum_gap};

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
    int bars[8];
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

const Stats& stats() { return g_stats; }

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
    if (world.flash > 0) {
        pse::draw_rect(screen, 0, 0, k_screen_w, k_window_h, 255, 255, 255);
    }
}

void render_panel(const jr::World& world, const pse::RenderTarget& screen) {
    const int top = k_window_h;
    fill(screen, 0, top, k_screen_w - 1, k_screen_h - 1, k_ink);
    hline(screen, 0, k_screen_w - 1, top, k_dim);

    if (world.state == jr::kShop || world.state == jr::kOver ||
        world.state == jr::kWin) {
        return;                       // those screens are all text
    }

    // Progress toward the ante's target.
    const int bar_w = k_screen_w - 8;
    fill(screen, 4, top + 22, 4 + bar_w, top + 27, k_panel);
    if (world.target > 0 && world.banked > 0) {
        int32_t filled = static_cast<int32_t>(
            static_cast<int64_t>(bar_w) * world.banked / world.target);
        if (filled > bar_w) filled = bar_w;
        const Rgb c = world.banked >= world.target ? k_good : k_warn;
        fill(screen, 4, top + 22, 4 + filled, top + 27, c);
    }

    // The score box: chips and mult, separately and large, because watching
    // one of them grow is the whole feel this is borrowing. game.cpp writes
    // the numbers into it.
    fill(screen, 4, top + 32, 112, top + 58, k_panel);
    pse::draw_rect(screen, 4, top + 32, 109, 27, k_dim.r, k_dim.g, k_dim.b);

    // The speed dial, which is the only thing you set before pulling.
    for (int i = 0; i < jr::k_speeds; i++) {
        const bool on = i == world.speed;
        const int bx = 120 + i * 38;
        const Rgb bg = on ? Rgb{0xFF, 0xA3, 0x00} : k_panel;
        const Rgb edge = on ? k_select : k_dim;
        fill(screen, bx, top + 41, bx + 34, top + 55, bg);
        pse::draw_rect(screen, bx, top + 41, 35, 15, edge.r, edge.g, edge.b);
    }

    // Five joker slots. What a joker does is on its shop card and in the
    // tally when it fires, which is the two moments it matters.
    for (int i = 0; i < jr::k_max_jokers; i++) {
        const int bx = 4 + i * 46;
        const bool has = i < world.joker_count;
        fill(screen, bx, top + 82, bx + 42, top + 107, has ? k_panel : k_ink);
        const Rgb edge = has ? Rgb{0xC2, 0xC3, 0xC7} : k_dim;
        pse::draw_rect(screen, bx, top + 82, 43, 26, edge.r, edge.g, edge.b);
        if (!has) continue;
        const uint8_t j = world.jokers[i];
        const Rgb swatch = {static_cast<uint8_t>(60 + (j * 53) % 196),
                            static_cast<uint8_t>(90 + (j * 97) % 166),
                            static_cast<uint8_t>(120 + (j * 31) % 136)};
        fill(screen, bx + 3, top + 85, bx + 39, top + 95, swatch);
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
    for (int i = 0; i < world.shop_len; i++) {
        const int y = 34 + i * 42;
        const bool sel = i == world.shop_sel;
        fill(screen, 6, y, k_screen_w - 7, y + 37, sel ? k_panel : k_ink);
        pse::draw_rect(screen, 6, y, k_screen_w - 12, 38,
                       sel ? 0xC2 : k_dim.r, sel ? 0xC3 : k_dim.g,
                       sel ? 0xC7 : k_dim.b);
    }
    const int y = 34 + world.shop_len * 42;
    const bool sel = world.shop_sel >= world.shop_len;
    fill(screen, 70, y, 169, y + 17, sel ? k_cabinet : k_panel);
    pse::draw_rect(screen, 70, y, 100, 18,
                   sel ? k_good.r : k_dim.r, sel ? k_good.g : k_dim.g,
                   sel ? k_good.b : k_dim.b);
}

void render_end(const jr::World& world, const pse::RenderTarget& screen) {
    fill(screen, 0, 0, k_screen_w - 1, k_screen_h - 1, k_ink);
    const bool won = world.state == jr::kWin;
    const Rgb bar = won ? k_cabinet : Rgb{0x7E, 0x25, 0x53};
    fill(screen, 0, 48, k_screen_w - 1, 76, bar);
    hline(screen, 0, k_screen_w - 1, 47, won ? k_good : k_warn);
    hline(screen, 0, k_screen_w - 1, 77, won ? k_good : k_warn);
}

}  // namespace jrr
