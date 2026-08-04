#include "render.hpp"

#include <cmath>

#include "pse/parallel.hpp"
#include "pse/raster.hpp"
#include "pse/renderer3d.hpp"

#include "bike.hpp"
#include "cactus.hpp"

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

// The camera hangs on the near side of the road (negative z, looking +z),
// which puts world +x on screen right: the bike rides rightward. It sits
// high enough that the road and the sand shoulder behind it read as two
// distinct lanes.
constexpr float k_cam_z = -5.8f;
constexpr float k_cam_up = 3.9f;
constexpr float k_cam_pitch = -0.50f;

// Presentation state: eased camera height and bike body pitch, and the dust
// kicked up by the rear wheel. Replaying a run reproduces the sim exactly
// even though the dust differs.
float g_cam_y = k_cam_up;
float g_pitch = 0.0f;
bool g_cam_seeded = false;

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
// counter clockwise seen from the camera side: (x0,z0) (x1,z0) (x1,z1)
// (x0,z1) with y per corner.
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

// One longitudinal ground band: for each 1 m column the band spans z_near to
// z_far at the track's height, shaded a touch brighter on climbs so the
// dunes read as lit from ahead.
void draw_band(const World& world, float x0, int columns,
               float z_near, float z_far,
               uint8_t base_r, uint8_t base_g, uint8_t base_b,
               int shade_near, int shade_far) {
    for (int i = 0; i < columns; i++) {
        const float xa = x0 + static_cast<float>(i);
        const float xb = xa + 1.0f;
        const int32_t fa = static_cast<int32_t>(xa * 256.0f);
        const int32_t fb = static_cast<int32_t>(xb * 256.0f);
        const float ha = fp_to_f(dr::track_height(world, fa));
        const float hb = fp_to_f(dr::track_height(world, fb));
        const int slope_a = dr::track_slope(world, fa) / 4;
        const int slope_b = dr::track_slope(world, fb) / 4;

        const float wx[4] = {xa, xb, xb, xa};
        const float wy[4] = {ha, hb, hb, ha};
        const float wz[4] = {z_near, z_near, z_far, z_far};
        uint8_t r[4], g[4], b[4];
        const int lift[4] = {slope_a + shade_near, slope_b + shade_near,
                             slope_b + shade_far, slope_a + shade_far};
        for (int c = 0; c < 4; c++) {
            r[c] = clamp8(base_r + lift[c]);
            g[c] = clamp8(base_g + lift[c]);
            b[c] = clamp8(base_b + lift[c] / 2);
        }
        quad_world(wx, wy, wz, r, g, b);
    }
}

// The dune backdrop past the road: a skyline WALL, not a floor. A distant
// horizontal surface projects to a floating sliver from this camera; a
// vertical silhouette at fixed depth always reads as a dune ridge on the
// horizon. Two walls at different depths give the horizon parallax.
void draw_far_dunes(const World& world, float cam_x) {
    struct Ridge {
        float z, amp_div, base, phase;
        uint8_t r, g, b;
    };
    static const Ridge k_ridges[2] = {
        {21.0f, 34.0f, 3.6f, 11.0f, 228, 190, 148},
        {13.0f, 44.0f, 2.2f, 0.0f, 210, 164, 116},
    };
    for (const Ridge& ridge : k_ridges) {
        const float x0 = std::floor((cam_x - 26.0f) / 2.0f) * 2.0f;
        for (int i = 0; i < 27; i++) {
            const float xa = x0 + i * 2.0f;
            const float xb = xa + 2.0f;
            auto ridge_h = [&](float x) {
                const int32_t fx = static_cast<int32_t>(x * 256.0f);
                const float base =
                    fp_to_f(dr::track_height(world, fx)) * 0.4f;
                const int32_t k =
                    static_cast<int32_t>(x * 0.5f + ridge.phase);
                return base + ridge.base + sin64(k) / ridge.amp_div +
                       sin64(k * 3 + 17) / (ridge.amp_div * 2.4f);
            };
            const float ha = ridge_h(xa);
            const float hb = ridge_h(xb);
            const float wx[4] = {xa, xb, xb, xa};
            const float wy[4] = {ha - 12.0f, hb - 12.0f, hb, ha};
            const float wz[4] = {ridge.z, ridge.z, ridge.z, ridge.z};
            const uint8_t r[4] = {ridge.r, ridge.r, ridge.r, ridge.r};
            const uint8_t g[4] = {ridge.g, ridge.g, ridge.g, ridge.g};
            const uint8_t b[4] = {ridge.b, ridge.b, ridge.b, ridge.b};
            quad_world(wx, wy, wz, r, g, b);
        }
    }
}

