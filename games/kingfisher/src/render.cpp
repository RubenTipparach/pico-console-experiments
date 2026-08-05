#include "render.hpp"

#include <cmath>

#include "pse/parallel.hpp"
#include "pse/raster.hpp"
#include "pse/renderer3d.hpp"
#include "pse/shared_render.hpp"

#include "boat.hpp"
#include "bobber.hpp"
#include "fish.hpp"
#include "tree.hpp"

namespace kfr {
namespace {

using kf::World;

// Rendering state. The Rasterizer and the FrameQueue come from the engine
// rather than being declared here: on the console every game is linked into
// one binary, and a 14 KB depth buffer plus a 15 KB triangle queue per game
// is RAM spent on scenes that nothing is rendering. Only one game runs at a
// time and none of this survives leaving it. A standalone build of this game
// gets exactly one instance, same as it always had.
//   Rasterizer  ~14.4 KB (depth buffer)   shared
//   FrameQueue  ~15.4 KB (640 triangles)  shared
//   the rest    well under 1 KB
pse::Rasterizer& g_raster = pse::shared_rasterizer();
pse::Renderer3D g_renderer(g_raster);
pse::FrameQueue& g_queue = pse::shared_queue();

// The screen is two stacked scenes: the world above the surface in the top
// band, the water below it in the bottom band. Each band is one core's whole
// job on the device, so the split line is also the parallel seam.
constexpr int k_split = 60;

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
float g_hook_depth = 0.6f; // eased render depth of the hook underwater

// Underwater motes: a persistent pool of slow drifters in band screen space,
// 8.8 fixed point. They rise gently on their own, and the camera's travel
// pushes them the opposite way so the water reads as something moved
// through, not a static backdrop. Purely cosmetic, never touches the sim.
struct Mote {
    int32_t x, y;      // 8.8 fixed, band pixels
    uint8_t layer;     // 1 far .. 3 near; near motes move and shine more
    uint8_t phase;     // per mote wander offset
    int8_t vx;         // own sideways drift, 8.8 per frame
};
constexpr int k_max_motes = 12;
Mote g_motes[k_max_motes];
bool g_motes_seeded = false;
uint32_t g_mote_respawns = 0;   // stirs the hash when a mote is recycled
float g_mote_cam_x = 0.0f, g_mote_cam_z = 0.0f, g_mote_hook = 0.6f;

// How far the pool is currently bulged away from the centre line, 8.8. Camera
// travel toward the scene feeds it and it springs back to nothing, so parting
// the water is something the pool does and then recovers from.
//
// This used to be integrated into every mote's own x instead, which had no
// way back: a run of travel in one direction walked the whole field onto the
// centre column and left it there, and only a sideways move redistributed it.
// A bulge that decays cannot accumulate, so the pool keeps its spread.
int32_t g_mote_bulge = 0;
constexpr int32_t k_mote_bulge_max = 6 << 8;

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

inline uint8_t shade8(int value, int mul) {
    const int out = value * mul / 256;
    return static_cast<uint8_t>(out > 255 ? 255 : (out < 0 ? 0 : out));
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

void push_quad(const int sx[4], const int sy[4], const int sz[4],
               uint8_t r, uint8_t g, uint8_t b) {
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

// A flat ground quad spanning x -width..width over z z0..z1, at height y.
void draw_ground_quad(float width, float z0, float z1, float y,
                      uint8_t r, uint8_t g, uint8_t b) {
    int sx[4], sy[4], sz[4];
    const float xs[4] = {-width, width, width, -width};
    const float zs[4] = {z0, z0, z1, z1};
    for (int i = 0; i < 4; i++) {
        if (!g_renderer.project(xs[i], y, zs[i], sx[i], sy[i], sz[i])) return;
    }
    push_quad(sx, sy, sz, r, g, b);
}

// The land beyond the water: a sand line where the pond ends, grass rising
// behind it, and a stand of trees on the grass. This is what the eye lands on
// when it follows a long cast, so it gets real colours instead of more water.
void draw_shore(const SkyKey& sky) {
    const int light = sky.light;

    // Water between the animated grid and the shore, matched to the grid's
    // farthest row so the seam vanishes. The lake now runs 50 meters out,
    // so the land starts beyond the longest cast.
    const int wshade = 256 - (8 * 130) / 10;
    draw_ground_quad(70.0f, 44.0f, 58.0f, 0.0f,
                     shade8(sky.wat_r * wshade / 256, light),
                     shade8(sky.wat_g * wshade / 256, light),
                     shade8(sky.wat_b * wshade / 256, light));

    // Sand, then grass climbing gently away from the water.
    draw_ground_quad(80.0f, 58.0f, 64.0f, 0.02f,
                     shade8(205, light), shade8(180, light), shade8(126, light));
    draw_ground_quad(100.0f, 64.0f, 82.0f, 0.05f,
                     shade8(66, light), shade8(132, light), shade8(64, light));
    draw_ground_quad(130.0f, 82.0f, 130.0f, 0.4f,
                     shade8(44, light), shade8(96, light), shade8(52, light));

    // The treeline. A handful of trees at mixed depths and sizes reads as a
    // wooded shore without costing more than one fish worth of triangles.
    struct TreeSpot { float x, z, scale; };
    static const TreeSpot k_trees[] = {
        {-46.0f, 66.0f, 5.8f}, {-22.0f, 72.0f, 7.0f}, {2.0f, 67.0f, 5.6f},
        {24.0f, 74.0f, 7.4f}, {44.0f, 68.0f, 6.2f},  {64.0f, 76.0f, 6.6f},
    };
    for (const auto& spot : k_trees) {
        g_renderer.draw_mesh(models::tree, spot.x, 0.05f, spot.z, 0.0f,
                             spot.scale, sky.light, sky.light, sky.light);
    }
}

// The water is a grid of vertices ahead of the boat, two triangles per cell,
// coloured per vertex: deeper water is darker, crests catch the light. The
// single biggest fill cost in the frame, which is exactly what the two core
// split exists for.
constexpr int k_wx = 10;
constexpr int k_wz = 9;

void draw_water(const World& world, const SkyKey& sky, uint32_t t) {
    int sx[k_wx * k_wz];
    int sy[k_wx * k_wz];
    int sz[k_wx * k_wz];
    bool ok[k_wx * k_wz];
    uint8_t cr[k_wx * k_wz], cg[k_wx * k_wz], cb[k_wx * k_wz];

    // Row depths grow toward the horizon so nine rows can span fifty
    // meters of water without starving the near field of vertices.
    static const int32_t k_row_z_fp[k_wz] = {
        -256, 256, 896, 1792, 3072, 4864, 7168, 9984, 11520,   // -1 .. 45
    };

    for (int j = 0; j < k_wz; j++) {
        for (int i = 0; i < k_wx; i++) {
            const int idx = j * k_wx + i;
            // Widen with distance so the far edge spans the 90 degree
            // frustum rather than pinching into a dome on the horizon.
            const int32_t wx_fp = (2 * i - (k_wx - 1)) * (320 + j * 300);
            const int32_t wz_fp = k_row_z_fp[j];
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
            cr[idx] = shade8(r, light);
            cg[idx] = shade8(g, light);
            cb[idx] = shade8(b, light);
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
            if (g_renderer.project(px, 0.02f, pz, sx2, sy2, sz2) &&
                sy2 < k_split) {
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
        const int y = static_cast<int>((h >> 8) % 40) + ((t * 3 + i) % 8);
        g_raster.plot(x, y, 150, 170, 200);
        g_raster.plot(x, y + 1, 120, 140, 180);
    }
}

// Bresenham, bounded, and clipped to a row band so a line belonging to one
// scene can never scribble into the other.
void draw_line_band(int x0, int y0, int x1, int y1, int band_y0, int band_y1,
                    uint8_t r, uint8_t g, uint8_t b) {
    int dx = x1 - x0, dy = y1 - y0;
    const int steps = (dx < 0 ? -dx : dx) > (dy < 0 ? -dy : dy)
                          ? (dx < 0 ? -dx : dx) : (dy < 0 ? -dy : dy);
    if (steps == 0 || steps > 240) return;
    for (int i = 0; i <= steps; i++) {
        const int y = y0 + dy * i / steps;
        if (y < band_y0 || y >= band_y1) continue;
        g_raster.plot(x0 + dx * i / steps, y, r, g, b);
    }
}

// The red "!" over the hook. (bx, by) is the projected anchor; the glyph is
// then clamped fully inside [band_y0, band_y1) so it stays visible however
// far away or close to the seam the hook sits. An alert that can slide off
// screen is an alert that sometimes does not exist.
void draw_bite_mark(int bx, int by, int band_y0, int band_y1) {
    if (bx < 1) bx = 1;
    if (bx > 117) bx = 117;
    int top = by - 9;              // glyph spans 8 rows: bar, gap, dot
    if (top < band_y0 + 1) top = band_y0 + 1;
    if (top > band_y1 - 9) top = band_y1 - 9;
    for (int y = 0; y < 5; y++) {
        g_raster.plot(bx, top + y, 255, 60, 60);
        g_raster.plot(bx + 1, top + y, 255, 60, 60);
    }
    g_raster.plot(bx, top + 7, 255, 60, 60);
    g_raster.plot(bx + 1, top + 7, 255, 60, 60);
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

// A five pixel fish glyph, for labelling the stamina meter without a word.
void draw_fish_glyph(int x, int y, uint8_t r, uint8_t g, uint8_t b) {
    g_raster.plot(x + 1, y, r, g, b);
    g_raster.plot(x + 2, y, r, g, b);
    g_raster.plot(x + 1, y + 1, r, g, b);
    g_raster.plot(x + 2, y + 1, r, g, b);
    g_raster.plot(x, y + 1, r, g, b);
    g_raster.plot(x + 4, y, r, g, b);
    g_raster.plot(x + 4, y + 2, r, g, b);
    g_raster.plot(x + 3, y + 1, r, g, b);
}

// The fight meters the whole rework hangs on. Left edge: the fish's stamina
// under a fish glyph, draining as the player tugs and refilling when they
// rest. Bottom edge: line tension running left to right into a marked red
// zone; camping in the red is what breaks the line, so the zone and the
// charge toward breaking are both visible.
void draw_fight_meters(const World& world) {
    // Stamina, vertical on the left of the underwater view.
    //
    // It goes dark blue while the fish is taking its second wind. That window
    // is the one time pulling is free, and it ends by itself, so it has to be
    // legible at a glance rather than inferred from a bar that happens to be
    // climbing: the colour says spend this now, the bright bar says stop.
    const int stam_h = 40;
    const int fill = world.stamina_max > 0
        ? (world.stamina * stam_h) / world.stamina_max : 0;
    const bool recovering = world.spent_timer > 0;
    const uint8_t sr = recovering ? 30 : 120;
    const uint8_t sg = recovering ? 70 : 220;
    const uint8_t sb = recovering ? 160 : 250;
    draw_fish_glyph(3, k_split + 4, 240, 240, 250);
    draw_bar(3, k_split + 9, stam_h, fill, sr, sg, sb);

    // Tension, horizontal along the bottom. The red zone begins where the
    // danger counter starts charging.
    const int bar_x = 12, bar_w = 96, bar_y = 113, bar_h = 5;
    const int danger_x = bar_x + (kf::k_tension_danger * bar_w) / 1024;
    for (int x = 0; x < bar_w; x++) {
        const bool in_red = bar_x + x >= danger_x;
        for (int y = 0; y < bar_h; y++) {
            uint8_t r = in_red ? 90 : 30, g = 30, b = in_red ? 34 : 45;
            if (y == 0 || y == bar_h - 1 || x == 0 || x == bar_w - 1) {
                r = 60; g = 60; b = 80;
            }
            g_raster.plot(bar_x + x, bar_y + y, r, g, b);
        }
    }
    const int tfill = (world.tension * bar_w) / 1024;
    for (int x = 0; x < tfill; x++) {
        const bool in_red = bar_x + x >= danger_x;
        uint8_t r = in_red ? 250 : 120, g = in_red ? 80 : 220, b = 90;
        g_raster.plot(bar_x + x, bar_y + 1, r, g, b);
        g_raster.plot(bar_x + x, bar_y + 2, r, g, b);
        g_raster.plot(bar_x + x, bar_y + 3, r, g, b);
    }
    // The break charge creeps along the top edge of the red zone while the
    // player camps there. Full width of the zone means the fish is gone.
    if (world.danger > 0) {
        const int charge =
            (world.danger * (bar_x + bar_w - danger_x)) / kf::k_danger_ticks;
        for (int x = 0; x < charge; x++) {
            g_raster.plot(danger_x + x, bar_y - 1, 255, 60, 60);
        }
    }
}

// Advance and draw the mote pool. Call with the underwater camera bound:
// the camera's frame to frame travel becomes an opposing screen space push,
// scaled per layer, so near specks stream past faster than far ones. The
// hook sinking tilts the view down, which reads as the water sliding up.
void draw_motes(const SkyKey& sky, uint32_t t) {
    constexpr int k_band_top = (k_split + 3) << 8;
    constexpr int k_band_bottom = 119 << 8;
    constexpr int k_band_h = k_band_bottom - k_band_top;

    float cam_x, cam_y, cam_z;
    g_renderer.camera_position(cam_x, cam_y, cam_z);

    if (!g_motes_seeded) {
        for (int i = 0; i < k_max_motes; i++) {
            const uint32_t h = (i + 1) * 2246822519u;
            // One mote per column slice plus jitter, rather than a hash across
            // the whole width: twelve specks is few enough that an unlucky
            // hash leaves visible gaps and clumps on the very first frame.
            constexpr int32_t slice = (120 << 8) / k_max_motes;
            g_motes[i].x = i * slice + static_cast<int32_t>(h % slice);
            g_motes[i].y = k_band_top + static_cast<int32_t>((h >> 9) % k_band_h);
            g_motes[i].layer = static_cast<uint8_t>(1 + (h >> 21) % 3);
            g_motes[i].phase = static_cast<uint8_t>(h >> 24);
            // Each mote drifts its own way. The shared sine wander only
            // oscillates, so without this nothing ever separates two motes
            // that happen to meet.
            g_motes[i].vx = static_cast<int8_t>((h >> 16) % 13 - 6);
        }
        g_motes_seeded = true;
        g_mote_cam_x = cam_x;
        g_mote_cam_z = cam_z;
        g_mote_hook = g_hook_depth;
    }

    // Camera travel since the last frame, clamped so a camera cut (cast,
    // catch, menu) does not slingshot the pool across the band.
    float dx = cam_x - g_mote_cam_x;
    float dz = cam_z - g_mote_cam_z;
    float dh = g_hook_depth - g_mote_hook;
    g_mote_cam_x = cam_x;
    g_mote_cam_z = cam_z;
    g_mote_hook = g_hook_depth;
    if (dx > 0.5f || dx < -0.5f || dz > 0.5f || dz < -0.5f) {
        dx = 0.0f; dz = 0.0f; dh = 0.0f;
    }

    // World travel to 8.8 screen push, opposing the motion. The gains are
    // sized so a fish dragging the camera streams the pool at about half a
    // pixel per frame: clearly flowing, nowhere near noise.
    const int push_x = static_cast<int>(-dx * 1400.0f);
    const int push_y = static_cast<int>(-dh * 1200.0f);

    // Moving toward the hook parts the water. Fed by travel and sprung back
    // toward nothing every frame, so it bulges the pool and releases it
    // instead of walking it somewhere and leaving it there.
    g_mote_bulge += static_cast<int32_t>(dz * 500.0f);
    g_mote_bulge -= g_mote_bulge / 8;
    if (g_mote_bulge > k_mote_bulge_max) g_mote_bulge = k_mote_bulge_max;
    if (g_mote_bulge < -k_mote_bulge_max) g_mote_bulge = -k_mote_bulge_max;

    for (int i = 0; i < k_max_motes; i++) {
        Mote& m = g_motes[i];
        const int scale = m.layer;   // 1..3, near layers move more

        // Gentle ambient rise with a sideways wander, a fraction of a pixel
        // per frame, so an undisturbed pool still feels like open water.
        const int amb_x = sin64(t / 6 + m.phase) / 10;
        const int amb_y = -14 - 4 * scale;

        m.x += amb_x + m.vx + push_x * scale / 3;
        m.y += amb_y + push_y * scale / 3;

        // Wrap within the band so the pool never thins out. A mote that has
        // risen out of the top comes back at the bottom somewhere new: over a
        // long sit that reseeding is what keeps the field even, whatever the
        // drift has been doing.
        if (m.x < 0) m.x += 120 << 8;
        if (m.x >= (120 << 8)) m.x -= 120 << 8;
        if (m.y < k_band_top) {
            m.y += k_band_h;
            const uint32_t h = (++g_mote_respawns + i * 71u) * 2654435761u;
            m.x = static_cast<int32_t>(h % (120 << 8));
            m.vx = static_cast<int8_t>((h >> 16) % 13 - 6);
        }
        if (m.y >= k_band_bottom) m.y -= k_band_h;

        // The bulge is a draw time offset, never stored: that is what stops
        // it accumulating into the positions.
        int32_t draw_x = m.x + (m.x >= (60 << 8) ? 1 : -1) *
                                   (g_mote_bulge * scale) / 3;
        if (draw_x < 0) draw_x += 120 << 8;
        if (draw_x >= (120 << 8)) draw_x -= 120 << 8;

        const int lift = 40 + scale * 12;
        g_raster.plot(draw_x >> 8, m.y >> 8,
                      shade8(sky.wat_r + lift, sky.light),
                      shade8(sky.wat_g + lift, sky.light),
                      shade8(sky.wat_b + lift + 10, sky.light));
    }
}

void set_top_camera(const World& world, uint32_t t) {
    g_renderer.set_viewport(0, k_split);
    float cam_tx = 0.0f, cam_ty = 0.0f, cam_tz = 5.6f;
    float yaw = sin64(t / 4) / 2200.0f;
    float dist = 8.2f, height = 3.8f;
    // The lake wants the default framing: aiming a unit over the water pushes
    // it down the band and keeps the far shore and the sky in shot.
    float look_lift = 1.0f;
    if (world.mode == kf::Mode::Fight && world.hooked_fish >= 0) {
        cam_tx = fp_to_f(world.fish[world.hooked_fish].x) * 0.4f;
        cam_tz = 4.6f + fp_to_f(world.fish[world.hooked_fish].z) * 0.15f;
    } else if (world.mode == kf::Mode::Landed) {
        // The trophy shot is the one camera studying a single object, so it
        // aims straight at it. With the lake's lift the fish hung a metre
        // below the aim point, which put it across the split and cut its
        // belly off on the biggest species, exactly the ones worth showing.
        cam_tx = 0.0f; cam_ty = 0.55f; cam_tz = 3.1f;
        dist = 2.3f; height = 0.7f;
        yaw = 0.0f;
        look_lift = 0.0f;
    }
    g_renderer.set_orbit_camera(cam_tx, cam_ty, cam_tz, yaw, dist, height,
                                look_lift);
}

// The underwater camera hangs below the surface and looks at the hook, or
// drifts under the boat when no line is out.
void set_underwater_camera(const World& world, bool lure_in_water) {
    g_renderer.set_viewport(k_split, 120 - k_split);
    float cx = 0.0f, cy = -0.55f, cz = 0.6f;
    float look_x = 0.0f, look_y = -0.9f, look_z = 6.0f;
    if (lure_in_water) {
        const float lx = fp_to_f(world.lure_x);
        const float lz = fp_to_f(world.lure_z);
        cx = lx * 0.5f;
        cz = lz - 3.4f;
        look_x = lx;
        look_y = -g_hook_depth;
        look_z = lz;
    }
    const float dx = look_x - cx, dy = look_y - cy, dz = look_z - cz;
    const float yaw = atan2f(dx, dz);
    const float pitch = atan2f(dy, sqrtf(dx * dx + dz * dz));
    g_renderer.set_camera(cx, cy, cz, yaw, pitch);
}

void draw_underwater_scene(const World& world, const SkyKey& sky, uint32_t t,
                           bool lure_in_water) {
    // Everything down here shades toward the deep with its own vertex depth,
    // which is what makes the fish read as swimming bodies instead of decals.
    g_renderer.set_depth_fade(true,
                              shade8(sky.wat_r, 70), shade8(sky.wat_g, 80),
                              shade8(sky.wat_b, 95),
                              -0.1f, -2.8f);

    for (const auto& fish : world.fish) {
        if (fish.state == kf::FishState::Gone) continue;
        if (world.mode == kf::Mode::Landed) continue;

        const kf::Species& s = kf::k_species[fish.species];
        float fx = fp_to_f(fish.x);
        float fy = -fp_to_f(fish.y) - 0.12f;
        float fz = fp_to_f(fish.z);
        float fyaw;
        if (fish.state == kf::FishState::Hooked) {
            // Thrash on the line instead of swimming a heading.
            fx += sin64(t * 3) / 500.0f;
            fy -= sin64(t * 5 + 9) / 900.0f;
            fyaw = atan2f(fp_to_f(world.lure_x) - fx, 0.8f) +
                   sin64(t * 4) / 300.0f;
        } else {
            fyaw = atan2f(static_cast<float>(fish.vx),
                          static_cast<float>(fish.vz == 0 ? 1 : fish.vz));
        }
        const float scale = 0.30f + fish.size_cm * 0.006f;
        g_renderer.draw_mesh(models::fish, fx, fy, fz, fyaw, scale,
                             shade8(s.r, sky.light), shade8(s.g, sky.light),
                             shade8(s.b, sky.light));
    }

    // The hook. The bobber mesh reads as the lure below the surface too, and
    // its eased depth is what the camera tracks.
    if (lure_in_water) {
        float target = 1.0f;
        if (world.mode == kf::Mode::Fight && world.hooked_fish >= 0) {
            target = fp_to_f(world.fish[world.hooked_fish].y) + 0.12f;
        }
        g_hook_depth += (target - g_hook_depth) * 0.08f;
        const float bob = sin64(t * 2) / 2000.0f;
        g_renderer.draw_mesh(models::bobber, fp_to_f(world.lure_x),
                             -g_hook_depth + bob, fp_to_f(world.lure_z), 0.0f,
                             0.18f, sky.light, sky.light, sky.light);
    }

    g_renderer.set_depth_fade(false, 0, 0, 0);
}

}  // namespace

void render_scene(const World& world, const pse::RenderTarget& target,
                  uint32_t time_ms) {
    const uint32_t t = time_ms / 33;   // animation clock, ~30 steps a second
    const SkyKey sky = sky_now(world);
    const bool lure_in_water = world.mode == kf::Mode::Sinking ||
                               world.mode == kf::Mode::Fight;
    const bool lure_visible = lure_in_water || world.mode == kf::Mode::Flying;

    g_raster.begin_frame_collect(target, g_queue);

    // ---- top scene: the world above the surface ----
    set_top_camera(world, t);
    draw_shore(sky);
    draw_water(world, sky, t);

    // Boat, bobbing on the same water function so it sits in the waves.
    const float boat_bob = wave_fp(4, 1, t, world.raining != 0) / 256.0f;
    g_renderer.draw_mesh(models::boat, 0.0f, 0.02f + boat_bob, 0.2f, 0.0f,
                         0.85f, sky.light, sky.light, sky.light);

    // A hooked fish breaching the surface is the one moment a fish belongs
    // to the top scene.
    if (world.mode == kf::Mode::Fight && world.hooked_fish >= 0 &&
        world.leap_timer > 0) {
        const auto& fish = world.fish[world.hooked_fish];
        const kf::Species& s = kf::k_species[fish.species];
        const int lt = world.leap_timer;
        const float y = 0.045f + (lt * (45 - lt)) / 900.0f;
        const float fyaw = atan2f(fp_to_f(fish.x), fp_to_f(fish.z));
        const float scale = 0.30f + fish.size_cm * 0.006f;
        g_renderer.draw_mesh(models::fish, fp_to_f(fish.x), y,
                             fp_to_f(fish.z), fyaw, scale, s.r, s.g, s.b);
    }

    // Trophy shot while the card is up.
    if (world.mode == kf::Mode::Landed && world.card_species >= 0) {
        const kf::Species& s = kf::k_species[world.card_species];
        const float scale = 0.30f + world.card_size * 0.006f;
        g_renderer.draw_mesh(models::fish, 0.0f, 0.55f, 3.1f,
                             t * 0.05f, scale, s.r, s.g, s.b);
    }

    // Bobber on or above the surface.
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

    // ---- bottom scene: underwater ----
    g_queue.mark_split();
    set_underwater_camera(world, lure_in_water);
    draw_underwater_scene(world, sky, t, lure_in_water);

    // Everything above went into the queue; this is the frame's parallel
    // part. The top band clears with the sky, the bottom with the water
    // column fading into the deep.
    pse::run_split(g_raster, g_queue,
                   pse::SkyGradient{sky.sky_r, sky.sky_g, sky.sky_b,
                                    sky.hor_r, sky.hor_g, sky.hor_b},
                   pse::SkyGradient{shade8(sky.wat_r * 5 / 4, sky.light),
                                    shade8(sky.wat_g * 5 / 4, sky.light),
                                    shade8(sky.wat_b * 6 / 5, sky.light),
                                    shade8(sky.wat_r, 50),
                                    shade8(sky.wat_g, 60),
                                    shade8(sky.wat_b, 80)});
    g_raster.end_collect();

    // ---- overlay pass, immediate mode on top of the finished frame ----
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

    // The waterline: a bright seam where the two scenes meet.
    for (int x = 0; x < target.width; x++) {
        const int shimmer = sin64(x * 6 + t * 2) / 24;
        g_raster.plot(x, k_split - 1,
                      shade8(sky.wat_r + 90 + shimmer, sky.light),
                      shade8(sky.wat_g + 90 + shimmer, sky.light),
                      shade8(sky.wat_b + 80 + shimmer, sky.light));
        g_raster.plot(x, k_split,
                      shade8(sky.wat_r + 40, sky.light),
                      shade8(sky.wat_g + 40, sky.light),
                      shade8(sky.wat_b + 40, sky.light));
    }

    // Top half overlay, under the top camera.
    set_top_camera(world, t);
    draw_ripples(sky);
    draw_rain(world, t);

    // Surface shadows for fish near the top, so the pond reads alive before
    // a cast without paying for meshes twice.
    for (const auto& fish : world.fish) {
        if (fish.state == kf::FishState::Gone) continue;
        if (fish.y > 320) continue;   // deep fish belong to the bottom half
        int sx2, sy2, sz2;
        if (g_renderer.project(fp_to_f(fish.x), 0.01f, fp_to_f(fish.z),
                               sx2, sy2, sz2) && sy2 < k_split - 1) {
            const uint8_t r = shade8(sky.wat_r, 90);
            const uint8_t g = shade8(sky.wat_g, 90);
            const uint8_t b = shade8(sky.wat_b, 110);
            g_raster.plot(sx2, sy2, r, g, b);
            g_raster.plot(sx2 + 1, sy2, r, g, b);
            g_raster.plot(sx2 - 1, sy2, r, g, b);
        }
    }

    // The line, rod tip to where it meets the water (or the airborne lure).
    if (lure_visible) {
        int rx, ry, rz, lx, ly, lz;
        const bool rod_ok = g_renderer.project(0.18f, 0.95f, 1.55f, rx, ry, rz);
        const float entry_y = world.mode == kf::Mode::Flying
                                  ? -fp_to_f(world.lure_y) + 0.08f : 0.04f;
        const bool lure_ok = g_renderer.project(
            fp_to_f(world.lure_x), entry_y, fp_to_f(world.lure_z),
            lx, ly, lz);
        if (rod_ok && lure_ok) {
            uint8_t lr = 210, lg = 210, lb = 215;
            if (world.mode == kf::Mode::Fight) {
                lr = world.tension >= kf::k_tension_danger ? 240 : 160;
                lg = world.tension >= kf::k_tension_danger ? 70 : 220;
                lb = 80;
            }
            draw_line_band(rx, ry, lx, ly, 0, k_split, lr, lg, lb);
        }
    }

    // Bite mark: one glyph, no words. Gated on an actual biting fish as well
    // as the timer, so a stale timer can never show a phantom alert. The
    // glyph is clamped into the band rather than culled: after the world
    // grew to 50 m a distant float projected within a few rows of the
    // horizon, the old bounds check dropped it, and the bite played its
    // sound with no visual at all.
    bool bite_open = false;
    for (const auto& fish : world.fish) {
        if (fish.state == kf::FishState::Biting) bite_open = true;
    }
    if (bite_open && world.bite_timer > 0 && world.mode == kf::Mode::Sinking) {
        int bx, by, bz;
        if (g_renderer.project(fp_to_f(world.lure_x), 0.30f,
                               fp_to_f(world.lure_z), bx, by, bz)) {
            draw_bite_mark(bx, by, 0, k_split);
        }
    }

    // Power meter while aiming lives in the top half.
    if (world.mode == kf::Mode::Aiming) {
        draw_bar(5, 8, 44, (world.power * 44) / 255, 250, 210, 90);
    }

    // Bottom half overlay, under the water camera.
    set_underwater_camera(world, lure_in_water);

    // The line continues below the surface, straight down the seam to the
    // hook, and it carries the same tension colour as above.
    if (lure_in_water) {
        int ex, ey, ez, hx, hy, hz;
        const bool entry_ok = g_renderer.project(
            fp_to_f(world.lure_x), 0.0f, fp_to_f(world.lure_z), ex, ey, ez);
        const bool hook_ok = g_renderer.project(
            fp_to_f(world.lure_x), -g_hook_depth + 0.06f,
            fp_to_f(world.lure_z), hx, hy, hz);
        if (entry_ok && hook_ok) {
            uint8_t lr = 190, lg = 195, lb = 205;
            if (world.mode == kf::Mode::Fight) {
                lr = world.tension >= kf::k_tension_danger ? 240 : 150;
                lg = world.tension >= kf::k_tension_danger ? 70 : 210;
                lb = 90;
            }
            draw_line_band(ex, ey, hx, hy, k_split + 1, 120, lr, lg, lb);
        }
    }

    // The bite mark again, right above the hook, so the alert reads in
    // whichever half of the screen the player is watching.
    if (bite_open && world.bite_timer > 0 && world.mode == kf::Mode::Sinking) {
        int bx, by, bz;
        if (g_renderer.project(fp_to_f(world.lure_x), -g_hook_depth + 0.35f,
                               fp_to_f(world.lure_z), bx, by, bz)) {
            draw_bite_mark(bx, by, k_split + 1, 120);
        }
    }

    // Drifting motes give the water body without geometry.
    draw_motes(sky, t);

    if (world.mode == kf::Mode::Fight) draw_fight_meters(world);

    g_renderer.set_viewport(0, 0);
}

}  // namespace kfr
