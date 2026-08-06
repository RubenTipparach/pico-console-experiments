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

// Angle units to radians. The sim counts 4096 to the turn in fp8.
float angle_f(int32_t fp8_angle) {
    return static_cast<float>(fp8_angle) * (2.0f * k_pi / (4096.0f * 256.0f));
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

// The pads. A box each, which is the engine's own primitive: rule 11 keeps
// hand written vertex tables out of C++, and a box is exactly the trivial
// case it exempts because draw_box already generates one.
//
// The deck stands proud of its apron for a reason that is not looks. Lying
// flat it shared a depth step with the ground and they tied, and ties go to
// whoever drew first, so the pad flickered.
void draw_pads(const World& world, uint32_t time_ms) {
    for (int i = 0; i < tl::k_pad_count; i++) {
        const tl::Pad& pad = world.pads[i];
        const bool done = world.state == tl::Flight::Landed &&
                          world.landed_on == i;
        const bool target = i == world.target && !done;

        // The deck you are aiming at pulses. Nothing else out there is
        // saturated, so it reads from across the valley with no marker.
        uint8_t sr = 95, sg = 87, sb = 79;
        if (target) {
            const int phase = static_cast<int>((time_ms / 12) % 256);
            const int wave = tl::sin_fp(phase * 16) >> 7;      // -128..128
            const int lit = 190 + wave / 2;
            sr = static_cast<uint8_t>(lit);
            sg = static_cast<uint8_t>(lit * 163 / 255);
            sb = 0;
        }

        // draw_box's own vertex table puts x and z at +/- 0.5 and y at 0 to 1,
        // so the sizes it wants are the FULL width and the FULL height, and
        // the box grows UP from the y it is given rather than being centred
        // on it. Passing half extents and a centre, which is what the name
        // suggests, drew a deck half the size of the area that counts as the
        // pad, floating above its own apron.
        const float full = to_f(tl::k_pad_half) * 2.0f;
        const float rise = to_f(tl::k_pad_rise);
        g_renderer.draw_box(to_f(pad.x), to_f(pad.y), to_f(pad.z),
                            full, rise, full,
                            // A white deck, which is the one shape out there
                            // that is obviously not terrain.
                            255, 241, 232, sr, sg, sb);
    }
}

void draw_ship(const World& world) {
    g_renderer.draw_mesh(models::tomlander::tom,
                         to_f(world.x), to_f(world.y), to_f(world.z),
                         0.0f, k_ship_scale,
                         255, 255, 255,
                         angle_f(world.pitch), 0,
                         angle_f(world.roll));
}

// Thruster flames, as billboards drawn after the geometry with a depth test,
// which is how the engine expects sprites to go on.
void draw_flames(const World& world, const pse::RenderTarget& target,
                 uint32_t time_ms) {
    const float sp = tl::sin_fp(world.pitch >> 8) / 16384.0f;
    const float cp = tl::cos_fp(world.pitch >> 8) / 16384.0f;
    const float sr = tl::sin_fp(world.roll >> 8) / 16384.0f;
    const float cr = tl::cos_fp(world.roll >> 8) / 16384.0f;

    for (int i = 0; i < tl::kPodCount; i++) {
        const uint8_t throttle = world.throttle[i];
        if (throttle < 8) continue;

        // The nozzle mouth in model space, then through the same rotation the
        // hull got: roll, then pitch. Yaw is zero, the hull never turns.
        const float mx = tl::k_pods[i].ox * 4.0f * k_ship_scale;
        const float mz = tl::k_pods[i].oz * 4.0f * k_ship_scale;
        const float my = -3.1f * k_ship_scale;
        const float rx = mx * cr - my * sr;
        const float ry = mx * sr + my * cr;
        const float fy = ry * cp + mz * sp;
        const float fz = mz * cp - ry * sp;

        int px, py, depth_i;
        float scale;
        uint8_t depth;
        if (!g_renderer.project_billboard(to_f(world.x) + rx,
                                          to_f(world.y) + fy,
                                          to_f(world.z) + fz,
                                          1.0f, px, py, scale, depth)) {
            continue;
        }
        (void)depth_i;

        const int flick = static_cast<int>((time_ms / 40 + i * 3) % 3);
        const int len = 2 + (throttle * 7) / 255 + flick;
        for (int k = 0; k < len; k++) {
            const int half = 1 + (2 * (len - k)) / (len * 2 + 1);
            // White at the throat, yellow, then the orange the pads use.
            const uint8_t r = 255;
            const uint8_t g = k * 3 < len ? 255 : (k * 3 < len * 2 ? 236 : 163);
            const uint8_t b = k * 3 < len ? 232 : (k * 3 < len * 2 ? 39 : 0);
            for (int w = -half; w <= half; w++) {
                const int x = px + w, yy = py + k;
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
// then: the deck pulses, and a marker stuck on top of something already in
// frame is the decorative status line rule 9 exists to keep off this screen.
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
    draw_pads(world, time_ms);
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