// Guardrail as terrain hugging quads: a bright beam over dark posts, on the
// painted edge between road and shoulder.
void draw_rails(const World& world, float cam_x) {
    const int32_t first =
        (static_cast<int32_t>((cam_x - 10.0f) * 256.0f)) >> dr::k_chunk_shift;
    for (int i = 0; i < 11; i++) {
        const int32_t index = first + i;
        if (index < 0) continue;
        const int32_t x_fp = index << dr::k_chunk_shift;
        if (!dr::track_rail_at(world, x_fp + 1)) continue;
        const float xa = fp_to_f(x_fp);
        const float xb = xa + 2.0f;
        const float ha = fp_to_f(dr::track_height(world, x_fp));
        const float hb =
            fp_to_f(dr::track_height(world, x_fp + dr::k_chunk_len));

        // Beam, facing the camera: tall and bright so the lane lock is
        // legible from across the screen.
        {
            const float wx[4] = {xa, xb, xb, xa};
            const float wy[4] = {ha + 0.34f, hb + 0.34f, hb + 0.78f,
                                 ha + 0.78f};
            const float wz[4] = {1.5f, 1.5f, 1.5f, 1.5f};
            const uint8_t r[4] = {200, 200, 244, 244};
            const uint8_t g[4] = {200, 200, 244, 244};
            const uint8_t b[4] = {204, 204, 250, 250};
            quad_world(wx, wy, wz, r, g, b);
        }
        // Two posts per tile.
        for (int p = 0; p < 2; p++) {
            const float px = xa + 0.5f + p;
            const float ph =
                fp_to_f(dr::track_height(
                    world, static_cast<int32_t>(px * 256.0f)));
            const float wx[4] = {px - 0.08f, px + 0.08f, px + 0.08f,
                                 px - 0.08f};
            const float wy[4] = {ph, ph, ph + 0.40f, ph + 0.40f};
            const float wz[4] = {1.5f, 1.5f, 1.5f, 1.5f};
            const uint8_t r[4] = {88, 88, 60, 60};
            const uint8_t g[4] = {74, 74, 50, 50};
            const uint8_t b[4] = {62, 62, 42, 42};
            quad_world(wx, wy, wz, r, g, b);
        }
    }
}

void draw_cacti(const World& world, float cam_x) {
    const int32_t from =
        static_cast<int32_t>((cam_x - 8.0f) * 256.0f);
    int32_t scan = from < 0 ? 0 : from;
    const int32_t limit = static_cast<int32_t>((cam_x + 10.0f) * 256.0f);
    for (int guard = 0; guard < 12 && scan < limit; guard++) {
        int32_t cx;
        bool sand;
        if (!dr::track_next_cactus(world, scan, limit - scan, cx, sand)) break;
        const float x = fp_to_f(cx);
        const float h = fp_to_f(dr::track_height(world, cx));
        const float z = sand ? fp_to_f(dr::k_lane_sand_z)
                             : fp_to_f(dr::k_lane_road_z);
        // Size and facing vary by position, not by randomness, so a chunk
        // renders identically every frame it is on screen.
        const uint32_t hash = static_cast<uint32_t>(cx) * 2654435761u;
        const float yaw = static_cast<float>(hash & 255) * 0.02f;
        const float scale = 0.85f + static_cast<float>((hash >> 8) & 63) / 160.0f;
        g_renderer.draw_mesh(models::cactus, x, h, z, yaw, scale);
        scan = cx + 1;
    }
}

