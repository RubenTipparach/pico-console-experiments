#include "render.hpp"

#include <cmath>

#include "pse/parallel.hpp"
#include "pse/raster.hpp"
#include "pse/renderer3d.hpp"

#include "boat.hpp"
#include "bobber.hpp"
#include "fish.hpp"

namespace kfr {
namespace {

using kf::World;

// Rendering state. Static because dynamic allocation is banned, and it is the
// documented RAM cost of drawing this game:
//   Rasterizer  ~14.5 KB (depth buffer)
//   FrameQueue  ~15.4 KB (640 triangles for the two core split)
//   the rest    well under 1 KB
pse::Rasterizer g_raster;
pse::Renderer3D g_renderer(g_raster);
pse::FrameQueue g_queue;

// Cosmetic surface effects. Presentation only, so they live here and not in
// the sim: replaying the same sim produces the same catches even though the
// droplets differ.
constexpr int k_ripple_life = 40;
struct Ripple {
    int16_t x, z;      // fp8 world
    uint8_t age = k_ripple_life;   // counts up, dead at k_ripple_life
    uint8_t big = 0;
};
constexpr int k_max_ripples = 14;
Ripple g_ripples[k_max_ripples];
uint8_t g_dip_timer = 0;   // bobber dip after a nibble

// Quarter resolution sine, s8 amplitude 127, 64 entries per turn.
const int8_t k_sin[64] = {
    0, 12, 24, 36, 48, 59, 70, 80, 89, 98, 105, 112, 117, 121, 124, 126,
    127, 126, 124, 121, 117, 112, 105, 98, 89, 80, 70, 59, 48, 36, 24, 12,
    0, -12, -24, -36, -48, -59, -70, -80, -89, -98, -105, -112, -117, -121,
    -124, -126, -127, -126, -124, -121, -117, -112, -105, -98, -89, -80,
    -70, -59, -48, -36, -24, -12,
};

inline int sin64(uint32_t index) { return k_sin[index & 63]; }

inline uint8_t lerp8(uint8_t a, uint8_t b, int t256) {
    return static_cast<uint8_t>(a + ((b - a) * t256) / 256);
}

// Day cycle palette keyframes. Phase 0 is dawn; night begins at 128.
struct SkyKey {
    uint8_t phase;
    uint8_t sky_r, sky_g, sky_b;
    uint8_t hor_r, hor_g, hor_b;
    uint8_t wat_r, wat_g, wat_b;
    uint8_t light;   // 0..255 overall brightness for meshes
};
const SkyKey k_sky[] = {
    {0,   235, 140, 90,   250, 190, 140,   60, 75, 95,    200},
    {28,  95, 155, 225,   170, 210, 240,   35, 95, 120,   255},
    {100, 95, 155, 225,   170, 210, 240,   35, 95, 120,   255},
    {128, 95, 60, 110,    235, 130, 85,    45, 55, 85,    190},
    {150, 12, 16, 42,     30, 40, 80,      14, 28, 48,    110},
    {230, 12, 16, 42,     30, 40, 80,      14, 28, 48,    110},
    {255, 235, 140, 90,   250, 190, 140,   60, 75, 95,    200},
};

SkyKey sky_now(const World& world) {
    const int phase = kf::day_phase(world);
    const int count = static_cast<int>(sizeof(k_sky) / sizeof(k_sky[0]));
    for (int i = 0; i < count - 1; i++) {
        const SkyKey& a = k_sky[i];
        const SkyKey& b = k_sky[i + 1];
        if (phase < a.phase || phase > b.phase) continue;
        const int span = b.phase - a.phase;
        const int t = span > 0 ? ((phase - a.phase) * 256) / span : 0;
        SkyKey out;
        out.phase = static_cast<uint8_t>(phase);
        out.sky_r = lerp8(a.sky_r, b.sky_r, t);
        out.sky_g = lerp8(a.sky_g, b.sky_g, t);
        out.sky_b = lerp8(a.sky_b, b.sky_b, t);
        out.hor_r = lerp8(a.hor_r, b.hor_r, t);
        out.hor_g = lerp8(a.hor_g, b.hor_g, t);
        out.hor_b = lerp8(a.hor_b, b.hor_b, t);
        out.wat_r = lerp8(a.wat_r, b.wat_r, t);
        out.wat_g = lerp8(a.wat_g, b.wat_g, t);
        out.wat_b = lerp8(a.wat_b, b.wat_b, t);
        out.light = lerp8(a.light, b.light, t);
        return out;
    }
    return k_sky[0];
}

inline float fp_to_f(int32_t v) { return static_cast<float>(v) / 256.0f; }

// Water surface height in fp8 at a grid point, animated. Small integers in,
// small integers out, no overflow anywhere.
int wave_fp(int gx, int gz, uint32_t t, bool raining) {
    const int amp = raining ? 22 : 12;
    const int a = sin64(gx * 5 + (t >> 1));
    const int b = sin64(gz * 7 + gx * 3 + (t / 3));
    return ((a + b) * amp) >> 8;
}

// The water is a 10x9 grid of vertices ahead of the boat, two triangles per
// cell, coloured per vertex: deeper water is darker, crests catch the light.
// 144 triangles, the single biggest fill cost in the frame, which is exactly
// what the two core split exists for.
constexpr int k_wx = 10;
constexpr int k_wz = 9;

void draw_far_water(const SkyKey& sky) {
    // Same shade as the animated grid's farthest row, so the seam vanishes.
    const int shade = 256 - ((k_wz - 1) * 130) / (k_wz + 1);
    const uint8_t r = static_cast<uint8_t>(sky.wat_r * shade / 256 * sky.light / 256);
    const uint8_t g = static_cast<uint8_t>(sky.wat_g * shade / 256 * sky.light / 256);
    const uint8_t b = static_cast<uint8_t>(sky.wat_b * shade / 256 * sky.light / 256);

    int sx[4], sy[4], sz[4];
    const float xs[4] = {-60.0f, 60.0f, 60.0f, -60.0f};
    const float zs[4] = {14.5f, 14.5f, 70.0f, 70.0f};
    for (int i = 0; i < 4; i++) {
        if (!g_renderer.project(xs[i], 0.0f, zs[i], sx[i], sy[i], sz[i])) {
            return;
        }
    }
    pse::ScreenTriangle t1;
    t1.x0 = static_cast<int16_t>(sx[0]); t1.y0 = static_cast<int16_t>(sy[0]);
    t1.x1 = static_cast<int16_t>(sx[1]); t1.y1 = static_cast<int16_t>(sy[1]);
    t1.x2 = static_cast<int16_t>(sx[2]); t1.y2 = static_cast<int16_t>(sy[2]);
    t1.z0 = static_cast<uint16_t>(sz[0]);
    t1.z1 = static_cast<uint16_t>(sz[1]);
    t1.z2 = static_cast<uint16_t>(sz[2]);
    t1.r0 = t1.r1 = t1.r2 = r;
    t1.g0 = t1.g1 = t1.g2 = g;
    t1.b0 = t1.b1 = t1.b2 = b;
    g_raster.draw(t1);
    pse::ScreenTriangle t2 = t1;
    t2.x1 = static_cast<int16_t>(sx[2]); t2.y1 = static_cast<int16_t>(sy[2]);
    t2.z1 = static_cast<uint16_t>(sz[2]);
    t2.x2 = static_cast<int16_t>(sx[3]); t2.y2 = static_cast<int16_t>(sy[3]);
    t2.z2 = static_cast<uint16_t>(sz[3]);
    g_raster.draw(t2);
}

void draw_water(const World& world, const SkyKey& sky, uint32_t t) {
    int sx[k_wx * k_wz];
    int sy[k_wx * k_wz];
    int sz[k_wx * k_wz];
    bool ok[k_wx * k_wz];
    uint8_t cr[k_wx * k_wz], cg[k_wx * k_wz], cb[k_wx * k_wz];

    for (int j = 0; j < k_wz; j++) {
        for (int i = 0; i < k_wx; i++) {
            const int idx = j * k_wx + i;
            // Widen with distance so the far edge spans the frustum rather
            // than pinching into a dome on the horizon.
            const int32_t wx_fp = (2 * i - (k_wx - 1)) * (320 + j * 60);
            const int32_t wz_fp = -256 + j * 512;               // -1 .. 15
            const int h = wave_fp(i, j, t, world.raining != 0);
            ok[idx] = g_renderer.project(fp_to_f(wx_fp), fp_to_f(h),
                                         fp_to_f(wz_fp),
                                         sx[idx], sy[idx], sz[idx]);
            // Darker with distance, lighter on crests.
            const int depth_shade = 256 - (j * 130) / (k_wz + 1);
            const int crest = h > 0 ? h * 6 : 0;
            const int light = sky.light;
            int r = (sky.wat_r * depth_shade / 256) + crest;
            int g = (sky.wat_g * depth_shade / 256) + crest;
            int b = (sky.wat_b * depth_shade / 256) + crest / 2;
            cr[idx] = static_cast<uint8_t>(r * light / 256 > 255 ? 255 : r * light / 256);
            cg[idx] = static_cast<uint8_t>(g * light / 256 > 255 ? 255 : g * light / 256);
            cb[idx] = static_cast<uint8_t>(b * light / 256 > 255 ? 255 : b * light / 256);
        }
    }

    for (int j = 0; j < k_wz - 1; j++) {
        for (int i = 0; i < k_wx - 1; i++) {
            const int a = j * k_wx + i;
            const int b = j * k_wx + i + 1;
            const int c = (j + 1) * k_wx + i + 1;
            const int d = (j + 1) * k_wx + i;
            if (!ok[a] || !ok[b] || !ok[c] || !ok[d]) continue;

            pse::ScreenTriangle t1;
            t1.x0 = static_cast<int16_t>(sx[a]); t1.y0 = static_cast<int16_t>(sy[a]);
            t1.x1 = static_cast<int16_t>(sx[b]); t1.y1 = static_cast<int16_t>(sy[b]);
            t1.x2 = static_cast<int16_t>(sx[c]); t1.y2 = static_cast<int16_t>(sy[c]);
            t1.z0 = static_cast<uint16_t>(sz[a]);
            t1.z1 = static_cast<uint16_t>(sz[b]);
            t1.z2 = static_cast<uint16_t>(sz[c]);
            t1.r0 = cr[a]; t1.g0 = cg[a]; t1.b0 = cb[a];
            t1.r1 = cr[b]; t1.g1 = cg[b]; t1.b1 = cb[b];
            t1.r2 = cr[c]; t1.g2 = cg[c]; t1.b2 = cb[c];
            g_raster.draw(t1);

            pse::ScreenTriangle t2 = t1;
            t2.x1 = static_cast<int16_t>(sx[c]); t2.y1 = static_cast<int16_t>(sy[c]);
            t2.z1 = static_cast<uint16_t>(sz[c]);
            t2.r1 = cr[c]; t2.g1 = cg[c]; t2.b1 = cb[c];
            t2.x2 = static_cast<int16_t>(sx[d]); t2.y2 = static_cast<int16_t>(sy[d]);
            t2.z2 = static_cast<uint16_t>(sz[d]);
            t2.r2 = cr[d]; t2.g2 = cg[d]; t2.b2 = cb[d];
            g_raster.draw(t2);
        }
    }
}

void spawn_ripple(int32_t x_fp, int32_t z_fp, bool big) {
    for (auto& r : g_ripples) {
        if (r.age < k_ripple_life) continue;
        r.x = static_cast<int16_t>(x_fp);
        r.z = static_cast<int16_t>(z_fp);
        r.age = 0;
        r.big = big ? 1 : 0;
        return;
    }
}

void draw_ripples(const SkyKey& sky) {
    for (auto& r : g_ripples) {
        if (r.age >= k_ripple_life) continue;
        r.age++;
        const float cx = fp_to_f(r.x);
        const float cz = fp_to_f(r.z);
        const float radius = (r.big ? 0.06f : 0.03f) * r.age;
        const int fade = 255 - (r.age * 255) / k_ripple_life;
        const uint8_t cr2 = lerp8(sky.wat_r, 255, fade / 2);
        const uint8_t cg2 = lerp8(sky.wat_g, 255, fade / 2);
        const uint8_t cb2 = lerp8(sky.wat_b, 255, fade / 2);
        for (int k = 0; k < 12; k++) {
            const float px = cx + radius * (sin64(k * 5 + 16) / 127.0f);
            const float pz = cz + radius * (sin64(k * 5) / 127.0f);
            int sx2, sy2, sz2;
            if (g_renderer.project(px, 0.02f, pz, sx2, sy2, sz2)) {
                g_raster.plot(sx2, sy2, cr2, cg2, cb2);
            }
        }
    }
}

void draw_rain(const World& world, uint32_t t) {
    if (!world.raining) return;
    // Streaks in the sky region. Positions come from a hash so there is no
    // rain state to store.
    for (int i = 0; i < 18; i++) {
        uint32_t h = (i * 2654435761u) ^ ((t / 2 + i * 13) * 40503u);
        const int x = static_cast<int>(h % 120);
        const int y = static_cast<int>((h >> 8) % 46) + ((t * 3 + i) % 8);
        g_raster.plot(x, y, 150, 170, 200);
        g_raster.plot(x, y + 1, 120, 140, 180);
    }
}

void draw_line_screen(int x0, int y0, int x1, int y1,
                      uint8_t r, uint8_t g, uint8_t b) {
    // Bresenham, bounded: the endpoints come from project() so they are near
    // screen space already.
    int dx = x1 - x0, dy = y1 - y0;
    const int steps = (dx < 0 ? -dx : dx) > (dy < 0 ? -dy : dy)
                          ? (dx < 0 ? -dx : dx) : (dy < 0 ? -dy : dy);
    if (steps == 0 || steps > 240) return;
    for (int i = 0; i <= steps; i++) {
        g_raster.plot(x0 + dx * i / steps, y0 + dy * i / steps, r, g, b);
    }
}

void draw_bar(int x, int y_top, int height, int filled, uint8_t r, uint8_t g,
              uint8_t b) {
    for (int y = 0; y < height; y++) {
        for (int x2 = 0; x2 < 4; x2++) {
            const bool on = (height - 1 - y) < filled;
            if (on) {
                g_raster.plot(x + x2, y_top + y, r, g, b);
            } else if (x2 == 0 || x2 == 3 || y == 0 || y == height - 1) {
                g_raster.plot(x + x2, y_top + y, 30, 30, 45);
            }
        }
    }
}

// Fish tint: species colour washed toward the water colour with depth, so a
// deep fish reads as a shadow and a surfacing fish gains its real colours.
void fish_tint(const kf::Fish& fish, const SkyKey& sky, bool surfaced,
               uint8_t& r, uint8_t& g, uint8_t& b) {
    const kf::Species& s = kf::k_species[fish.species];
    if (surfaced) {
        r = s.r; g = s.g; b = s.b;
        return;
    }
    int depth = static_cast<int>(fish.y) - 32;
    if (depth < 0) depth = 0;
    if (depth > 480) depth = 480;
    const int t = (depth * 200) / 480;   // never fully invisible
    r = lerp8(s.r, sky.wat_r / 2, t);
    g = lerp8(s.g, sky.wat_g / 2, t);
    b = lerp8(s.b, sky.wat_b / 2, t);
}

}  // namespace

void render_scene(const World& world, const pse::RenderTarget& target,
                  uint32_t time_ms) {
    const uint32_t t = time_ms / 33;   // animation clock, ~30 steps a second
    const SkyKey sky = sky_now(world);

    // Camera. A slow sway keeps the water alive; fights pull the view toward
    // the fish; the catch card gets a close orbit around the trophy.
    float cam_tx = 0.0f, cam_ty = 0.0f, cam_tz = 5.6f;
    float yaw = sin64(t / 4) / 2200.0f;
    float dist = 8.2f, height = 3.8f;
    if (world.mode == kf::Mode::Fight && world.hooked_fish >= 0) {
        cam_tx = fp_to_f(world.fish[world.hooked_fish].x) * 0.4f;
        cam_tz = 4.6f + fp_to_f(world.fish[world.hooked_fish].z) * 0.15f;
    } else if (world.mode == kf::Mode::Landed) {
        cam_tx = 0.0f; cam_ty = 0.6f; cam_tz = 3.1f;
        dist = 3.2f; height = 1.2f;
        yaw = 0.0f;
    }
    g_renderer.set_orbit_camera(cam_tx, cam_ty, cam_tz, yaw, dist, height);

    g_raster.begin_frame_collect(target, g_queue);

    draw_far_water(sky);
    draw_water(world, sky, t);

    // Boat, bobbing on the same water function so it sits in the waves.
    const float boat_bob = wave_fp(4, 1, t, world.raining != 0) / 256.0f;
    g_renderer.draw_mesh(models::boat, 0.0f, 0.02f + boat_bob, 0.2f, 0.0f,
                         0.85f, sky.light, sky.light, sky.light);

    // Fish. Shadows under the surface, real colours when they breach.
    for (const auto& fish : world.fish) {
        if (fish.state == kf::FishState::Gone) continue;

        float y = 0.045f;
        bool surfaced = false;
        if (fish.state == kf::FishState::Hooked && world.leap_timer > 0) {
            const int lt = world.leap_timer;
            y = 0.045f + (lt * (45 - lt)) / 900.0f;   // up to ~0.56 units
            surfaced = true;
        } else if (world.mode == kf::Mode::Landed) {
            continue;   // the trophy is drawn separately
        }

        uint8_t r, g, b;
        fish_tint(fish, sky, surfaced, r, g, b);

        float fyaw;
        if (fish.state == kf::FishState::Hooked) {
            fyaw = atan2f(fp_to_f(fish.x), fp_to_f(fish.z));
        } else {
            fyaw = atan2f(static_cast<float>(fish.vx),
                          static_cast<float>(fish.vz == 0 ? 1 : fish.vz));
        }
        const float scale = 0.30f + fish.size_cm * 0.006f;
        g_renderer.draw_mesh(models::fish, fp_to_f(fish.x), y, fp_to_f(fish.z),
                             fyaw, scale, r, g, b);
    }

    // Trophy shot while the card is up.
    if (world.mode == kf::Mode::Landed && world.card_species >= 0) {
        const kf::Species& s = kf::k_species[world.card_species];
        const float scale = 0.30f + world.card_size * 0.006f;
        g_renderer.draw_mesh(models::fish, 0.0f, 0.55f, 3.1f,
                             t * 0.05f, scale, s.r, s.g, s.b);
    }

    // Bobber.
    const bool lure_visible = world.mode == kf::Mode::Flying ||
                              world.mode == kf::Mode::Sinking ||
                              world.mode == kf::Mode::Fight;
    float bob_y = 0.0f;
    if (lure_visible) {
        if (world.mode == kf::Mode::Flying) {
            bob_y = -fp_to_f(world.lure_y);   // sim y is down
        } else {
            bob_y = 0.05f + wave_fp(2, 3, t, world.raining != 0) / 256.0f;
            if (g_dip_timer > 0) bob_y -= 0.10f;
            for (const auto& fish : world.fish) {
                if (fish.state == kf::FishState::Biting) bob_y -= 0.22f;
            }
        }
        g_renderer.draw_mesh(models::bobber, fp_to_f(world.lure_x), bob_y,
                             fp_to_f(world.lure_z), 0.0f, 0.22f,
                             sky.light, sky.light, sky.light);
    }

    // Everything above went into the queue; this is the frame's parallel part.
    pse::run_split(g_raster, g_queue,
                   pse::SkyGradient{sky.sky_r, sky.sky_g, sky.sky_b,
                                    sky.hor_r, sky.hor_g, sky.hor_b});
    g_raster.end_collect();

    // Overlay pass, immediate mode on top of the finished frame.
    if (g_dip_timer > 0) g_dip_timer--;

    // Presentation reactions to this tick's events.
    if (world.ev.splash) spawn_ripple(static_cast<int16_t>(world.lure_x),
                                      static_cast<int16_t>(world.lure_z), true);
    if (world.ev.nibble) {
        spawn_ripple(static_cast<int16_t>(world.lure_x),
                     static_cast<int16_t>(world.lure_z), false);
        g_dip_timer = 14;
    }
    if (world.ev.leap && world.hooked_fish >= 0) {
        spawn_ripple(static_cast<int16_t>(world.fish[world.hooked_fish].x),
                     static_cast<int16_t>(world.fish[world.hooked_fish].z),
                     true);
    }

    draw_ripples(sky);
    draw_rain(world, t);

    // The line, rod tip to lure.
    if (lure_visible) {
        int rx, ry, rz, lx, ly, lz;
        const bool rod_ok = g_renderer.project(0.18f, 0.95f, 1.55f, rx, ry, rz);
        const bool lure_ok = g_renderer.project(
            fp_to_f(world.lure_x), bob_y + 0.08f, fp_to_f(world.lure_z),
            lx, ly, lz);
        if (rod_ok && lure_ok) {
            uint8_t lr = 210, lg = 210, lb = 215;
            if (world.mode == kf::Mode::Fight) {
                lr = world.tension >= 800 ? 240 : 160;
                lg = world.tension >= 800 ? 70 : 220;
                lb = 80;
            }
            draw_line_screen(rx, ry, lx, ly, lr, lg, lb);
        }
    }

    // Bite mark: one glyph, no words. Gated on an actual biting fish as well
    // as the timer, so a stale timer can never show a phantom alert.
    bool bite_open = false;
    for (const auto& fish : world.fish) {
        if (fish.state == kf::FishState::Biting) bite_open = true;
    }
    if (bite_open && world.bite_timer > 0 && world.mode == kf::Mode::Sinking) {
        int bx, by, bz;
        if (g_renderer.project(fp_to_f(world.lure_x), 0.9f,
                               fp_to_f(world.lure_z), bx, by, bz)) {
            for (int y = 0; y < 5; y++) {
                g_raster.plot(bx, by + y - 9, 255, 60, 60);
                g_raster.plot(bx + 1, by + y - 9, 255, 60, 60);
            }
            g_raster.plot(bx, by - 2, 255, 60, 60);
            g_raster.plot(bx + 1, by - 2, 255, 60, 60);
        }
    }

    // Power meter while aiming, tension bar while fighting. Nothing else.
    if (world.mode == kf::Mode::Aiming) {
        draw_bar(5, 30, 62, (world.power * 62) / 255, 250, 210, 90);
    }
    if (world.mode == kf::Mode::Fight) {
        const int fill = (world.tension * 62) / 1023;
        uint8_t r = 90, g = 220, b = 110;
        if (world.tension >= 800) { r = 240; g = 70; b = 70; }
        else if (world.tension >= 500) { r = 240; g = 200; b = 80; }
        draw_bar(111, 30, 62, fill, r, g, b);
    }
}

}  // namespace kfr
