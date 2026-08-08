#include "render.hpp"

#include <cmath>

#include "pse/parallel.hpp"
#include "pse/quat.hpp"
#include "pse/raster.hpp"
#include "pse/renderer3d.hpp"
#include "pse/shared_render.hpp"
#include "pse/text.hpp"

#include "starlance/bomber.hpp"
#include "starlance/fighter.hpp"
#include "starlance/frigate.hpp"
#include "starlance/gunship.hpp"
#include "starlance/interceptor.hpp"

namespace slr {
namespace {

using sl::World;

// The Rasterizer and the FrameQueue come from the engine rather than being
// declared here: on the console every game is linked into one binary, and a
// 14 KB depth buffer plus a 15 KB triangle queue per game is RAM spent on
// scenes nothing is rendering.
//   Rasterizer  ~14.4 KB (depth buffer)   shared
//   FrameQueue  ~15.4 KB (640 triangles)  shared
//   this file    ~1.0 KB (star table, camera, frame stats)
pse::Rasterizer& g_raster = pse::shared_rasterizer();
pse::Renderer3D g_renderer(g_raster);
pse::FrameQueue& g_queue = pse::shared_queue();

FrameStats g_stats{};

// The bracket this frame's world was projected with, kept so the instruments
// can go back to it.
//
// They have to. The backdrop pass moves the projector out to a lens that can
// reach a body at infinity, and every HUD element that projects a world point
// runs after it: the reticle, the target box and the lead pip all came back
// off screen for anything nearer than about sixty units, because the sky's
// near plane had swallowed them. On screen that was a crosshair that simply
// was not there in a dogfight, which reads as a HUD that has not been written
// rather than as a lens left in the wrong place.
float g_world_near = 3.0f;
float g_world_far = 200.0f;

// ---- the lens ----

// Wide, because a space sim with a long lens feels like it is on rails: with
// nothing in shot but the ship and the stars, the sense of turning comes
// almost entirely from how fast the frame sweeps, and a narrow field of view
// takes that away.
constexpr float k_fov = 82.0f;

// Where the chase camera sits, in world units, along the ship's OWN back and
// up axes. It is rigidly attached: the camera rolls with the ship, which is
// the whole reason the engine grew set_camera_basis. A camera that stayed
// level while the ship rolled would make roll invisible, and roll is a third
// of the control scheme.
constexpr float k_cam_back = 6.2f;
// High enough that the ship sits clear of the middle of the frame. At a
// smaller lift the hull was drawn straight over whatever was targeted: a
// frigate dead ahead at sixty units disappeared behind your own tail, with
// the target box round the place it should have been.
constexpr float k_cam_lift = 2.15f;

// How fast the camera catches up with the ship, per 60th of a second, which
// is the rate the pico-8 prototype this game descends from used. Bolted
// rigidly to the hull, a turn moves the whole sky at once and reads as the
// universe rotating rather than as the ship turning; letting the camera trail
// and settle is what puts the motion in the ship.
constexpr float k_cam_lerp = 0.1f;
constexpr float k_cam_frame_ms = 16.667f;

// Further than this from where it should be and the camera jumps rather than
// flies. A sortie restarting, or a wave arriving somewhere else, would
// otherwise be a long swoop across the arena from wherever the camera used to
// be, which looks like a bug in the level rather than a smooth camera.
constexpr float k_cam_snap = 40.0f;

// ---- the one byte depth buffer ----
//
// The rasterizer's depth is 8 bits and the projector's curve is perspective,
// so nearly all of the resolution sits near the near plane and a scene spread
// from 4 units to 300 lands almost entirely in the last three values. The
// answer is to re-bracket the range every frame to what the frame actually
// contains, which is what these bound.
//
// 0.42 rather than something nearer 1: the projector's effective near plane
// is the HARMONIC mean 2fn/(f+n), which for a far plane much larger than the
// near one is close to 2n. Setting the near plane at the nearest object would
// therefore clip that object away entirely.
constexpr float k_near_fraction = 0.42f;
constexpr float k_far_fraction = 1.25f;
constexpr float k_near_floor = 3.0f;
constexpr float k_far_ceiling = 360.0f;

// The player's own hull gets its own bracket, because including it in the one
// above would pin the near plane at three units and flatten the whole battle
// into the top of the range. Nothing in the world is normally between the
// camera and the ship it is bolted behind, so drawing it in a second pass
// with a near lens of its own costs nothing and buys the scene its depth back.
constexpr float k_hull_near = 1.1f;
constexpr float k_hull_far = 400.0f;

// ---- the sky ----
//
// Backdrop bodies are at infinity: they are drawn from a DIRECTION, and the
// radius below is only a distance far enough that they sit behind everything
// and near enough that the projector's fixed point does not overflow.
// project() forms `m[3][k] * to_fixed(v)` in an int32, and to_fixed is a
// multiply by 1024, so the arena's 170 unit half width plus this radius has to
// stay inside about 500 units. 240 leaves a quarter of the range spare.
constexpr float k_sky_radius = 240.0f;
constexpr float k_sky_near = 30.0f;
constexpr float k_sky_far = 500.0f;

// Depths the sky claims. Geometry writes 0..254, and the frame is cleared to
// 255, so a body at 253 fills exactly the empty sky and loses to anything at
// all. Stars sit one behind that, so they are hidden by a planet without the
// planet needing to know they exist.
constexpr uint8_t k_depth_body = 253;
constexpr uint8_t k_depth_star = 254;

// ---- small maths ----

inline int clamp_int(int v, int lo, int hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

inline uint8_t clamp8(int v) {
    return static_cast<uint8_t>(clamp_int(v, 0, 255));
}

inline float fp_to_f(int32_t v) {
    return static_cast<float>(v) / static_cast<float>(sl::k_one);
}

int isqrt_int(int value) {
    if (value <= 0) return 0;
    int x = value, bit = 1 << 30;
    while (bit > x) bit >>= 2;
    int root = 0;
    while (bit != 0) {
        if (x >= root + bit) {
            x -= root + bit;
            root = (root >> 1) + bit;
        } else {
            root >>= 1;
        }
        bit >>= 2;
    }
    return root;
}

// ---- the camera frame, kept for the frame ----

struct Camera {
    float x, y, z;
    float right[3];
    float up[3];
    float forward[3];
};
Camera g_cam{};
bool g_cam_seeded = false;
uint32_t g_cam_last_ms = 0;

inline float dot_cam(const float axis[3], float x, float y, float z) {
    return axis[0] * x + axis[1] * y + axis[2] * z;
}

inline float dot3(const float a[3], const float b[3]) {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

void cross3(const float a[3], const float b[3], float out[3]) {
    out[0] = a[1] * b[2] - a[2] * b[1];
    out[1] = a[2] * b[0] - a[0] * b[2];
    out[2] = a[0] * b[1] - a[1] * b[0];
}

bool normalize3(float v[3]) {
    const float len2 = dot3(v, v);
    if (len2 < 1e-9f) return false;
    const float inv = 1.0f / sqrtf(len2);
    v[0] *= inv; v[1] *= inv; v[2] *= inv;
    return true;
}

// Make a lerped frame into a rotation again.
//
// The three axes are eased independently, and independent easing does not
// preserve a basis: two unit vectors lerped halfway give something shorter
// than either, and a frame that is no longer orthonormal skews and scales
// everything drawn through it. The pico-8 game this camera is modelled on
// simply lives with that, because its projection is a hand written dot
// product per vertex and a slightly short axis only nudges the scale. Here the
// basis goes straight into a view matrix, so it has to be a real one.
//
// Gram-Schmidt off the forward axis: forward is the direction that matters,
// right is whatever is left of the eased right once forward is taken out of
// it, and up follows from the other two.
void orthonormalize(Camera& cam) {
    if (!normalize3(cam.forward)) {
        cam.forward[0] = 0.0f; cam.forward[1] = 0.0f; cam.forward[2] = 1.0f;
    }

    const float along_fwd = dot3(cam.right, cam.forward);
    cam.right[0] -= cam.forward[0] * along_fwd;
    cam.right[1] -= cam.forward[1] * along_fwd;
    cam.right[2] -= cam.forward[2] * along_fwd;

    if (!normalize3(cam.right)) {
        // The eased right has collapsed onto forward, which takes a half turn
        // of roll inside one frame to manage. Any perpendicular will do, so
        // seed from the world axis forward leans on LEAST: crossing with the
        // one it leans on most is crossing with something nearly parallel,
        // which gives back another near zero vector and fixes nothing.
        const float ax = cam.forward[0] < 0.0f ? -cam.forward[0] : cam.forward[0];
        const float ay = cam.forward[1] < 0.0f ? -cam.forward[1] : cam.forward[1];
        float seed[3] = {0.0f, 1.0f, 0.0f};
        if (ax <= ay) { seed[0] = 1.0f; seed[1] = 0.0f; }
        cross3(seed, cam.forward, cam.right);
        normalize3(cam.right);
    }

    // up = forward x right, which is the handedness Renderer3D builds its view
    // rows with. Crossing the other way round mirrors the picture.
    cross3(cam.forward, cam.right, cam.up);
}

// ---- drawing primitives ----

void plot_depth(const pse::RenderTarget& target, int x, int y, uint8_t depth,
                uint8_t r, uint8_t g, uint8_t b) {
    if (!g_raster.test_and_set_depth(x, y, depth)) return;
    pse::plot_pixel(target, x, y, r, g, b);
}

// Bresenham, depth tested at a constant depth. Every line this game draws is
// either a bolt (short, and its two ends are within a depth step of each
// other) or HUD furniture, so interpolating depth along it would be spending
// a divide per pixel to move nothing.
void line_depth(const pse::RenderTarget& target, int x0, int y0, int x1, int y1,
                uint8_t depth, uint8_t r, uint8_t g, uint8_t b) {
    int dx = x1 - x0, dy = y1 - y0;
    const int step_x = dx < 0 ? -1 : 1;
    const int step_y = dy < 0 ? -1 : 1;
    dx = dx < 0 ? -dx : dx;
    dy = dy < 0 ? -dy : dy;

    // A stray coordinate from a projection near the frustum edge can be
    // enormous, and walking it a pixel at a time would hang the frame. The
    // clip in plot_pixel makes it invisible, not cheap.
    if (dx > 400 || dy > 400) return;

    int err = dx - dy;
    for (int guard = 0; guard < 512; guard++) {
        plot_depth(target, x0, y0, depth, r, g, b);
        if (x0 == x1 && y0 == y1) return;
        const int e2 = err * 2;
        if (e2 > -dy) { err -= dy; x0 += step_x; }
        if (e2 < dx) { err += dx; y0 += step_y; }
    }
}

void line_flat(const pse::RenderTarget& target, int x0, int y0, int x1, int y1,
               uint8_t r, uint8_t g, uint8_t b) {
    int dx = x1 - x0, dy = y1 - y0;
    const int step_x = dx < 0 ? -1 : 1;
    const int step_y = dy < 0 ? -1 : 1;
    dx = dx < 0 ? -dx : dx;
    dy = dy < 0 ? -dy : dy;
    if (dx > 400 || dy > 400) return;
    int err = dx - dy;
    for (int guard = 0; guard < 512; guard++) {
        pse::plot_pixel(target, x0, y0, r, g, b);
        if (x0 == x1 && y0 == y1) return;
        const int e2 = err * 2;
        if (e2 > -dy) { err -= dy; x0 += step_x; }
        if (e2 < dx) { err += dx; y0 += step_y; }
    }
}

void disc_depth(const pse::RenderTarget& target, int cx, int cy, int radius,
                uint8_t depth, uint8_t r, uint8_t g, uint8_t b) {
    if (radius <= 0) {
        plot_depth(target, cx, cy, depth, r, g, b);
        return;
    }
    const int r2 = radius * radius;
    for (int dy = -radius; dy <= radius; dy++) {
        const int span2 = r2 - dy * dy;
        if (span2 < 0) continue;
        const int span = isqrt_int(span2);
        for (int dx = -span; dx <= span; dx++) {
            plot_depth(target, cx + dx, cy + dy, depth, r, g, b);
        }
    }
}

// ---- the backdrop ----
//
// A sun, two worlds, a moon and a nebula, all at infinity, so they do not
// slide as the player flies but do sweep as the player turns. That is what
// makes a turn read as a turn in a scene with no ground in it: without a
// backdrop, rolling a fighter in empty space looks exactly like not rolling
// it.

struct Body {
    float dx, dy, dz;      // direction, unit length
    int16_t radius;        // pixels at the 120 wide render size
    uint8_t r, g, b;
    uint8_t halo;          // halo radius as a multiple of 16 of `radius`
    bool is_sun;
};

// Directions are hand placed so the set is spread around the sky rather than
// clustered: with a 82 degree lens, two bodies within 40 degrees of each other
// are almost always in shot together and the sky looks half empty everywhere
// else.
const Body k_bodies[] = {
    // The sun, low and behind the battle's opening heading. The halo is kept
    // to about twice the disc: at three times it filled a third of the frame
    // and washed out everything flying in front of it.
    {-0.52f, 0.18f, 0.83f, 8, 255, 246, 214, 36, true},
    // A banded gas giant, the big one.
    {0.74f, -0.10f, 0.66f, 21, 196, 148, 96, 0, false},
    // Its moon, close by and small, which is what says the giant is a giant.
    {0.60f, 0.14f, 0.79f, 5, 170, 172, 178, 0, false},
    // A cold world off to port.
    {-0.83f, -0.34f, -0.44f, 12, 108, 150, 186, 0, false},
    // Nebula: a huge dim smear with no lit side, drawn as pure halo.
    {0.10f, 0.62f, -0.78f, 3, 84, 54, 116, 210, false},
};
constexpr int k_body_count = static_cast<int>(sizeof(k_bodies) / sizeof(Body));

constexpr int k_star_count = 84;
struct StarDir { int8_t x, y, z, bright; };
StarDir g_stars[k_star_count];
bool g_stars_ready = false;

void seed_stars() {
    // Generated once rather than written out as a table, and kept in RAM
    // rather than flash, because 84 directions is 336 bytes either way and
    // this way the field can be resized by changing one number. Rejection
    // sampled in the cube so the field is even on the sphere: normalising
    // whatever falls in the cube would crowd the corners.
    uint32_t rng = 0x51A2C0DEu;
    int made = 0;
    for (int guard = 0; guard < 4000 && made < k_star_count; guard++) {
        rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
        const int x = static_cast<int>((rng >> 2) & 0xFF) - 128;
        const int y = static_cast<int>((rng >> 11) & 0xFF) - 128;
        const int z = static_cast<int>((rng >> 20) & 0xFF) - 128;
        const int len2 = x * x + y * y + z * z;
        if (len2 > 16384 || len2 < 1024) continue;   // outside the shell
        const int len = isqrt_int(len2);
        g_stars[made].x = static_cast<int8_t>((x * 120) / len);
        g_stars[made].y = static_cast<int8_t>((y * 120) / len);
        g_stars[made].z = static_cast<int8_t>((z * 120) / len);
        g_stars[made].bright = static_cast<int8_t>(90 + ((rng >> 28) & 7) * 22);
        made++;
    }
    for (; made < k_star_count; made++) g_stars[made] = StarDir{0, 120, 0, 100};
    g_stars_ready = true;
}

// Project a direction at infinity. Returns false when it is behind the camera
// or so far off axis that the projection is not worth trusting.
bool project_sky(float dx, float dy, float dz, int& sx, int& sy) {
    if (dot_cam(g_cam.forward, dx, dy, dz) < 0.15f) return false;
    int depth = 0;
    return g_renderer.project(g_cam.x + dx * k_sky_radius,
                              g_cam.y + dy * k_sky_radius,
                              g_cam.z + dz * k_sky_radius, sx, sy, depth);
}

void draw_body(const pse::RenderTarget& target, const Body& body,
               const float sun[3]) {
    int cx = 0, cy = 0;
    if (!project_sky(body.dx, body.dy, body.dz, cx, cy)) return;

    const int radius = body.radius;
    const int halo = (radius * body.halo) / 16;
    const int reach = radius > halo ? radius : halo;
    if (cx + reach < 0 || cx - reach >= target.width) return;
    if (cy + reach < 0 || cy - reach >= target.height) return;

    // Where the sun is, in the CAMERA's frame, so the lit side of a world
    // stays lit as the player rolls past it. lz is how much the sun is behind
    // the camera: at +1 the world is a full disc, at -1 a thin crescent.
    const float lx = dot_cam(g_cam.right, sun[0], sun[1], sun[2]);
    const float ly = dot_cam(g_cam.up, sun[0], sun[1], sun[2]);
    const float lz = dot_cam(g_cam.forward, sun[0], sun[1], sun[2]);

    const int r2 = radius * radius;
    const int halo2 = halo * halo;
    const int inv_r = radius > 0 ? (256 / radius) : 256;

    for (int dy = -reach; dy <= reach; dy++) {
        const int y = cy + dy;
        if (y < 0 || y >= target.height) continue;
        for (int dx = -reach; dx <= reach; dx++) {
            const int x = cx + dx;
            if (x < 0 || x >= target.width) continue;
            const int rr = dx * dx + dy * dy;

            if (rr <= r2) {
                int shade;
                if (body.is_sun) {
                    shade = 255;
                } else {
                    // A flat gradient across the disc rather than a real
                    // sphere normal. A sphere normal needs a square root per
                    // pixel, and at twenty one pixels across nobody can tell
                    // a straight terminator from a curved one.
                    const int across = ((dx * static_cast<int>(lx * 128.0f)) -
                                        (dy * static_cast<int>(ly * 128.0f))) *
                                       inv_r / 256;
                    shade = 96 + across + static_cast<int>(lz * 70.0f);
                    shade = clamp_int(shade, 26, 255);
                }
                plot_depth(target, x, y, k_depth_body,
                           clamp8(body.r * shade / 255),
                           clamp8(body.g * shade / 255),
                           clamp8(body.b * shade / 255));
                continue;
            }

            if (halo <= radius || rr > halo2) continue;
            // Falls off with the square of the radius, so the glow is tight
            // around the disc rather than a flat plate of colour.
            const int fade = ((halo2 - rr) * 255) / (halo2 - r2);
            const int soft = (fade * fade) / 255;
            if (soft < 8) continue;
            plot_depth(target, x, y, k_depth_body,
                       clamp8(body.r * soft / 255),
                       clamp8(body.g * soft / 255),
                       clamp8(body.b * soft / 255));
        }
    }
}

void draw_backdrop(const pse::RenderTarget& target) {
    if (!g_stars_ready) seed_stars();

    g_renderer.set_depth_range(k_sky_near, k_sky_far);

    // Bodies first, stars after: a star that lands on a planet then fails its
    // depth test against the planet, so nothing has to sort them.
    const float sun[3] = {k_bodies[0].dx, k_bodies[0].dy, k_bodies[0].dz};
    for (int i = 0; i < k_body_count; i++) {
        draw_body(target, k_bodies[i], sun);
    }

    for (int i = 0; i < k_star_count; i++) {
        const StarDir& s = g_stars[i];
        int sx = 0, sy = 0;
        if (!project_sky(s.x / 120.0f, s.y / 120.0f, s.z / 120.0f, sx, sy)) {
            continue;
        }
        const uint8_t v = static_cast<uint8_t>(s.bright);
        plot_depth(target, sx, sy, k_depth_star, v, v,
                   static_cast<uint8_t>(v > 235 ? 255 : v + 20));
    }
}

// ---- the battle ----

const pse::MeshData& mesh_for(sl::Hull cls) {
    switch (cls) {
        case sl::Hull::Bomber:  return models::starlance::bomber;
        case sl::Hull::Gunship: return models::starlance::gunship;
        case sl::Hull::Frigate: return models::starlance::frigate;
        default:                return models::starlance::fighter;
    }
}

float camera_distance(int32_t x, int32_t y, int32_t z) {
    const float dx = fp_to_f(x) - g_cam.x;
    const float dy = fp_to_f(y) - g_cam.y;
    const float dz = fp_to_f(z) - g_cam.z;
    const float d2 = dx * dx + dy * dy + dz * dz;
    // No sqrtf in a loop over ships: the bracket only needs an ordering and a
    // rough magnitude, and the integer root is exact enough for both.
    return static_cast<float>(isqrt_int(static_cast<int>(d2)));
}

// Where the camera would sit if it kept up perfectly, and which way it would
// be looking. The easing below chases this, it does not replace it.
void ideal_camera(const World& world, const Chrome& chrome, Camera& out) {
    pse::Basis basis;
    sl::player_basis(world, basis);

    const float ship_right[3] = {basis.m[0], basis.m[3], basis.m[6]};
    const float ship_up[3] = {basis.m[1], basis.m[4], basis.m[7]};
    const float ship_fwd[3] = {basis.m[2], basis.m[5], basis.m[8]};

    // The seat is always behind and above the hull, whichever way the camera
    // ends up looking. Padlock turns the head, it does not move the chair.
    out.x = fp_to_f(world.x) - ship_fwd[0] * k_cam_back + ship_up[0] * k_cam_lift;
    out.y = fp_to_f(world.y) - ship_fwd[1] * k_cam_back + ship_up[1] * k_cam_lift;
    out.z = fp_to_f(world.z) - ship_fwd[2] * k_cam_back + ship_up[2] * k_cam_lift;

    const sl::Ship* target = sl::target_ship(world);
    if (chrome.look_at_target && target != nullptr) {
        int32_t tx = target->x, ty = target->y, tz = target->z;
        const sl::Subsystem* sub = sl::target_subsystem(world);
        if (sub != nullptr) sl::sub_position(*target, *sub, tx, ty, tz);

        float look[3] = {fp_to_f(tx) - out.x, fp_to_f(ty) - out.y,
                         fp_to_f(tz) - out.z};
        if (normalize3(look)) {
            out.forward[0] = look[0];
            out.forward[1] = look[1];
            out.forward[2] = look[2];

            // Rolled with the ship rather than levelled to the world: this is
            // a pilot turning their head, and their head is attached to a hull
            // that may well be inverted.
            //
            // The ship's UP is the reference, not its right, and that is not
            // interchangeable. Taking right as the reference and squaring it
            // against the look direction collapses when the two are parallel,
            // which is to say whenever the target is directly abeam, which is
            // exactly the case padlock exists for: the roll would flip to a
            // world axis as the contact crossed the wingtip. Up only collapses
            // when the target is straight overhead or underneath, and the
            // ship's nose covers that.
            float hint[3] = {ship_up[0], ship_up[1], ship_up[2]};
            cross3(hint, out.forward, out.right);
            if (!normalize3(out.right)) {
                hint[0] = ship_fwd[0]; hint[1] = ship_fwd[1];
                hint[2] = ship_fwd[2];
                cross3(hint, out.forward, out.right);
                normalize3(out.right);
            }
            cross3(out.forward, out.right, out.up);
            return;
        }
    }

    for (int i = 0; i < 3; i++) {
        out.right[i] = ship_right[i];
        out.up[i] = ship_up[i];
        out.forward[i] = ship_fwd[i];
    }
}

void set_up_camera(const World& world, const Chrome& chrome, uint32_t time_ms) {
    Camera want{};
    ideal_camera(world, chrome, want);

    // Per frame in the pico-8 original, which ran at a fixed 60. This one does
    // not, so the rate is scaled by how long the frame actually took. A fixed
    // per frame factor would make the camera lag depend on the frame rate,
    // which is the sort of thing that feels fine on a laptop and sluggish on
    // the device.
    const uint32_t dt_ms = time_ms > g_cam_last_ms ? time_ms - g_cam_last_ms : 0;
    g_cam_last_ms = time_ms;
    float f = k_cam_lerp * (static_cast<float>(dt_ms) / k_cam_frame_ms);
    if (f > 1.0f) f = 1.0f;
    if (f < 0.0f) f = 0.0f;

    const float dx = want.x - g_cam.x, dy = want.y - g_cam.y;
    const float dz = want.z - g_cam.z;
    const bool jumped = dx * dx + dy * dy + dz * dz > k_cam_snap * k_cam_snap;

    if (!g_cam_seeded || jumped) {
        g_cam = want;
        g_cam_seeded = true;
    } else {
        g_cam.x += dx * f;
        g_cam.y += dy * f;
        g_cam.z += dz * f;
        for (int i = 0; i < 3; i++) {
            g_cam.right[i] += (want.right[i] - g_cam.right[i]) * f;
            g_cam.up[i] += (want.up[i] - g_cam.up[i]) * f;
            g_cam.forward[i] += (want.forward[i] - g_cam.forward[i]) * f;
        }
        orthonormalize(g_cam);
    }

    pse::Basis basis;
    basis.m[0] = g_cam.right[0]; basis.m[1] = g_cam.up[0];
    basis.m[2] = g_cam.forward[0];
    basis.m[3] = g_cam.right[1]; basis.m[4] = g_cam.up[1];
    basis.m[5] = g_cam.forward[1];
    basis.m[6] = g_cam.right[2]; basis.m[7] = g_cam.up[2];
    basis.m[8] = g_cam.forward[2];

    g_renderer.set_fov(k_fov);
    g_renderer.set_camera_basis(g_cam.x, g_cam.y, g_cam.z, basis);
}

// Bracket the depth range to what this frame actually holds. See the constants
// above for why this is not optional.
void bracket_depth(const World& world) {
    float nearest = 1e9f, farthest = 0.0f;
    bool any = false;

    auto consider = [&](int32_t x, int32_t y, int32_t z, float pad) {
        const float d = camera_distance(x, y, z);
        if (d > k_far_ceiling) return;
        const float lo = d - pad, hi = d + pad;
        if (lo < nearest) nearest = lo;
        if (hi > farthest) farthest = hi;
        any = true;
    };

    for (uint8_t i = 0; i < sl::k_max_ships; i++) {
        const sl::Ship& ship = world.ships[i];
        if (!ship.active) continue;
        consider(ship.x, ship.y, ship.z, fp_to_f(sl::hull_length(ship.cls)));
    }
    for (uint8_t i = 0; i < sl::k_max_bolts; i++) {
        if (world.shots[i].active) {
            consider(world.shots[i].x, world.shots[i].y, world.shots[i].z, 1.0f);
        }
    }
    for (uint8_t i = 0; i < sl::k_max_blasts; i++) {
        if (world.blasts[i].active) {
            consider(world.blasts[i].x, world.blasts[i].y, world.blasts[i].z,
                     2.0f);
        }
    }

    if (!any) { nearest = 20.0f; farthest = 200.0f; }
    if (nearest < 1.0f) nearest = 1.0f;

    float near_plane = nearest * k_near_fraction;
    if (near_plane < k_near_floor) near_plane = k_near_floor;
    float far_plane = farthest * k_far_fraction;
    if (far_plane > k_far_ceiling) far_plane = k_far_ceiling;
    if (far_plane < near_plane * 6.0f) far_plane = near_plane * 6.0f;

    g_world_near = near_plane;
    g_world_far = far_plane;
    g_renderer.set_depth_range(near_plane, far_plane);
    g_stats.near_units = static_cast<uint16_t>(near_plane);
    g_stats.far_units = static_cast<uint16_t>(far_plane);
}

// Put the lens back where the world was drawn with it. Anything that projects
// a world point after the hull or the backdrop passes has to call this first.
void restore_world_lens() {
    g_renderer.set_depth_range(g_world_near, g_world_far);
}

// Below this many pixels of projected half size, a hull is drawn as a contact
// blob rather than as a mesh.
//
// Not an optimisation. A ship 5 pixels across has 36 triangles each covering
// less than a pixel, and the rasterizer's backface test is a signed screen
// area, so every one of them comes out zero and is culled: the ship does not
// get smaller, it disappears completely. A fighter at thirty units was simply
// not in the frame, with a target box drawn neatly around the empty space
// where it should have been.
constexpr float k_lod_pixels = 4.0f;

// What a contact is drawn as when it is too small to be a shape. Colour per
// class, so a dot at two hundred units still says whether it is a fighter or
// something that needs a wing.
void contact_colour(sl::Hull cls, uint8_t& r, uint8_t& g, uint8_t& b) {
    switch (cls) {
        case sl::Hull::Bomber:  r = 232; g = 176; b = 96; break;
        case sl::Hull::Gunship: r = 168; g = 206; b = 150; break;
        case sl::Hull::Frigate: r = 176; g = 196; b = 226; break;
        default:                r = 234; g = 110; b = 92; break;
    }
}

bool too_small_to_draw(const World& world, const sl::Ship& ship,
                       float& out_scale) {
    int x = 0, y = 0;
    uint8_t depth = 0;
    out_scale = 0.0f;
    if (!g_renderer.project_billboard(fp_to_f(ship.x), fp_to_f(ship.y),
                                      fp_to_f(ship.z),
                                      fp_to_f(sl::hull_length(ship.cls)), x, y,
                                      out_scale, depth)) {
        // Off screen or behind: nothing to draw either way, and the mesh path
        // would reach the same conclusion more slowly.
        return true;
    }
    return out_scale < k_lod_pixels;
}

void draw_hulls(const World& world) {
    g_stats.hulls_drawn = 0;
    g_stats.hulls_live = 0;

    for (uint8_t i = 0; i < sl::k_max_ships; i++) {
        const sl::Ship& ship = world.ships[i];
        if (!ship.active) continue;
        g_stats.hulls_live++;

        if (sl::range_to(world, ship) > sl::k_draw_range) continue;

        float scale = 0.0f;
        if (too_small_to_draw(world, ship, scale)) continue;
        g_stats.hulls_drawn++;

        // A hull that has just been hit whitens. draw_mesh lerps toward white
        // AFTER the tint, which is the only way to brighten a mesh: the tint
        // multiplies, so it can darken and recolour and never lighten.
        const uint8_t flash =
            ship.hit_flash > 0 ? static_cast<uint8_t>(ship.hit_flash * 34) : 0;

        // A derelict is a dead grey. Nothing else says "that one has stopped
        // fighting" on a hull twelve pixels across.
        const bool dead_crew = ship.task == sl::Task::Derelict;
        const uint8_t tint = dead_crew ? 130 : 255;

        g_renderer.draw_mesh(mesh_for(ship.cls), fp_to_f(ship.x),
                             fp_to_f(ship.y), fp_to_f(ship.z),
                             pse::quat_basis(ship.q),
                             fp_to_f(sl::hull_length(ship.cls)), tint, tint,
                             tint, flash);
    }
}

// Hulls too small to be shapes, drawn as depth tested blobs after the
// geometry. See k_lod_pixels for why this exists at all.
void draw_far_contacts(const World& world, const pse::RenderTarget& target) {
    for (uint8_t i = 0; i < sl::k_max_ships; i++) {
        const sl::Ship& ship = world.ships[i];
        if (!ship.active) continue;
        if (sl::range_to(world, ship) > sl::k_draw_range) continue;

        float scale = 0.0f;
        if (!too_small_to_draw(world, ship, scale)) continue;
        if (scale <= 0.0f) continue;      // off screen, not merely small

        int x = 0, y = 0;
        uint8_t depth = 0;
        if (!g_renderer.project_billboard(fp_to_f(ship.x), fp_to_f(ship.y),
                                          fp_to_f(ship.z),
                                          fp_to_f(sl::hull_length(ship.cls)), x,
                                          y, scale, depth)) {
            continue;
        }

        g_stats.hulls_drawn++;

        uint8_t r, g, b;
        contact_colour(ship.cls, r, g, b);
        if (ship.task == sl::Task::Derelict) { r = 120; g = 120; b = 126; }

        // One pixel at the limit of sight, growing to the size the mesh takes
        // over at, so a contact closing on you does not pop from a dot to a
        // ship in one frame.
        const int radius = clamp_int(static_cast<int>(scale) - 1, 0, 3);
        disc_depth(target, x, y, radius, depth, r, g, b);
    }
}

// A short bright streak rather than a dot: a bolt crossing the frame in half a
// second is two pixels of dot per frame, which reads as noise. Drawn after the
// geometry so it can be depth tested against the ships it is flying past.
void draw_bolts(const World& world, const pse::RenderTarget& target) {
    for (uint8_t i = 0; i < sl::k_max_bolts; i++) {
        const sl::Shot& shot = world.shots[i];
        if (!shot.active) continue;

        const float hx = fp_to_f(shot.x), hy = fp_to_f(shot.y);
        const float hz = fp_to_f(shot.z);
        const float tx = hx - fp_to_f(shot.vx) * 2.2f;
        const float ty = hy - fp_to_f(shot.vy) * 2.2f;
        const float tz = hz - fp_to_f(shot.vz) * 2.2f;

        int x0 = 0, y0 = 0, d0 = 0, x1 = 0, y1 = 0, d1 = 0;
        if (!g_renderer.project(hx, hy, hz, x0, y0, d0)) continue;
        if (!g_renderer.project(tx, ty, tz, x1, y1, d1)) continue;

        const uint8_t depth =
            static_cast<uint8_t>(clamp_int(d0 * 255 / pse::k_fixed_one, 0, 255));

        uint8_t r = 130, g = 245, b = 170;          // player: green
        if (shot.kind == sl::Bolt::EnemyGun) { r = 255; g = 120; b = 90; }
        if (shot.kind == sl::Bolt::TurretShell) { r = 255; g = 200; b = 80; }

        line_depth(target, x0, y0, x1, y1, depth, r, g, b);
        plot_depth(target, x0, y0, depth, 255, 255, 255);
    }
}

void draw_missiles(const World& world, const pse::RenderTarget& target) {
    for (uint8_t i = 0; i < sl::k_max_missiles; i++) {
        const sl::Missile& m = world.missiles_live[i];
        if (!m.active) continue;
        int x = 0, y = 0, depth = 0;
        if (!g_renderer.project(fp_to_f(m.x), fp_to_f(m.y), fp_to_f(m.z), x, y,
                                depth)) {
            continue;
        }
        const uint8_t d =
            static_cast<uint8_t>(clamp_int(depth * 255 / pse::k_fixed_one, 0, 255));
        plot_depth(target, x, y, d, 255, 255, 220);
        plot_depth(target, x, y + 1, d, 255, 170, 60);
        plot_depth(target, x, y - 1, d, 255, 170, 60);
        plot_depth(target, x + 1, y, d, 255, 170, 60);
        plot_depth(target, x - 1, y, d, 255, 170, 60);
    }
}

void draw_blasts(const World& world, const pse::RenderTarget& target) {
    for (uint8_t i = 0; i < sl::k_max_blasts; i++) {
        const sl::Blast& blast = world.blasts[i];
        if (!blast.active) continue;

        const float world_size =
            static_cast<float>(blast.size) / 100.0f *
            (1.0f - static_cast<float>(blast.life) / sl::k_blast_life * 0.55f);

        int x = 0, y = 0;
        float scale = 0.0f;
        uint8_t depth = 0;
        if (!g_renderer.project_billboard(fp_to_f(blast.x), fp_to_f(blast.y),
                                          fp_to_f(blast.z), world_size, x, y,
                                          scale, depth)) {
            continue;
        }

        const int radius = clamp_int(static_cast<int>(scale), 1, 34);
        // Cools from white through amber to a dull red as it ages, which is
        // the whole of the animation: three colours and a growing radius.
        const int age = 255 - (blast.life * 255) / sl::k_blast_life;
        const uint8_t r = 255;
        const uint8_t g = clamp8(240 - age);
        const uint8_t b = clamp8(200 - age * 2);
        disc_depth(target, x, y, radius, depth, r, g, b);
        if (radius > 3) {
            disc_depth(target, x, y, radius / 2, depth, 255, 250, 220);
        }
    }
}

// The player's own hull, in its own depth bracket. See k_hull_near.
void draw_own_hull(const World& world) {
    g_renderer.set_depth_range(k_hull_near, k_hull_far);
    pse::Basis basis;
    sl::player_basis(world, basis);
    const uint8_t flash =
        world.hit_flash > 0 ? static_cast<uint8_t>(world.hit_flash * 22) : 0;
    g_renderer.draw_mesh(models::starlance::interceptor, fp_to_f(world.x),
                         fp_to_f(world.y), fp_to_f(world.z), basis,
                         fp_to_f(sl::hull_length(sl::Hull::Fighter)), 255, 255,
                         255, flash);
}

// ---- the head up display ----

void bar(const pse::RenderTarget& target, int x, int y, int w, int value,
         int maximum, uint8_t r, uint8_t g, uint8_t b) {
    pse::fill_rect(target, x, y, w, 3, 26, 30, 40);
    if (maximum <= 0 || value <= 0) return;
    const int filled = clamp_int(value * w / maximum, 1, w);
    pse::fill_rect(target, x, y, filled, 3, r, g, b);
}

// A bracket at the four corners of a box, which is a target box that never
// hides the thing inside it. A closed rectangle round a fighter twelve pixels
// across covers the fighter.
void corner_box(const pse::RenderTarget& target, int cx, int cy, int half,
                uint8_t r, uint8_t g, uint8_t b) {
    const int arm = clamp_int(half / 2, 2, 5);
    const int x0 = cx - half, x1 = cx + half;
    const int y0 = cy - half, y1 = cy + half;
    line_flat(target, x0, y0, x0 + arm, y0, r, g, b);
    line_flat(target, x0, y0, x0, y0 + arm, r, g, b);
    line_flat(target, x1, y0, x1 - arm, y0, r, g, b);
    line_flat(target, x1, y0, x1, y0 + arm, r, g, b);
    line_flat(target, x0, y1, x0 + arm, y1, r, g, b);
    line_flat(target, x0, y1, x0, y1 - arm, r, g, b);
    line_flat(target, x1, y1, x1 - arm, y1, r, g, b);
    line_flat(target, x1, y1, x1, y1 - arm, r, g, b);
}

void print_uint(char* out, uint32_t value, int width) {
    for (int i = width - 1; i >= 0; i--) {
        out[i] = static_cast<char>('0' + (value % 10));
        value /= 10;
    }
    out[width] = '\0';
}

// The reticle: where the guns actually converge, projected, rather than a
// cross painted at the middle of the screen. The camera sits a little above
// the ship, so the two are not the same point, and a fixed crosshair would be
// a crosshair that lies.
void draw_reticle(const World& world, const pse::RenderTarget& target) {
    pse::Basis basis;
    sl::player_basis(world, basis);
    const float fx = basis.m[2], fy = basis.m[5], fz = basis.m[8];
    const float reach = fp_to_f(sl::k_gun_convergence);

    int x = 0, y = 0, depth = 0;
    if (!g_renderer.project(fp_to_f(world.x) + fx * reach,
                            fp_to_f(world.y) + fy * reach,
                            fp_to_f(world.z) + fz * reach, x, y, depth)) {
        return;
    }
    // Arms set well out from the middle, and a single pixel at the centre.
    // Everything else the HUD draws lands here too, the target box and the
    // lead pip both, and a crosshair with long arms turned the middle of the
    // screen into a knot you could not read a fighter out of.
    const uint8_t r = 120, g = 230, b = 160;
    line_flat(target, x - 9, y, x - 5, y, r, g, b);
    line_flat(target, x + 5, y, x + 9, y, r, g, b);
    line_flat(target, x, y - 9, x, y - 5, r, g, b);
    line_flat(target, x, y + 5, x, y + 9, r, g, b);
    pse::plot_pixel(target, x, y, 200, 255, 210);
}

// Where to aim to hit what is targeted: the target's position plus its own
// velocity over the time a bolt takes to arrive. Without this, hitting
// anything crossing is guesswork, and with it the game is about flying.
void draw_lead(const World& world, const sl::Ship& ship, int box_x, int box_y,
               const pse::RenderTarget& target) {
    const int32_t range = sl::range_to(world, ship);
    const int32_t flight = range / sl::k_gun_speed;
    if (flight > 400) return;

    int32_t fwd[3];
    pse::quat_rotate(ship.q, 0, 0, pse::k_quat_one, fwd[0], fwd[1], fwd[2]);
    const float travel = fp_to_f(ship.speed) * static_cast<float>(flight);

    int x = 0, y = 0, depth = 0;
    if (!g_renderer.project(
            fp_to_f(ship.x) + fwd[0] / 16384.0f * travel,
            fp_to_f(ship.y) + fwd[1] / 16384.0f * travel,
            fp_to_f(ship.z) + fwd[2] / 16384.0f * travel, x, y, depth)) {
        return;
    }

    // Only when it is somewhere the target box is not. A contact coming
    // straight at you needs no lead at all, so the pip lands on top of the box
    // and adds a knot of pixels that says nothing: two marks in the same place
    // are harder to read than one.
    const int off_x = x - box_x, off_y = y - box_y;
    if (off_x * off_x + off_y * off_y < 16) return;

    // A hollow diamond, so it stays legible on top of the hull it is sitting
    // over. Filled, it reads as part of the ship.
    line_flat(target, x, y - 3, x + 3, y, 250, 220, 90);
    line_flat(target, x + 3, y, x, y + 3, 250, 220, 90);
    line_flat(target, x, y + 3, x - 3, y, 250, 220, 90);
    line_flat(target, x - 3, y, x, y - 3, 250, 220, 90);
}

// An arrow at the edge of the screen for a target that is not in shot. This is
// the instrument that makes the target cycle usable: without it, pressing Y
// on to something behind you selects a contact you then cannot find.
void draw_off_screen_arrow(const World& world, int32_t tx, int32_t ty,
                           int32_t tz, const pse::RenderTarget& target) {
    int32_t bx, by, bz;
    sl::bearing(world, tx, ty, tz, bx, by, bz);

    // The bearing is in the player's frame, so its x and y ARE screen right
    // and screen up, with y flipped for the raster. A target dead astern has
    // no direction on screen at all, so it gets pushed to straight down.
    int dx = bx, dy = -by;
    if (dx == 0 && dy == 0) dy = 1;
    const int mag = isqrt_int(dx * dx + dy * dy);
    if (mag == 0) return;

    const int cx = target.width / 2, cy = target.height / 2;
    const int reach = (target.width * 5) / 12;
    const int ax = cx + (dx * reach) / mag;
    const int ay = cy + (dy * reach) / mag;

    // A stub pointing outward, thick enough to see at 120 pixels.
    const int tipx = cx + (dx * (reach + 5)) / mag;
    const int tipy = cy + (dy * (reach + 5)) / mag;
    line_flat(target, ax, ay, tipx, tipy, 250, 210, 80);
    line_flat(target, ax - 1, ay, tipx, tipy, 250, 210, 80);
    line_flat(target, ax, ay - 1, tipx, tipy, 250, 210, 80);
    if (bz < 0) {
        // Behind you. One more pixel out, so "ahead but off screen" and
        // "astern" are not the same picture.
        pse::plot_pixel(target, tipx, tipy, 255, 120, 80);
    }
}

void draw_target_hud(const World& world, const pse::RenderTarget& target) {
    const sl::Ship* ship = sl::target_ship(world);
    if (ship == nullptr) return;

    const sl::Subsystem* sub = sl::target_subsystem(world);
    int32_t mark_x = ship->x, mark_y = ship->y, mark_z = ship->z;
    if (sub != nullptr) sl::sub_position(*ship, *sub, mark_x, mark_y, mark_z);

    int x = 0, y = 0;
    float scale = 0.0f;
    uint8_t depth = 0;
    const float size = sub != nullptr
                           ? fp_to_f(sl::sub_radius(*ship, *sub))
                           : fp_to_f(sl::hull_radius(ship->cls));

    const bool on_screen = g_renderer.project_billboard(
        fp_to_f(mark_x), fp_to_f(mark_y), fp_to_f(mark_z), size, x, y, scale,
        depth) &&
        x >= 0 && x < target.width && y >= 0 && y < target.height;

    if (on_screen) {
        // Never smaller than the reticle's own arms, so the box encloses the
        // crosshair rather than sitting inside it.
        const int half = clamp_int(static_cast<int>(scale), 6, 40);
        if (sub != nullptr) {
            corner_box(target, x, y, half, 250, 200, 70);
        } else {
            corner_box(target, x, y, half, 110, 220, 250);
        }
        draw_lead(world, *ship, x, y, target);
    } else {
        draw_off_screen_arrow(world, mark_x, mark_y, mark_z, target);
    }

    // The panel: what it is, how far, and how much is left of it.
    char line[24];
    const char* name = sub != nullptr ? sl::sub_name(sub->kind)
                                      : sl::hull_name(ship->cls);
    pse::draw_text(target, name, 2, 2, 150, 220, 250);

    const int32_t range = sl::range_to(world, *ship) / sl::k_one;
    print_uint(line, static_cast<uint32_t>(clamp_int(range, 0, 999)), 3);
    pse::draw_text(target, line, target.width - 2 - pse::text_width(line, 1), 2,
                   150, 220, 250);

    if (sub != nullptr) {
        bar(target, 2, 11, 46, sub->hull, sub->hull_max, 250, 190, 60);
    } else {
        bar(target, 2, 11, 46, ship->hull, ship->hull_max, 240, 90, 80);
        if (ship->shield_max > 0) {
            bar(target, 2, 15, 46, ship->shield, ship->shield_max, 90, 170, 250);
        }
    }
}

void draw_status(const World& world, const pse::RenderTarget& target) {
    // Three bars, no labels: hull red, shields blue, throttle green. A label
    // on a 120 pixel screen costs more room than the bar it names, and colour
    // is enough when the same three sit in the same order every frame.
    const int base = target.height - 17;
    bar(target, 2, base, 40, world.hull, sl::k_player_hull_max, 240, 90, 80);
    bar(target, 2, base + 5, 40, world.shield, sl::k_player_shield_max, 90, 170,
        250);
    bar(target, 2, base + 10, 40, world.throttle, sl::k_throttle_one, 120, 220,
        130);

    // The speed actually being made, drawn as a notch on the throttle bar.
    // The lever and the ship disagree for about a fifth of a second after a
    // change, and that gap IS the feel of the throttle: without it the bar
    // says the ship has already done what it has only been asked to do.
    if (sl::k_player_speed_max > 0) {
        const int notch = clamp_int(
            static_cast<int>((static_cast<int64_t>(world.speed) * 40) /
                             sl::k_player_speed_max), 0, 39);
        pse::fill_rect(target, 2 + notch, base + 9, 1, 5, 230, 255, 210);
    }

    // Missiles as pips rather than a number. A count needs a label to say what
    // it counts; six squares next to a launcher bar do not.
    for (int i = 0; i < sl::k_missiles_max; i++) {
        const bool have = i < world.missiles;
        pse::fill_rect(target, 46 + i * 3, base + 4, 2, 6,
                       have ? 250 : 46, have ? 220 : 50, have ? 110 : 62);
    }
}

void draw_mission(const World& world, const pse::RenderTarget& target,
                  uint32_t time_ms) {
    char line[24];

    // The jump clock, low and centred rather than along the top.
    //
    // The top row already carries the target's name on the left and its range
    // on the right, and a seven letter class name reaches the middle of a 120
    // pixel screen: FRIGATE and JUMP 86 were printing through each other,
    // which is exactly what rule 9's "measure text" is about, in the one
    // place measuring a single string would not have caught it. This row is
    // clear, and it is still the only thing in the game that can be lost by
    // being ignored.
    const int clock_y = target.height - 22;
    const uint32_t left = sl::jump_ticks_left(world);
    if (left > 0) {
        const uint32_t seconds = left / 100;
        print_uint(line, seconds, 2);
        char banner[16] = {'J', 'U', 'M', 'P', ' ', line[0], line[1], '\0'};
        const bool urgent = seconds < 20;
        const bool blink = urgent && ((time_ms / 300) & 1);
        pse::draw_text_centred(target, banner, target.width / 2, clock_y,
                               urgent ? 255 : 250, blink ? 90 : 190, 60);
    } else if (world.jump_stopped) {
        pse::draw_text_centred(target, "NAV DOWN", target.width / 2, clock_y,
                               120, 240, 150);
    }

    if (world.phase == sl::Phase::Briefing) {
        print_uint(line, world.wave, 1);
        char banner[16] = {'W', 'A', 'V', 'E', ' ', line[0], '\0'};
        pse::draw_text_centred(target, banner, target.width / 2,
                               target.height / 2 - 26, 200, 230, 255, 2);
    }

    // One row above the clock, so the two warnings never share a line.
    if (world.out_of_bounds && ((time_ms / 250) & 1)) {
        pse::draw_text_centred(target, "TURN BACK", target.width / 2,
                               clock_y - 10, 255, 120, 90);
    }
}

// ---- the screens ----

// A panel sized from the text it holds, both ways. Rule 9: measure text,
// never place it by eye. A hand picked width is only correct for the exact
// strings it was tuned against, and the first wording change prints through
// the edge of its own panel.
void panel(const pse::RenderTarget& target, const char* const* lines,
           const uint8_t* highlight, int count, int top, int scale) {
    int widest = 0;
    for (int i = 0; i < count; i++) {
        const int w = pse::text_width(lines[i], scale);
        if (w > widest) widest = w;
    }
    const int line_h = pse::text_height(scale) + 3;
    const int height = count * line_h + 5;
    const int x = (target.width - widest) / 2 - 4;

    pse::fill_rect(target, x, top - 3, widest + 8, height, 12, 16, 28);
    pse::fill_rect(target, x, top - 3, widest + 8, 1, 60, 90, 130);
    pse::fill_rect(target, x, top - 4 + height, widest + 8, 1, 60, 90, 130);

    for (int i = 0; i < count; i++) {
        const bool lit = highlight != nullptr && highlight[i] != 0;
        pse::draw_text_centred(target, lines[i], target.width / 2,
                               top + i * line_h, lit ? 255 : 130,
                               lit ? 240 : 150, lit ? 160 : 170, scale);
    }
}

const char* sound_word(bool on) { return on ? "SOUND ON" : "SOUND OFF"; }
const char* pitch_word(bool invert) {
    return invert ? "PITCH INVERTED" : "PITCH NORMAL";
}

void draw_title(const Chrome& chrome, const pse::RenderTarget& target) {
    pse::draw_text_centred(target, "STARLANCE", target.width / 2, 24, 190, 225,
                           255, 2);
    pse::draw_text_centred(target, "5TH WING", target.width / 2, 40, 90, 130,
                           170);

    const char* lines[kTitleItemCount] = {"LAUNCH", sound_word(chrome.sound_on),
                                          pitch_word(chrome.invert_pitch)};
    uint8_t lit[kTitleItemCount] = {0, 0, 0};
    lit[chrome.item % kTitleItemCount] = 1;
    panel(target, lines, lit, kTitleItemCount, 62, 1);

    if (chrome.best_score > 0) {
        char line[16];
        print_uint(line, chrome.best_score, 6);
        pse::draw_text_centred(target, line, target.width / 2,
                               target.height - 12, 120, 150, 190);
    }
}

void draw_pause(const Chrome& chrome, const pse::RenderTarget& target) {
    const char* lines[kPauseItemCount] = {"RESUME", sound_word(chrome.sound_on),
                                          pitch_word(chrome.invert_pitch),
                                          "ABORT SORTIE"};
    uint8_t lit[kPauseItemCount] = {0, 0, 0, 0};
    lit[chrome.item % kPauseItemCount] = 1;
    panel(target, lines, lit, kPauseItemCount, 40, 1);
}

void draw_debrief(const World& world, const Chrome& chrome,
                  const pse::RenderTarget& target) {
    const char* verdict = "WING LOST";
    if (world.phase == sl::Phase::Won) {
        verdict = "FRIGATE DOWN";
    } else if (world.loss == sl::Loss::Jumped) {
        verdict = "SHE JUMPED";
    }

    char score[16];
    print_uint(score, world.score, 6);
    char tally[16];
    print_uint(tally, world.kills, 2);
    char subs[16];
    print_uint(subs, world.subs_killed, 2);

    char kills_line[24] = {'K', 'I', 'L', 'L', 'S', ' ', tally[0], tally[1],
                           ' ', ' ', 'S', 'U', 'B', ' ', subs[0], subs[1],
                           '\0'};

    const char* lines[3] = {verdict, score, kills_line};
    panel(target, lines, nullptr, 3, 46, 1);

    if (world.score >= chrome.best_score && world.score > 0) {
        pse::draw_text_centred(target, "BEST", target.width / 2,
                               target.height - 16, 250, 220, 110);
    }
}

}  // namespace

void render_scene(const World& world, const Chrome& chrome,
                  const pse::RenderTarget& target, uint32_t time_ms) {
    set_up_camera(world, chrome, time_ms);

    // ---- the world, collected and split across both cores ----
    g_raster.begin_frame_collect(target, g_queue);
    bracket_depth(world);
    draw_hulls(world);

    g_stats.queued = g_queue.count;
    g_stats.dropped = g_queue.dropped;

    // Deep space is not black: a very dark blue holds the difference between
    // "the sky" and "a hole where a ship should be", which pure black does
    // not, and the gradient gives the frame an up.
    pse::run_split(g_raster, g_queue, pse::SkyGradient{7, 9, 22, 3, 4, 11});
    g_raster.end_collect();

    // ---- immediate passes, over the geometry and its depth buffer ----
    draw_far_contacts(world, target);
    draw_bolts(world, target);
    draw_missiles(world, target);
    draw_blasts(world, target);
    draw_own_hull(world);
    draw_backdrop(target);

    // ---- the instruments ----
    //
    // Back on the world's lens first: the two passes above both moved it, and
    // every instrument below projects a world point.
    restore_world_lens();
    if (chrome.screen == Screen::Play || chrome.screen == Screen::Paused) {
        draw_reticle(world, target);
        draw_target_hud(world, target);
        draw_status(world, target);
        draw_mission(world, target, time_ms);
    }

    switch (chrome.screen) {
        case Screen::Title:    draw_title(chrome, target); break;
        case Screen::Paused:   draw_pause(chrome, target); break;
        case Screen::Debrief:  draw_debrief(world, chrome, target); break;
        case Screen::Play:     break;
    }
}

FrameStats last_frame_stats() { return g_stats; }

CameraState last_camera() {
    CameraState out{};
    out.x = g_cam.x; out.y = g_cam.y; out.z = g_cam.z;
    for (int i = 0; i < 3; i++) {
        out.right[i] = g_cam.right[i];
        out.up[i] = g_cam.up[i];
        out.forward[i] = g_cam.forward[i];
    }
    return out;
}

}  // namespace slr