// A soft dark patch under the bike. It stays on the ground when the bike
// flies, which is the landing aid.
void draw_shadow(const World& world, float bike_x, float bike_z) {
    const int32_t fx = static_cast<int32_t>(bike_x * 256.0f);
    const float h = fp_to_f(dr::track_height(world, fx)) + 0.03f;
    const float air =
        fp_to_f((world.y16 >> 8) - dr::track_height(world, world.x));
    float half = 0.95f - air * 0.06f;
    if (half < 0.30f) half = 0.30f;
    const float wx[4] = {bike_x - half, bike_x + half, bike_x + half,
                         bike_x - half};
    const float wy[4] = {h, h, h, h};
    const float wz[4] = {bike_z - 0.30f, bike_z - 0.30f, bike_z + 0.30f,
                         bike_z + 0.30f};
    const uint8_t r[4] = {132, 132, 132, 132};
    const uint8_t g[4] = {104, 104, 104, 104};
    const uint8_t b[4] = {70, 70, 70, 70};
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
            g_raster.plot(sx, sy, shade, clamp8(shade - 30), clamp8(shade - 70));
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
    const int32_t danger_behind = rel + dr::k_window_half;     // 0 = dead
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
    const float ground_at_cam =
        fp_to_f(dr::track_height(world, world.screen_x));
    if (!g_cam_seeded) {
        g_cam_y = ground_at_cam + k_cam_up;
        g_cam_seeded = true;
    }
    g_cam_y += (ground_at_cam + k_cam_up - g_cam_y) * 0.10f;

    g_raster.begin_frame_collect(target, g_queue);
    g_renderer.set_camera(cam_x, g_cam_y, k_cam_z, 0.0f, k_cam_pitch);

    const float x0 = std::floor(cam_x) - 10.0f;
    draw_far_dunes(world, cam_x);
    // Foreground sand between camera and road, the road itself, and the
    // shoulder the rider can bail onto.
    // Sun bleached asphalt, light enough that the bike's dark tires read
    // against it.
    draw_band(world, x0, 21, -5.0f, -1.5f, 208, 168, 110, -14, 6);
    draw_band(world, x0, 21, -1.5f, 1.5f, 138, 132, 126, 5, -8);
    draw_band(world, x0, 21, 1.5f, 6.0f, 226, 188, 126, 2, -12);

    // Centerline dashes, every other meter.
    {
        const int start = static_cast<int>(x0);
        for (int i = 0; i < 21; i++) {
            if ((start + i) & 1) continue;
            const float xa = x0 + i + 0.15f;
            const float xb = x0 + i + 0.85f;
            const int32_t fa = static_cast<int32_t>(xa * 256.0f);
            const int32_t fb = static_cast<int32_t>(xb * 256.0f);
            const float ha = fp_to_f(dr::track_height(world, fa)) + 0.015f;
            const float hb = fp_to_f(dr::track_height(world, fb)) + 0.015f;
            const float wx[4] = {xa, xb, xb, xa};
            const float wy[4] = {ha, hb, hb, ha};
            const float wz[4] = {-0.08f, -0.08f, 0.08f, 0.08f};
            const uint8_t r[4] = {240, 240, 240, 240};
            const uint8_t g[4] = {232, 232, 232, 232};
            const uint8_t b[4] = {198, 198, 198, 198};
            quad_world(wx, wy, wz, r, g, b);
        }
    }

    draw_rails(world, cam_x);
    draw_cacti(world, cam_x);

    // ---- the bike ----
    const float bike_x = fp_to_f(world.x);
    const float bike_y = static_cast<float>(world.y16) / 65536.0f;
    const float bike_z = fp_to_f(world.z);
    draw_shadow(world, bike_x, bike_z);

    // Body pitch: follow the hill, add the wheelie under throttle (strongest
    // when pulling hard at low speed), hold a light nose up in the air, and
    // slump nose down in a wreck.
    const float slope_pitch = std::atan(
        static_cast<float>(dr::track_slope(world, world.x)) / 256.0f);
    float target_pitch;
    if (!world.alive) {
        target_pitch = -0.85f;
    } else if (!world.grounded) {
        target_pitch = 0.18f;
    } else {
        target_pitch = slope_pitch;
        if (world.throttling) {
            const float headroom =
                1.0f - static_cast<float>(world.v) /
                           static_cast<float>(dr::k_bike_vmax);
            target_pitch += 0.10f + 0.42f * (headroom < 0.0f ? 0.0f : headroom);
        }
    }
    g_pitch += (target_pitch - g_pitch) * 0.14f;

    // A wheelie rotates about the REAR contact patch, not the bike's
    // centre: lift the model so the rear wheel stays planted while the
    // nose comes up. 0.62 is the rear axle's offset in the model.
    float wheelie = g_pitch - (world.grounded ? slope_pitch : g_pitch);
    if (wheelie < 0.0f) wheelie = 0.0f;
    const float lift = 0.62f * std::sin(wheelie);

    g_renderer.draw_mesh(models::bike, bike_x, bike_y + lift, bike_z,
                         k_pi / 2.0f, 1.0f, 255, 255, 255, g_pitch);

    pse::run_split(g_raster, g_queue,
                   pse::SkyGradient{118, 174, 232, 248, 206, 152});
    g_raster.end_collect();

    // ---- immediate overlays ----
    if (world.alive && world.grounded && world.throttling &&
        world.v > dr::k_bike_vmax / 8) {
        spawn_dust(bike_x - 0.85f, bike_y, bike_z, 2);
    }
    if (!world.alive) {
        spawn_dust(bike_x, bike_y + 0.2f, bike_z, 1);
    }
    if (world.ev.landed) {
        spawn_dust(bike_x - 0.4f, bike_y, bike_z, 6);
    }
    draw_dust();
    draw_edge_warnings(world, t);
}

}  // namespace drr
