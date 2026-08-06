#include "render.hpp"

#include <cmath>

#include "pse/parallel.hpp"
#include "pse/raster.hpp"
#include "pse/renderer3d.hpp"
#include "pse/shared_render.hpp"
#include "pse/text.hpp"

#include "tomlander/tom.hpp"

namespace tlr {
namespace {

using tl::World;

// The Rasterizer and the FrameQueue come from the engine rather than being
// declared here: on the console every game is linked into one binary, and a
// 14 KB depth buffer plus a 15 KB triangle queue per game is RAM spent on
// scenes nothing is rendering. A standalone build gets exactly one instance.
//   Rasterizer  ~14.4 KB (depth buffer)   shared
//   FrameQueue  ~15.4 KB (640 triangles)  shared
//   the rest    a few hundred bytes
pse::Rasterizer& g_raster = pse::shared_rasterizer();
pse::Renderer3D g_renderer(g_raster);
pse::FrameQueue& g_queue = pse::shared_queue();

FrameStats g_stats{};

constexpr float k_pi = 3.14159265f;

// The camera sits 45 degrees above the horizontal, which is the brief. Equal
// distance and height is what makes it exactly 45; the two move together.
constexpr float k_cam_reach = 16.3f;
constexpr float k_cam_fov = 58.0f;

// Bracketed to the scene rather than left at the engine default. The depth
// buffer is one byte and a perspective curve spends nearly all of it near the
// near plane, so a 0.25 to 400 range puts the ship and the ground it is
// standing on in the same step. See Renderer3D::set_depth_range.
constexpr float k_z_near = 6.0f;
constexpr float k_z_far = 170.0f;

// Model units to world units for the ship. Its bounding box is 9.5 across, so
// it flies about 5.9 world units wide.
constexpr float k_ship_scale = 0.62f;

// The ground, as cells around the ship. This was a checkerboard of small
// quads once: 12x12 of them is 288 triangles against the ship's 204, so over
// half the frame queue went on a flat pattern whose only job was to show that
// the ship was moving. Shading does that for free. Larger cells, fewer of
// them, each one lit off its own normal.
constexpr float k_cell = 20.0f;
constexpr int k_cells = 4;          // either side: 8x8 cells, 128 triangles

constexpr float k_fp16 = 1.0f / 65536.0f;

float to_f(int32_t fp) { return fp * k_fp16; }

// Nearest integer. A plain cast truncates toward zero, which biases a plume
// walking up and left by half a pixel per step against one walking down and
// right, and a flame that is symmetric in the maths is then not symmetric on
// screen.
int round_i(float v) {
    return static_cast<int>(v < 0.0f ? v - 0.5f : v + 0.5f);
}

struct Rgb { uint8_t r, g, b; };

Rgb lerp_rgb(Rgb a, Rgb b, int t256) {
    return Rgb{static_cast<uint8_t>(a.r + (b.r - a.r) * t256 / 256),
               static_cast<uint8_t>(a.g + (b.g - a.g) * t256 / 256),
               static_cast<uint8_t>(a.b + (b.b - a.b) * t256 / 256)};
}

// Ground colour by height. Per VERTEX, not per face, because ScreenTriangle
// already carries three colours and the rasterizer already interpolates them:
// a gradient across the landscape is free, and it is what stops eight metre
// cells reading as eight metre cells.
Rgb ground_colour(float height) {
    const Rgb low{18, 83, 89};        // basins, in shadow
    const Rgb mid{95, 87, 79};        // the general regolith
    const Rgb high{194, 195, 199};    // ridge tops catching the light
    if (height < 0.0f) {
        int t = static_cast<int>((height + 9.0f) * 256.0f / 9.0f);
        t = t < 0 ? 0 : (t > 256 ? 256 : t);
        return lerp_rgb(low, mid, t);
    }
    int t = static_cast<int>(height * 256.0f / 10.0f);
    t = t < 0 ? 0 : (t > 256 ? 256 : t);
    return lerp_rgb(mid, high, t);
}

// One ground triangle: three projected corners with their own colours.
void ground_tri(const int sx[3], const int sy[3], const int sz[3],
                const Rgb c[3]) {
    pse::ScreenTriangle tri;
    tri.x0 = static_cast<int16_t>(sx[0]);
    tri.y0 = static_cast<int16_t>(sy[0]);
    tri.z0 = static_cast<uint16_t>(sz[0]);
    tri.x1 = static_cast<int16_t>(sx[1]);
    tri.y1 = static_cast<int16_t>(sy[1]);
    tri.z1 = static_cast<uint16_t>(sz[1]);
    tri.x2 = static_cast<int16_t>(sx[2]);
    tri.y2 = static_cast<int16_t>(sy[2]);
    tri.z2 = static_cast<uint16_t>(sz[2]);
    tri.r0 = c[0].r; tri.g0 = c[0].g; tri.b0 = c[0].b;
    tri.r1 = c[1].r; tri.g1 = c[1].g; tri.b1 = c[1].b;
    tri.r2 = c[2].r; tri.g2 = c[2].g; tri.b2 = c[2].b;
    g_raster.draw(tri);
}

void draw_ground(const World& world) {
    const int base_x = static_cast<int>(std::floor(to_f(world.x) / k_cell));
    const int base_z = static_cast<int>(std::floor(to_f(world.z) / k_cell));

    for (int cz = base_z - k_cells; cz < base_z + k_cells; cz++) {
        for (int cx = base_x - k_cells; cx < base_x + k_cells; cx++) {
            const float x0 = cx * k_cell, x1 = x0 + k_cell;
            const float z0 = cz * k_cell, z1 = z0 + k_cell;
            const float wx[4] = {x0, x1, x1, x0};
            const float wz[4] = {z0, z0, z1, z1};

            int sx[4], sy[4], sz[4];
            float hy[4];
            Rgb col[4];
            bool visible = true;
            for (int i = 0; i < 4; i++) {
                hy[i] = to_f(tl::terrain_height(
                    world, static_cast<int32_t>(wx[i] * 65536.0f),
                    static_cast<int32_t>(wz[i] * 65536.0f)));
                col[i] = ground_colour(hy[i]);
                if (!g_renderer.project(wx[i], hy[i], wz[i],
                                        sx[i], sy[i], sz[i])) {
                    visible = false;
                    break;
                }
            }
            if (!visible) continue;

            // Alternate which way the cell splits. Cut every one the same way
            // and the shading lines up into stripes, and the surface reads as
            // a grid again, which is the thing being got rid of.
            const int a[2][3] = {{0, 1, 2}, {0, 2, 3}};
            const int b[2][3] = {{1, 2, 3}, {1, 3, 0}};
            const int (*split)[3] = ((cx + cz) & 1) ? b : a;
            for (int t = 0; t < 2; t++) {
                const int i0 = split[t][0], i1 = split[t][1], i2 = split[t][2];

                // Height alone made the landscape mush: without a slope term
                // a hillside is the same colour as the flat beside it, and at
                // twenty unit cells the gradient just reads as fog. So each
                // triangle gets a lambert off its own normal, applied to all
                // three of its vertex colours. The colours still vary per
                // vertex, so the shape reads AND the facet does.
                const float hx0 = wx[i0], hz0 = wz[i0];
                const float e1x = wx[i1] - hx0, e1z = wz[i1] - hz0;
                const float e2x = wx[i2] - hx0, e2z = wz[i2] - hz0;
                const float e1y = hy[i1] - hy[i0], e2y = hy[i2] - hy[i0];
                float nx = e1y * e2z - e1z * e2y;
                float ny = e1z * e2x - e1x * e2z;
                float nz = e1x * e2y - e1y * e2x;
                const float len = std::sqrt(nx * nx + ny * ny + nz * nz);
                if (len > 1e-6f) { nx /= len; ny /= len; nz /= len; }
                if (ny < 0.0f) { nx = -nx; ny = -ny; nz = -nz; }  // faces up
                float lambert = nx * 0.42f + ny * 0.82f + nz * -0.39f;
                if (lambert < 0.0f) lambert = 0.0f;
                const int shade256 =
                    static_cast<int>((0.52f + 0.48f * lambert) * 256.0f);

                Rgb tc[3] = {col[i0], col[i1], col[i2]};
                for (int c = 0; c < 3; c++) {
                    tc[c].r = static_cast<uint8_t>(tc[c].r * shade256 / 256);
                    tc[c].g = static_cast<uint8_t>(tc[c].g * shade256 / 256);
                    tc[c].b = static_cast<uint8_t>(tc[c].b * shade256 / 256);
                }
                const int tx[3] = {sx[i0], sx[i1], sx[i2]};
                const int ty[3] = {sy[i0], sy[i1], sy[i2]};
                const int tz[3] = {sz[i0], sz[i1], sz[i2]};
                ground_tri(tx, ty, tz, tc);
            }
        }
    }
}

// One deck quad with its own four corner colours.
void deck_quad(const float px[4], const float py, const float pz[4],
               const Rgb c[4]) {
    int sx[4], sy[4], sz[4];
    for (int i = 0; i < 4; i++) {
        if (!g_renderer.project(px[i], py, pz[i], sx[i], sy[i], sz[i])) return;
    }
    const int t0[3] = {0, 1, 2}, t1[3] = {0, 2, 3};
    const int* tris[2] = {t0, t1};
    for (int t = 0; t < 2; t++) {
        const int i0 = tris[t][0], i1 = tris[t][1], i2 = tris[t][2];
        const int qx[3] = {sx[i0], sx[i1], sx[i2]};
        const int qy[3] = {sy[i0], sy[i1], sy[i2]};
        const int qz[3] = {sz[i0], sz[i1], sz[i2]};
        const Rgb qc[3] = {c[i0], c[i1], c[i2]};
        ground_tri(qx, qy, qz, qc);
    }
}

// Grey steel, and cool rather than warm on purpose: everything else out
// there is warm, the regolith browns and the flames, so a cool grey is
// the one hue that cannot be mistaken for ground. These go straight to the
// framebuffer, ground_tri applies no lighting, so what is written here is
// exactly what shows and the contrast has to be built in. The first pass was
// a near white 255,241,232 against 214,202,196, a 16 percent step, and at
// 120x120 that is not plating, it is a white rectangle with a suggestion.
const Rgb k_deck_lit{170, 179, 188};
const Rgb k_deck_dark{104, 112, 121};
// What the lit corner of the deck reaches. The engine's light is at
// (0.40, 0.82, -0.40), so the sheen runs from the -X +Z corner up to the
// +X -Z one.
const Rgb k_deck_sheen{226, 233, 240};
// The block under the deck. Its sides are the pad's silhouette, so they have
// to be darker than the deck and darker than the ground's mid tone
// (95,87,79), which the old warm brown sides matched almost exactly and
// disappeared into.
const Rgb k_pad_side{62, 69, 77};

// The four walls carry draw_box's own per face shading, so the body reads the
// same as it did when it was a box. Indices run the same cycle the walls are
// drawn in: -Z, +X, +Z, -X.
const float k_wall_shade[4] = {0.70f, 1.00f, 0.90f, 0.60f};

// The deck, as a 3x3 grid of vertices rather than one flat quad.
//
// draw_box paints its lid a single colour, and a plain white slab is the one
// thing out there that looks untextured, because it is: the engine has no
// texture mapping. Per vertex colour is the whole of what it does have, and
// it is free, since ScreenTriangle already carries three colours and the
// rasterizer already interpolates them. Nine vertices over four quads is
// enough to read as plating, alternating light and shadow so the seams fall
// where a real deck's panel joins would, and it costs 8 triangles.
void draw_deck(const tl::Pad& pad, float y) {
    const float half = to_f(tl::k_pad_half);
    const float x0 = to_f(pad.x) - half, z0 = to_f(pad.z) - half;
    const float step = half;

    for (int gz = 0; gz < 2; gz++) {
        for (int gx = 0; gx < 2; gx++) {
            const float qx[4] = {x0 + gx * step, x0 + (gx + 1) * step,
                                 x0 + (gx + 1) * step, x0 + gx * step};
            const float qz[4] = {z0 + gz * step, z0 + gz * step,
                                 z0 + (gz + 1) * step, z0 + (gz + 1) * step};
            Rgb c[4];
            for (int i = 0; i < 4; i++) {
                // Corner index in the 3x3 grid, so neighbouring quads share a
                // colour at the vertex they share and the plating reads as one
                // surface rather than four tiles.
                const int cx = gx + ((i == 1 || i == 2) ? 1 : 0);
                const int cz = gz + ((i == 2 || i == 3) ? 1 : 0);
                // Two terms. The sheen runs across the deck toward the
                // engine's own light, which is what makes a flat horizontal
                // slab read as metal instead of as a hole cut in the scene:
                // lambert alone gives a horizontal surface one constant
                // value, so without this the deck is the only unlit thing out
                // there. The plate checker then rides on top of it.
                const int sheen = (cx - cz + 2) * 255 / 4;    // 0..255
                const bool bright = ((cx + cz) & 1) == 0;
                const Rgb base = bright ? k_deck_lit : k_deck_dark;
                const int r = base.r + (k_deck_sheen.r - base.r) * sheen / 255;
                const int g = base.g + (k_deck_sheen.g - base.g) * sheen / 255;
                const int b = base.b + (k_deck_sheen.b - base.b) * sheen / 255;
                c[i] = Rgb{static_cast<uint8_t>(r), static_cast<uint8_t>(g),
                           static_cast<uint8_t>(b)};
            }
            deck_quad(qx, y, qz, c);
        }
    }
}

// One vertical wall of the pad body, wound outward so the rasterizer's own
// backface cull (`area <= 0` in draw_typed) drops the walls facing away.
//
// Corners run A_low, B_low, B_high, A_high, which is the REVERSE of the order
// draw_box's own face table uses. That is not a tidy up: wound draw_box's way
// the cull keeps the far wall and drops the near one, so the pad showed the
// inside of its back face and nothing at all where the front should be. The
// engine's boxes have the same inversion, but every one of them is either
// viewed from a distance or hidden inside something, so it has never shown;
// fixing it there reaches every game and belongs in its own change.
void pad_wall(float ax, float az, float bx, float bz,
              float y_low, float y_high, float shade) {
    const float wx[4] = {ax, bx, bx, ax};
    const float wy[4] = {y_low, y_low, y_high, y_high};
    const float wz[4] = {az, bz, bz, az};

    int sx[4], sy[4], sz[4];
    for (int i = 0; i < 4; i++) {
        if (!g_renderer.project(wx[i], wy[i], wz[i], sx[i], sy[i], sz[i])) return;
    }

    const Rgb flat{static_cast<uint8_t>(k_pad_side.r * shade),
                   static_cast<uint8_t>(k_pad_side.g * shade),
                   static_cast<uint8_t>(k_pad_side.b * shade)};
    // The same two corner lift draw_box gives a wall, so a flat face still
    // reads with a gradient across it rather than as paper.
    const Rgb lift{static_cast<uint8_t>(flat.r + 30 > 255 ? 255 : flat.r + 30),
                   static_cast<uint8_t>(flat.g + 30 > 255 ? 255 : flat.g + 30),
                   static_cast<uint8_t>(flat.b + 30 > 255 ? 255 : flat.b + 30)};
    const Rgb c[4] = {flat, lift, lift, flat};

    const int t0[3] = {0, 1, 2}, t1[3] = {0, 2, 3};
    const int* tris[2] = {t0, t1};
    for (int t = 0; t < 2; t++) {
        const int i0 = tris[t][0], i1 = tris[t][1], i2 = tris[t][2];
        const int qx[3] = {sx[i0], sx[i1], sx[i2]};
        const int qy[3] = {sy[i0], sy[i1], sy[i2]};
        const int qz[3] = {sz[i0], sz[i1], sz[i2]};
        const Rgb qc[3] = {c[i0], c[i1], c[i2]};
        ground_tri(qx, qy, qz, qc);
    }
}

// The pads. Four walls for the body and the plated deck on top, and NOTHING
// underneath that deck.
//
// This was a draw_box plus a deck laid over its lid, and that lid is what made
// the pad mottle. draw_box always paints a top face, so the pad had two
// horizontal surfaces 0.048 units apart, and this depth buffer cannot tell
// them apart: it is one byte over the whole 6 to 170 range, so at the 23 units
// the camera works at, 0.048 of vertical separation is about a fifth of a
// single depth step. The two tied nearly everywhere, ties go to whoever drew
// first, and the lid drew first, so the deck lost most of its own surface to
// the grey lid underneath and kept only the pixels where the interpolation
// happened to round its way.
//
// Widening the gap is not the fix. A gap large enough to win at 23 units is
// still too small at 40, the pads are seen from across the valley, and a gap
// that big would show daylight between the deck and the body it sits on.
// The fix is that there is only one surface up there now, so there is nothing
// left to tie with. The deck stands proud of its apron for that same reason
// and that part still earns its 2.4 units: apron to deck is about ten depth
// steps, which separates cleanly.
void draw_pads(const World& world) {
    const float half = to_f(tl::k_pad_half);
    const float rise = to_f(tl::k_pad_rise);

    for (int i = 0; i < tl::k_pad_count; i++) {
        const tl::Pad& pad = world.pads[i];
        const float x0 = to_f(pad.x) - half, x1 = to_f(pad.x) + half;
        const float z0 = to_f(pad.z) - half, z1 = to_f(pad.z) + half;
        const float y_low = to_f(pad.y), y_high = y_low + rise;

        // Round the rim: -Z, +X, +Z, -X, matching k_wall_shade.
        pad_wall(x0, z0, x1, z0, y_low, y_high, k_wall_shade[0]);
        pad_wall(x1, z0, x1, z1, y_low, y_high, k_wall_shade[1]);
        pad_wall(x1, z1, x0, z1, y_low, y_high, k_wall_shade[2]);
        pad_wall(x0, z1, x0, z0, y_low, y_high, k_wall_shade[3]);

        draw_deck(pad, y_high);
    }
}

void draw_ship(const World& world) {
    // The attitude straight through, as the orientation it is. No angles are
    // extracted on the way, which is the point: extracting them would put the
    // singularity back in the one place it would be hardest to spot, the
    // picture.
    g_renderer.draw_mesh(models::tomlander::tom,
                         to_f(world.x), to_f(world.y), to_f(world.z),
                         pse::quat_basis(world.q), k_ship_scale);
}

// How far below the nozzle the plume's aim is sampled, in world units. Only
// its direction is kept, so the number just has to be far enough that the two
// projected points are several pixels apart and the direction is not noise.
constexpr float k_plume_probe = 3.0f;

// Thruster flames, as billboards drawn after the geometry with a depth test,
// which is how the engine expects sprites to go on.
//
// A plume leaves along the hull's own down axis, so it has to be aimed, not
// just placed. The nozzle mouth was rotated with the hull from the start and
// the flame was then drawn straight down the screen from it, which reads as
// correct only while the ship is level: lean it over and the exhaust still
// falls vertically out of a tilted nozzle. Both ends of the plume go through
// the projection now, and the pixels walk the line between them.
void draw_flames(const World& world, const pse::RenderTarget& target,
                 uint32_t time_ms) {
    // One basis for the nozzle mouths and the plume direction both, the same
    // one the hull is drawn with. When these were rebuilt out of the sim's
    // angles instead, the flames were a second copy of the attitude
    // convention, and a second copy is a thing to get out of step.
    const pse::Basis b = pse::quat_basis(world.q);

    // The hull's own down, in the world.
    const float dx = -b.m[1], dy = -b.m[4], dz = -b.m[7];

    for (int i = 0; i < tl::kPodCount; i++) {
        g_stats.flame_ax[i] = 0;
        g_stats.flame_ay[i] = 0;
        const uint8_t throttle = world.throttle[i];
        if (throttle < 8) continue;

        // The nozzle mouth in model space, through the hull's basis.
        const float mx = tl::k_pods[i].ox * 4.0f * k_ship_scale;
        const float mz = tl::k_pods[i].oz * 4.0f * k_ship_scale;
        const float my = -3.1f * k_ship_scale;

        const float wx = to_f(world.x) + b.m[0]*mx + b.m[1]*my + b.m[2]*mz;
        const float wy = to_f(world.y) + b.m[3]*mx + b.m[4]*my + b.m[5]*mz;
        const float wz = to_f(world.z) + b.m[6]*mx + b.m[7]*my + b.m[8]*mz;

        int px, py;
        float scale;
        uint8_t depth;
        if (!g_renderer.project_billboard(wx, wy, wz, 1.0f, px, py, scale,
                                          depth)) {
            continue;
        }

        // Down the screen is the fallback, for the one case the aim point has
        // nothing to say: a nozzle pointing at the camera projects both ends
        // to nearly the same pixel, and normalising that is dividing noise.
        float ax = 0.0f, ay = 1.0f;
        int qx, qy;
        float qscale;
        uint8_t qdepth;
        if (g_renderer.project_billboard(wx + dx * k_plume_probe,
                                         wy + dy * k_plume_probe,
                                         wz + dz * k_plume_probe,
                                         1.0f, qx, qy, qscale, qdepth)) {
            const float vx = static_cast<float>(qx - px);
            const float vy = static_cast<float>(qy - py);
            const float mag = std::sqrt(vx * vx + vy * vy);
            if (mag > 1.5f) {
                ax = vx / mag;
                ay = vy / mag;
            }
        }
        // The plume's width runs across it, whichever way it is pointing.
        const float bx = -ay, by = ax;
        g_stats.flame_ax[i] = static_cast<int8_t>(round_i(ax * 64.0f));
        g_stats.flame_ay[i] = static_cast<int8_t>(round_i(ay * 64.0f));

        const int flick = static_cast<int>((time_ms / 40 + i * 3) % 3);
        const int len = 2 + (throttle * 7) / 255 + flick;
        for (int k = 0; k < len; k++) {
            const int half = 1 + (2 * (len - k)) / (len * 2 + 1);
            // White at the throat, then yellow, then orange.
            const uint8_t r = 255;
            const uint8_t g = k * 3 < len ? 255 : (k * 3 < len * 2 ? 236 : 163);
            const uint8_t b = k * 3 < len ? 232 : (k * 3 < len * 2 ? 39 : 0);
            const float cxf = px + ax * static_cast<float>(k);
            const float cyf = py + ay * static_cast<float>(k);
            for (int w = -half; w <= half; w++) {
                const float fw = static_cast<float>(w);
                const int x = round_i(cxf + bx * fw);
                const int yy = round_i(cyf + by * fw);
                if (x < 0 || yy < 0 || x >= target.width ||
                    yy >= target.height) {
                    continue;
                }
                if (g_raster.test_and_set_depth(x, yy, depth)) {
                    g_raster.plot(x, yy, r, g, b);
                }
            }
        }
    }
}

// An arrow at the edge of the screen for the deck you cannot see, and only
// then: a marker stuck on top of something already in frame is the decorative
// status line rule 9 exists to keep off this screen. With the pads no longer
// marking themselves, the HUD's own range readout is what names the target
// while it is on screen.
//
// Direction comes from the world bearing rotated into the camera's own frame,
// NOT from the projection, because a pad behind the camera has no valid
// projection at all and behind the camera is when an arrow earns its pixels.
void draw_target_arrow(const World& world, const pse::RenderTarget& target,
                       float yaw) {
    g_stats.arrow_x = -1;
    g_stats.arrow_y = -1;
    if (world.state != tl::Flight::Flying) return;

    const tl::Pad& pad = world.pads[world.target];
    const float deck_y = to_f(pad.y) + to_f(tl::k_pad_rise);

    int px, py, pz;
    const bool on_screen =
        g_renderer.project(to_f(pad.x), deck_y, to_f(pad.z), px, py, pz);
    g_stats.pad_visible = on_screen;
    g_stats.pad_x = static_cast<int16_t>(on_screen ? px : -1);
    g_stats.pad_y = static_cast<int16_t>(on_screen ? py : -1);

    const int margin = 13;
    if (on_screen && px >= margin && px <= target.width - margin &&
        py >= margin && py <= target.height - margin) {
        return;
    }

    const float dx = to_f(pad.x) - to_f(world.x);
    const float dz = to_f(pad.z) - to_f(world.z);
    const float cy = std::cos(yaw), sy = std::sin(yaw);
    const float right = dx * cy - dz * sy;
    const float fwd = dx * sy + dz * cy;
    float ax = right, ay = -fwd;             // screen y counts downward
    const float len = std::sqrt(ax * ax + ay * ay);
    if (len < 1e-4f) return;
    ax /= len;
    ay /= len;

    const float half_w = target.width * 0.5f - 9.0f;
    const float half_h = target.height * 0.5f - 9.0f;
    const float tx = std::fabs(ax) > 1e-4f ? half_w / std::fabs(ax) : 1e9f;
    const float tz = std::fabs(ay) > 1e-4f ? half_h / std::fabs(ay) : 1e9f;
    const float t = tx < tz ? tx : tz;
    const float ex = target.width * 0.5f + ax * t;
    const float ey = target.height * 0.5f + ay * t;
    g_stats.arrow_x = static_cast<int16_t>(ex);
    g_stats.arrow_y = static_cast<int16_t>(ey);

    // A small solid triangle, filled by walking out from the tip. No depth
    // test: HUD furniture sits on top of the world by definition.
    const float perp_x = -ay, perp_y = ax;
    for (int step = 0; step <= 7; step++) {
        const float cx = ex + ax * (4.0f - step);
        const float cyp = ey + ay * (4.0f - step);
        const int spread = step / 2;
        for (int w = -spread; w <= spread; w++) {
            pse::plot_pixel(target,
                            static_cast<int>(cx + perp_x * w),
                            static_cast<int>(cyp + perp_y * w),
                            255, 163, 0);
        }
    }
}

}  // namespace

FrameStats last_frame_stats() { return g_stats; }

void render_scene(const World& world, const pse::RenderTarget& target,
                  float yaw, uint32_t time_ms) {
    g_queue.reset();
    g_raster.begin_frame_collect(target, g_queue);

    g_renderer.set_fov(k_cam_fov);
    g_renderer.set_depth_range(k_z_near, k_z_far);
    // Equal reach and height is what makes the camera exactly 45 degrees
    // above the horizontal. look_lift 0: this camera studies one object, and
    // the chase camera's default whole unit drops the ship out of frame.
    g_renderer.set_orbit_camera(to_f(world.x), to_f(world.y) - 2.0f,
                                to_f(world.z), yaw,
                                k_cam_reach, k_cam_reach, 0.0f);

    draw_ground(world);
    draw_pads(world);
    draw_ship(world);

    // A dusk sky: the ground fogs into it rather than ending on a line.
    pse::run_split(g_raster, g_queue,
                   pse::SkyGradient{16, 22, 48, 92, 74, 110});
    g_raster.end_collect();

    g_stats.queued = g_queue.count;
    g_stats.dropped = g_queue.dropped;

    draw_flames(world, target, time_ms);
    draw_target_arrow(world, target, yaw);
}

}  // namespace tlr
