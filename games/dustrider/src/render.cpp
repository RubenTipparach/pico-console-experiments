#include "render.hpp"

#include <cmath>

#include "pse/parallel.hpp"
#include "pse/raster.hpp"
#include "pse/renderer3d.hpp"

#include "bike.hpp"
#include "cactus.hpp"
#include "rock.hpp"

namespace drr {
namespace {

using dr::World;

// Rendering state. Static because dynamic allocation is banned; this is the
// documented RAM cost of drawing this game:
//   Rasterizer  ~14.5 KB (depth buffer)
//   FrameQueue  ~15.4 KB (640 triangles)
//   the rest    well under 1 KB
pse::Rasterizer g_raster;
pse::Renderer3D g_renderer(g_raster);
pse::FrameQueue g_queue;

constexpr float k_pi = 3.14159265f;

// The camera hangs south of the bike looking north, which puts world +x on
// screen right: the bike rides rightward. The ground is flat, so every
// rise and fall on screen is the road's own north/south curve seen in
// perspective.
// Close enough that the near edge of the tarmac is right in front of the
// lens rather than across a field of sand. The camera sits this far south
// of the bike, and the road's south edge is k_road_half nearer still, so
// the foreground sand band is only what is left over.
constexpr float k_cam_dist = 5.2f;
constexpr float k_cam_up = 3.4f;
constexpr float k_cam_pitch = -0.56f;

// How near and far the bike is ever allowed to sit from the lens. The
// camera is road locked, so the bike's depth is the road distance plus
// however far off the centerline the rider has strayed; these bound that.
// k_bike_depth_max is also what the survival window is derived against,
// because the bike is smallest, and so leaves the frame latest, when it is
// as far from the camera as it can get.
constexpr float k_bike_depth_min = 2.2f;
constexpr float k_bike_depth_max = 9.0f;

// How many 1 m columns of ground are drawn, and where they start relative
// to the camera.
//
// The width matters more than it looks. The frustum is 90 degrees, so
// ground at distance d needs d metres of x either side of the camera to
// reach the screen edges. Too few columns and the desert simply stops
// mid screen, which reads as a pale mound floating in the middle distance
// rather than as missing geometry.
constexpr int k_columns = 26;
constexpr float k_column_back = 13.0f;

// The nearest ground the camera draws, just in front of its near plane.
// Ground strips measure their near edge from HERE, not from the road:
// anchoring the foreground to the centerline puts it behind the camera the
// moment the rider drifts north, and a quad with a corner behind the near
// plane is dropped whole, which reads as the desert vanishing.
constexpr float k_near_ground = 0.45f;

// Presentation state: the eased camera z, the bike's body pitch, and the
// dust off the rear wheel. Replaying a run reproduces the sim exactly even
// though the dust differs.
float g_cam_z = 0.0f;
float g_pitch = 0.0f;
bool g_cam_seeded = false;
FrameStats g_stats{};

struct Dust {
    float x, y, z;
    float vx, vy;
    uint8_t life = 0;
};
constexpr int k_max_dust = 12;
Dust g_dust[k_max_dust];
uint32_t g_dust_rng = 0x0DDD1234u;

inline uint32_t dust_rand() {
    g_dust_rng ^= g_dust_rng << 13;
    g_dust_rng ^= g_dust_rng >> 17;
    g_dust_rng ^= g_dust_rng << 5;
    return g_dust_rng;
}

// Quarter resolution sine, s8 amplitude 127, 64 entries per turn.
const int8_t k_sin[64] = {
    0, 12, 24, 36, 48, 59, 70, 80, 89, 98, 105, 112, 117, 121, 124, 126,
    127, 126, 124, 121, 117, 112, 105, 98, 89, 80, 70, 59, 48, 36, 24, 12,
    0, -12, -24, -36, -48, -59, -70, -80, -89, -98, -105, -112, -117, -121,
    -124, -126, -127, -126, -124, -121, -117, -112, -105, -98, -89, -80,
    -70, -59, -48, -36, -24, -12,
};

inline int sin64(int32_t index) { return k_sin[index & 63]; }

inline float fp_to_f(int32_t v) { return static_cast<float>(v) / 256.0f; }

inline uint8_t clamp8(int v) {
    return static_cast<uint8_t>(v < 0 ? 0 : (v > 255 ? 255 : v));
}

// The road centerline in world units at a float x.
inline float center_at(const World& world, float x) {
    return fp_to_f(dr::track_center_z(
        world, static_cast<int32_t>(x * 256.0f)));
}

void push_quad(const int sx[4], const int sy[4], const int sz[4],
               const uint8_t r[4], const uint8_t g[4], const uint8_t b[4]) {
    pse::ScreenTriangle t1;
    t1.x0 = static_cast<int16_t>(sx[0]); t1.y0 = static_cast<int16_t>(sy[0]);
    t1.x1 = static_cast<int16_t>(sx[1]); t1.y1 = static_cast<int16_t>(sy[1]);
    t1.x2 = static_cast<int16_t>(sx[2]); t1.y2 = static_cast<int16_t>(sy[2]);
    t1.z0 = static_cast<uint16_t>(sz[0]);
    t1.z1 = static_cast<uint16_t>(sz[1]);
    t1.z2 = static_cast<uint16_t>(sz[2]);
    t1.r0 = r[0]; t1.g0 = g[0]; t1.b0 = b[0];
    t1.r1 = r[1]; t1.g1 = g[1]; t1.b1 = b[1];
    t1.r2 = r[2]; t1.g2 = g[2]; t1.b2 = b[2];
    g_raster.draw(t1);

    pse::ScreenTriangle t2 = t1;
    t2.x1 = t1.x2; t2.y1 = t1.y2; t2.z1 = t1.z2;
    t2.r1 = r[2]; t2.g1 = g[2]; t2.b1 = b[2];
    t2.x2 = static_cast<int16_t>(sx[3]); t2.y2 = static_cast<int16_t>(sy[3]);
    t2.z2 = static_cast<uint16_t>(sz[3]);
    t2.r2 = r[3]; t2.g2 = g[3]; t2.b2 = b[3];
    g_raster.draw(t2);
}

// Project and push one world space quad with per corner colour. Corners run
// counter clockwise seen from the camera: near left, near right, far right,
// far left.
bool quad_world(const float wx[4], const float wy[4], const float wz[4],
                const uint8_t r[4], const uint8_t g[4], const uint8_t b[4]) {
    int sx[4], sy[4], sz[4];
    for (int i = 0; i < 4; i++) {
        if (!g_renderer.project(wx[i], wy[i], wz[i], sx[i], sy[i], sz[i])) {
            return false;
        }
    }
    push_quad(sx, sy, sz, r, g, b);
    return true;
}

// One ground strip: for each 1 m column, a flat quad from near_z(x) to
// far_z(x). The edges are callbacks so a strip can hug the road (bending
// with it) or stay pinned to the camera, which is what keeps the desert
// gapless whatever the road is doing. Templated, so the callbacks inline
// and nothing is allocated.
template <typename NearFn, typename FarFn>
void draw_strip(float x0, int columns, float step, NearFn near_z, FarFn far_z,
                uint8_t base_r, uint8_t base_g, uint8_t base_b,
                int shade_near, int shade_far) {
    for (int i = 0; i < columns; i++) {
        const float xa = x0 + static_cast<float>(i) * step;
        const float xb = xa + step;

        const float wx[4] = {xa, xb, xb, xa};
        const float wy[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        const float wz[4] = {near_z(xa), near_z(xb), far_z(xb), far_z(xa)};
        uint8_t r[4], g[4], b[4];
        const int lift[4] = {shade_near, shade_near, shade_far, shade_far};
        for (int c = 0; c < 4; c++) {
            r[c] = clamp8(base_r + lift[c]);
            g[c] = clamp8(base_g + lift[c]);
            b[c] = clamp8(base_b + lift[c] / 2);
        }
        quad_world(wx, wy, wz, r, g, b);
    }
}

// The horizon: silhouette walls at fixed distances ahead of the camera, so
// they stay on the skyline however far the road has wandered. Two layers,
// the far one paler, which is the only depth cue the sky needs.
void draw_horizon(const World& world, float cam_x, float cam_z) {
    struct Ridge {
        float depth, amp, base, phase;
        uint8_t r, g, b;
    };
    static const Ridge k_ridges[2] = {
        {52.0f, 7.0f, 3.0f, 11.0f, 236, 206, 172},
        {38.0f, 5.0f, 1.6f, 0.0f, 214, 174, 128},
    };
    // 5 m columns with hashed corner heights: linear between them, the
    // ridge comes out as angular peaks rather than a rolling swell, which
    // is what a distant range looks like once it is eight pixels tall.
    constexpr int k_ridge_cols = 14;
    constexpr float k_ridge_step = 8.0f;
    for (const Ridge& ridge : k_ridges) {
        const float x0 =
            std::floor((cam_x - 56.0f) / k_ridge_step) * k_ridge_step;
        const float z = cam_z + ridge.depth;
        auto ridge_h = [&](float x) {
            const uint32_t k = static_cast<uint32_t>(
                static_cast<int32_t>(x / k_ridge_step) + 4096);
            const uint32_t h = (k + static_cast<uint32_t>(ridge.phase)) *
                               2654435761u;
            return ridge.base +
                   ridge.amp * static_cast<float>((h >> 13) & 255) / 255.0f;
        };
        for (int i = 0; i < k_ridge_cols; i++) {
            const float xa = x0 + i * k_ridge_step;
            const float xb = xa + k_ridge_step;
            const float ha = ridge_h(xa);
            const float hb = ridge_h(xb);
            const float wx[4] = {xa, xb, xb, xa};
            const float wy[4] = {-2.0f, -2.0f, hb, ha};
            const float wz[4] = {z, z, z, z};
            const uint8_t r[4] = {ridge.r, ridge.r, ridge.r, ridge.r};
            const uint8_t g[4] = {ridge.g, ridge.g, ridge.g, ridge.g};
            const uint8_t b[4] = {ridge.b, ridge.b, ridge.b, ridge.b};
            quad_world(wx, wy, wz, r, g, b);
        }
    }
}

// Scree and boulders scattered over the sand. This is the frame's only
// parallax: everything else out there (the ridges, the sky) is pinned to
// the camera, so without something at a real world position the desert
// slides past as one flat sheet and the sense of speed goes with it.
// Near stones sweep by fast, far ones crawl, for free, because they are
// actually where they say they are.
//
// Positions come from a hash of the world grid cell, never from a frame
// counter, so a stone stays put while the camera moves over it.
void draw_ground_detail(const World& world, float cam_x, float cam_z) {
    constexpr float k_cell = 1.25f;
    const int first =
        static_cast<int>(std::floor((cam_x - k_column_back) / k_cell));
    const float edge = fp_to_f(dr::k_road_half);

    for (int i = 0; i < 22; i++) {
        const int gx = first + i;
        const float x = static_cast<float>(gx) * k_cell;
        const uint32_t base = static_cast<uint32_t>(gx + 8192) * 2654435761u;

        for (int side = 0; side < 2; side++) {
            const uint32_t h = base ^ (side ? 0x9E3779B9u : 0x85EBCA6Bu);
            if ((h & 15) == 0) continue;     // gaps, so it is not a grid

            // Out on the sand, never on the tarmac. The two shoulders get
            // different bands because they sit at very different depths:
            // north runs away from the lens and has room to spread, south
            // is the near foreground and only a metre of it is both past
            // the near guard and still on screen. That near metre is worth
            // the trouble, since it is where parallax is fastest.
            const float spread = static_cast<float>((h >> 6) & 31) / 31.0f;
            const float out = side ? edge + 0.5f + spread * 5.0f
                                   : edge + 0.35f + spread * 0.95f;
            const float cz = center_at(world, x) + (side ? out : -out);

            // Keep detail out of the lens. A hand sized rock two metres
            // from the camera is a boulder the size of the bike, and the
            // south shoulder passes very close indeed.
            const float depth = cz - cam_z;
            if (depth < 1.6f || depth > 16.0f) continue;

            const float jx = x + static_cast<float>((h >> 12) & 15) * 0.09f;
            const float size = 0.22f + static_cast<float>((h >> 17) & 7) * 0.055f;
            const int tone = static_cast<int>((h >> 21) & 15);

            // A real rock, narrower at the top than the base, so it throws
            // a proper silhouette at any distance or camera pitch instead
            // of a flat quad that goes edge on and flickers to a line.
            // Occasionally bigger, for a boulder among the scree.
            const bool big = (h & 28) == 0;
            const float scale = big ? size * 1.6f : size;
            const float yaw = static_cast<float>((h >> 25) & 15) *
                              (k_pi / 8.0f);
            const uint8_t tint = clamp8(215 - tone * 5);
            g_renderer.draw_mesh(models::rock, jx, 0.0f, cz, yaw, scale,
                                 tint, tint, tint);
        }
    }
}

// Guardrail: a bright beam over dark posts, riding the north edge of the
// road. North means behind the bike from this camera, so it can never
// occlude the rider.
void draw_rails(const World& world, float cam_x) {
    const int32_t first =
        (static_cast<int32_t>((cam_x - k_column_back) * 256.0f)) >>
        dr::k_chunk_shift;
    const float edge = fp_to_f(dr::k_road_half);
    for (int i = 0; i < 12; i++) {
        const int32_t index = first + i;
        if (index < 0) continue;
        const int32_t x_fp = index << dr::k_chunk_shift;
        if (!dr::track_rail_at(world, x_fp + 1)) continue;
        const float xa = fp_to_f(x_fp);
        const float xb = xa + 2.0f;
        const float za = center_at(world, xa) + edge;
        const float zb = center_at(world, xb) + edge;

        {
            const float wx[4] = {xa, xb, xb, xa};
            const float wy[4] = {0.34f, 0.34f, 0.78f, 0.78f};
            const float wz[4] = {za, zb, zb, za};
            const uint8_t r[4] = {200, 200, 244, 244};
            const uint8_t g[4] = {200, 200, 244, 244};
            const uint8_t b[4] = {204, 204, 250, 250};
            quad_world(wx, wy, wz, r, g, b);
        }
        {
            const float px = xa + 1.0f;
            const float pz = center_at(world, px) + edge;
            const float wx[4] = {px - 0.08f, px + 0.08f, px + 0.08f,
                                 px - 0.08f};
            const float wy[4] = {0.0f, 0.0f, 0.40f, 0.40f};
            const float wz[4] = {pz, pz, pz, pz};
            const uint8_t r[4] = {88, 88, 60, 60};
            const uint8_t g[4] = {74, 74, 50, 50};
            const uint8_t b[4] = {62, 62, 42, 42};
            quad_world(wx, wy, wz, r, g, b);
        }
    }
}

void draw_cacti(const World& world, float cam_x) {
    const int32_t from =
        static_cast<int32_t>((cam_x - k_column_back) * 256.0f);
    int32_t scan = from < 0 ? 0 : from;
    const int32_t limit = static_cast<int32_t>((cam_x + 12.0f) * 256.0f);
    for (int guard = 0; guard < 12 && scan < limit; guard++) {
        int32_t cx, cz;
        if (!dr::track_next_cactus(world, scan, limit - scan, cx, cz)) break;
        // Size and facing vary by position, not by randomness, so a chunk
        // renders identically every frame it is on screen.
        // Keep the arm broadside to the camera, mirrored at random. A free
        // yaw points the one arm up the z axis half the time, and a saguaro
        // seen end on is just a post.
        const uint32_t hash = static_cast<uint32_t>(cx) * 2654435761u;
        const float jitter =
            (static_cast<float>((hash >> 16) & 63) - 32.0f) * 0.006f;
        const float yaw = ((hash >> 3) & 1) ? jitter : k_pi + jitter;
        const float scale =
            0.85f + static_cast<float>((hash >> 8) & 63) / 160.0f;
        g_renderer.draw_mesh(models::cactus, fp_to_f(cx), 0.0f, fp_to_f(cz),
                             yaw, scale);
        scan = cx + 1;
    }
}

// The bike's horizontal screen span, from its own vertex bounds put
// through the same yaw, pitch and lift that draw_mesh will use. Measured
// from the model rather than hardcoded, so reshaping the bike cannot
// quietly invalidate the window that the run's life depends on.
void measure_bike_span(float x, float y, float z, float yaw, float pitch) {
    static float bx0 = 0.0f, bx1 = 0.0f, by0 = 0.0f, by1 = 0.0f;
    static float bz0 = 0.0f, bz1 = 0.0f;
    static bool bounds_ready = false;
    if (!bounds_ready) {
        const pse::MeshData& m = models::bike;
        const float unit = 1.0f / static_cast<float>(m.scale);
        for (uint16_t i = 0; i < m.vertex_count; i++) {
            const float vx = m.vertices[i].x * unit;
            const float vy = m.vertices[i].y * unit;
            const float vz = m.vertices[i].z * unit;
            if (i == 0) {
                bx0 = bx1 = vx; by0 = by1 = vy; bz0 = bz1 = vz;
            }
            if (vx < bx0) bx0 = vx;
            if (vx > bx1) bx1 = vx;
            if (vy < by0) by0 = vy;
            if (vy > by1) by1 = vy;
            if (vz < bz0) bz0 = vz;
            if (vz > bz1) bz1 = vz;
        }
        bounds_ready = true;
    }

    const float sin_yaw = std::sin(yaw), cos_yaw = std::cos(yaw);
    const float sin_p = std::sin(pitch), cos_p = std::cos(pitch);
    int lo = 32767, hi = -32768;
    int ylo = 32767, yhi = -32768;
    for (int corner = 0; corner < 8; corner++) {
        const float lx = (corner & 1) ? bx1 : bx0;
        const float ry = (corner & 2) ? by1 : by0;
        const float rz = (corner & 4) ? bz1 : bz0;
        const float ly = ry * cos_p + rz * sin_p;
        const float lz = rz * cos_p - ry * sin_p;
        const float wx = x + lx * cos_yaw + lz * sin_yaw;
        const float wz = z - lx * sin_yaw + lz * cos_yaw;
        int sx, sy, sz;
        if (!g_renderer.project(wx, y + ly, wz, sx, sy, sz)) continue;
        if (sx < lo) lo = sx;
        if (sx > hi) hi = sx;
        if (sy < ylo) ylo = sy;
        if (sy > yhi) yhi = sy;
    }
    g_stats.bike_x0 = static_cast<int16_t>(lo);
    g_stats.bike_x1 = static_cast<int16_t>(hi);
    g_stats.bike_y0 = static_cast<int16_t>(ylo);
    g_stats.bike_y1 = static_cast<int16_t>(yhi);
}

// A soft dark patch under the bike, which is what sells a flat ground.
void draw_shadow(float bike_x, float bike_z, float lift) {
    float half = 0.95f - lift * 0.30f;
    if (half < 0.45f) half = 0.45f;
    const float wx[4] = {bike_x - half, bike_x + half, bike_x + half,
                         bike_x - half};
    const float wy[4] = {0.02f, 0.02f, 0.02f, 0.02f};
    const float wz[4] = {bike_z - 0.30f, bike_z - 0.30f, bike_z + 0.30f,
                         bike_z + 0.30f};
    const uint8_t r[4] = {150, 150, 150, 150};
    const uint8_t g[4] = {120, 120, 120, 120};
    const uint8_t b[4] = {84, 84, 84, 84};
    quad_world(wx, wy, wz, r, g, b);
}

void spawn_dust(float x, float y, float z, int count) {
    for (auto& d : g_dust) {
        if (count <= 0) break;
        if (d.life > 0) continue;
        const uint32_t r = dust_rand();
        d.x = x + static_cast<float>(r & 31) / 200.0f;
        d.y = y + 0.05f + static_cast<float>((r >> 5) & 31) / 300.0f;
        d.z = z + static_cast<float>((r >> 10) & 31) / 300.0f - 0.05f;
        d.vx = -0.05f - static_cast<float>((r >> 15) & 15) / 220.0f;
        d.vy = 0.02f + static_cast<float>((r >> 19) & 15) / 700.0f;
        d.life = static_cast<uint8_t>(14 + ((r >> 23) & 15));
        count--;
    }
}

void draw_dust() {
    for (auto& d : g_dust) {
        if (d.life == 0) continue;
        d.life--;
        d.x += d.vx;
        d.y += d.vy;
        d.vy -= 0.001f;
        int sx, sy, sz;
        if (g_renderer.project(d.x, d.y, d.z, sx, sy, sz)) {
            const uint8_t shade = static_cast<uint8_t>(150 + d.life * 4);
            g_raster.plot(sx, sy, shade, clamp8(shade - 30),
                          clamp8(shade - 70));
            g_raster.plot(sx + 1, sy, clamp8(shade - 12), clamp8(shade - 40),
                          clamp8(shade - 80));
        }
    }
}

// Red edge strips when the window is about to eat the rider. This is the
// one piece of HUD the game needs mid run, so it lives with the scene.
void draw_edge_warnings(const World& world, uint32_t t) {
    if (!world.alive) return;
    const int32_t rel = world.x - world.screen_x;
    const int32_t danger_behind = rel + dr::k_window_half;   // 0 = dead
    const int32_t danger_ahead = dr::k_window_half - rel;
    const int flash = 150 + sin64(t * 6) / 3;
    const int height = g_raster.target().height;
    if (danger_behind < 320) {
        for (int y = 0; y < height; y += 2) {
            g_raster.plot(0, y, clamp8(flash + 60), 40, 30);
            g_raster.plot(1, y, clamp8(flash), 30, 25);
        }
    }
    if (danger_ahead < 320) {
        const int w = g_raster.target().width;
        for (int y = 0; y < height; y += 2) {
            g_raster.plot(w - 1, y, clamp8(flash + 60), 40, 30);
            g_raster.plot(w - 2, y, clamp8(flash), 30, 25);
        }
    }
}

}  // namespace

void render_scene(const World& world, const pse::RenderTarget& target,
                  uint32_t time_ms) {
    const uint32_t t = time_ms / 33;

    const float cam_x = fp_to_f(world.screen_x);
    const float bike_x = fp_to_f(world.x);
    const float bike_z = fp_to_f(world.z);

    // The camera tracks the bike's own z, not the road's, so a rider who
    // drifts into the sand stays framed and it is the ROAD that slides away
    // under them. That slide is the steering feedback.
    // The camera rides the ROAD, not the bike, so the world holds still and
    // the rider is what moves up and down the frame. Locking it to the bike
    // instead makes every steering input slide the entire desert, which is
    // disorienting precisely because it is the one thing on screen the
    // player expects to be able to trust.
    //
    // The clamp is the exception that keeps it safe. The bike may wander
    // metres either side of the centerline, and south is toward the lens,
    // so a purely road locked camera would eventually have the bike behind
    // its own near plane. Normal riding never reaches the clamp, so the
    // camera is genuinely still; only a deep excursion into the sand nudges
    // it, and only as far as it has to.
    const float road_z = center_at(world, cam_x) - k_cam_dist;
    const float nearest = bike_z - k_bike_depth_min;
    const float farthest = bike_z - k_bike_depth_max;
    g_cam_z = road_z > nearest ? nearest
                               : (road_z < farthest ? farthest : road_z);
    g_cam_seeded = true;
    const float cam_z = g_cam_z;

    g_raster.begin_frame_collect(target, g_queue);
    g_renderer.set_camera(cam_x, k_cam_up, cam_z, 0.0f, k_cam_pitch);

    const float x0 = std::floor(cam_x) - k_column_back;
    const float edge = fp_to_f(dr::k_road_half);

    draw_horizon(world, cam_x, cam_z);

    // The desert in four strips: foreground sand pinned to the camera, the
    // two road edges and the tarmac between them hugging the centerline,
    // then open desert running out to the horizon. Only the middle three
    // bend, which is exactly what makes the road read as the thing that
    // curves.
    const float near_ground = cam_z + k_near_ground;
    auto south_edge = [&](float x) { return center_at(world, x) - edge; };
    auto north_edge = [&](float x) { return center_at(world, x) + edge; };
    auto north_out = [&](float x) { return center_at(world, x) + 7.0f; };

    // The open desert runs out to the horizon, so it needs to be far wider
    // in x than the road does. Coarse columns: nothing out there bends.
    constexpr int k_far_cols = 16;
    constexpr float k_far_step = 7.0f;
    draw_strip(cam_x - 56.0f, k_far_cols, k_far_step, north_out,
               [&](float) { return cam_z + 44.0f; }, 222, 184, 124, -6, -20);

    draw_strip(x0, k_columns, 1.0f, [&](float) { return near_ground; },
               south_edge, 214, 176, 116, -16, 8);
    draw_strip(x0, k_columns, 1.0f, south_edge, north_edge,
               132, 128, 124, 6, -10);
    draw_strip(x0, k_columns, 1.0f, north_edge, north_out,
               226, 188, 126, 4, -10);

    // Centerline dashes, every other meter.
    {
        const int start = static_cast<int>(x0);
        for (int i = 0; i < k_columns; i++) {
            if ((start + i) & 1) continue;
            const float xa = x0 + i + 0.15f;
            const float xb = x0 + i + 0.85f;
            const float ca = center_at(world, xa);
            const float cb = center_at(world, xb);
            const float wx[4] = {xa, xb, xb, xa};
            const float wy[4] = {0.015f, 0.015f, 0.015f, 0.015f};
            const float wz[4] = {ca - 0.08f, cb - 0.08f, cb + 0.08f,
                                 ca + 0.08f};
            const uint8_t r[4] = {240, 240, 240, 240};
            const uint8_t g[4] = {232, 232, 232, 232};
            const uint8_t b[4] = {198, 198, 198, 198};
            quad_world(wx, wy, wz, r, g, b);
        }
    }

    draw_ground_detail(world, cam_x, cam_z);
    draw_rails(world, cam_x);
    draw_cacti(world, cam_x);

    // ---- the bike ----
    // Body pitch: the wheelie under throttle (strongest when pulling hard
    // at low speed), slumping nose down in a wreck.
    float target_pitch;
    if (!world.alive) {
        target_pitch = -0.85f;
    } else if (world.throttling) {
        const float headroom =
            1.0f - static_cast<float>(world.v) /
                       static_cast<float>(dr::k_bike_vmax);
        target_pitch = 0.10f + 0.42f * (headroom < 0.0f ? 0.0f : headroom);
    } else {
        target_pitch = 0.0f;
    }
    g_pitch += (target_pitch - g_pitch) * 0.14f;

    // A wheelie rotates about the REAR contact patch, not the bike's
    // centre: lift the model so the rear wheel stays planted while the nose
    // comes up. 0.62 is the rear axle's offset in the model.
    const float wheelie = g_pitch > 0.0f ? g_pitch : 0.0f;
    const float lift = 0.62f * std::sin(wheelie);

    draw_shadow(bike_x, bike_z, lift);
    g_renderer.draw_mesh(models::bike, bike_x, lift, bike_z,
                         k_pi / 2.0f, 1.0f, 255, 255, 255, g_pitch);
    measure_bike_span(bike_x, lift, bike_z, k_pi / 2.0f, g_pitch);

    g_stats.queued = g_queue.count;
    g_stats.dropped = g_queue.dropped;

    pse::run_split(g_raster, g_queue,
                   pse::SkyGradient{118, 174, 232, 248, 206, 152});
    g_raster.end_collect();

    // ---- immediate overlays ----
    if (world.alive && world.throttling && world.v > dr::k_bike_vmax / 8) {
        spawn_dust(bike_x - 0.85f, 0.0f, bike_z, dr::off_road(world) ? 3 : 2);
    }
    if (!world.alive) {
        spawn_dust(bike_x, 0.2f, bike_z, 1);
    }
    draw_dust();
    draw_edge_warnings(world, t);
}

FrameStats last_frame_stats() { return g_stats; }

}  // namespace drr
