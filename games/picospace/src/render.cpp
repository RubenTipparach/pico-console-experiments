#include "render.hpp"

#include <cmath>

#include "pse/parallel.hpp"
#include "pse/raster.hpp"
#include "pse/renderer3d.hpp"
#include "pse/shared_render.hpp"
#include "pse/text.hpp"

#include "picospace/booster.hpp"
#include "picospace/lander.hpp"
#include "picospace/legs.hpp"
#include "picospace/pad.hpp"

namespace psr {
namespace {

using ps::World;

// The Rasterizer and the FrameQueue come from the engine rather than being
// declared here: on the console every game is linked into one binary, and a
// 14 KB depth buffer plus a 21 KB triangle queue per game is RAM spent on
// scenes nothing is rendering. A standalone build gets exactly one instance.
pse::Rasterizer& g_raster = pse::shared_rasterizer();
pse::Renderer3D g_renderer(g_raster);
pse::FrameQueue& g_queue = pse::shared_queue();

FrameStats g_stats{};

constexpr float k_pi = 3.14159265f;

// ---- the frame the flight view is drawn in --------------------------------
//
// The sim flies in a plane. The VIEW does not: it sits above that plane and
// looks down at a sphere, which is what makes a launch read as a rocket
// leaving a world rather than as a cross section of one.
//
// Every frame is built in a coordinate system centred on the ship:
//
//   +y  straight up away from the body it is nearest, so "up" on screen is
//       always the way the ship would fall, whatever quarter of the planet it
//       is over. This is the reason for having a local frame at all: the
//       engine's camera has world +y for its up vector, so a ship a quarter of
//       the way round the planet would otherwise fly sideways across the
//       screen with the ground up the left hand edge.
//   +x  along the ship's track, the way its bearing DECREASES, and the sign
//       there is the whole of a bug worth writing down. The obvious choice is
//       increasing bearing, and it makes the frame left handed against the
//       map: the map plots world x right and world y up, which puts its
//       viewer on one side of the orbital plane, and increasing-bearing-is-
//       right puts this camera on the other. Both pictures were internally
//       consistent and they were mirror images of each other, so a ship
//       climbing away to the left on the map had its prograde marker off to
//       the right out of the window.
//   +z  out of the orbital plane, which nothing in the sim ever moves along
//       and everything in the picture needs. It is x cross y, and with x as
//       above that puts the camera on the same side of the plane as the map's
//       viewer, which is the whole point.
//
// Everything that turns an in plane WORLD direction into this frame goes
// through local_dir below, so the sign lives in one place. A world direction
// at angle t, seen from a ship at bearing b, is (-sin(t - b), cos(t - b)):
// straight up the screen when the two agree, and swinging left as t runs
// ahead of b.
//
// The camera sits behind and above the origin and looks at it, so the ship is
// always dead centre and the world turns under it.
constexpr float k_fov = 55.0f;
constexpr float k_cam_dist = 54.0f;

// How far the camera is lifted out of the orbital plane. High near the ground,
// where a three quarter view over the terrain is what a launch and a landing
// want, and low in space, where the ship should be a silhouette against the
// limb rather than a plan view of itself.
constexpr float k_cam_high_low = 27.0f;
constexpr float k_cam_high_space = 12.0f;

// Two things are pinned, and the rest of the framing follows from them:
//
//   the ship is always the same size on screen, k_ship_draw view units per
//   metre, so a rocket is legible on the pad and legible from orbit;
//
//   the ground directly under the ship is always k_horizon_drop view units
//   below it, so the world does not slide off the bottom of the frame at
//   200 m and never come back.
//
// The second of those IS the world's scale: k_horizon_drop / height. At thirty
// metres up it comes out equal to k_ship_draw and the picture is honest, a
// rocket standing on a pad it is really the size of. Climb and the world
// shrinks around a ship that does not, until at 90 km the whole planet is a
// sphere under a rocket drawn forty thousand times life size. That is a cheat,
// and it is the same cheat every map icon in every space game is: the
// alternative is a correct picture of one pixel.
constexpr float k_ship_draw = 0.86f;
constexpr float k_horizon_drop = 24.0f;
constexpr float k_scale_floor_m = k_horizon_drop / k_ship_draw;

// The ground, as a patch of the real sphere. Twelve samples along the track by
// eight across it, so 77 quads: enough that a hemisphere seen from orbit reads
// as round and few enough that the ship, the pad and the ground together stay
// well inside the frame queue.
constexpr int k_patch_along = 12;
constexpr int k_patch_across = 8;

// The light. Fixed in world space and put over the launch site, so the pad is
// in full sun at the start of every flight and the far side of the planet is
// genuinely night when the ship gets there.
constexpr int32_t k_sun_angle = ps::k_turn / 4;
constexpr float k_ambient = 0.40f;

struct Rgb { uint8_t r, g, b; };

constexpr Rgb k_sky_low{116, 158, 210};
constexpr Rgb k_sky_high{30, 56, 116};
constexpr Rgb k_space{4, 5, 12};

int clamp_i(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
float clamp_f(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

int round_i(float v) {
    return static_cast<int>(v < 0.0f ? v - 0.5f : v + 0.5f);
}

Rgb mix(Rgb a, Rgb b, float t) {
    t = clamp_f(t, 0.0f, 1.0f);
    return Rgb{static_cast<uint8_t>(a.r + (b.r - a.r) * t),
               static_cast<uint8_t>(a.g + (b.g - a.g) * t),
               static_cast<uint8_t>(a.b + (b.b - a.b) * t)};
}

Rgb scale_rgb(Rgb c, float k) {
    return Rgb{static_cast<uint8_t>(clamp_i(round_i(c.r * k), 0, 255)),
               static_cast<uint8_t>(clamp_i(round_i(c.g * k), 0, 255)),
               static_cast<uint8_t>(clamp_i(round_i(c.b * k), 0, 255))};
}

float radians_of(int32_t angle_units) {
    return angle_units * (2.0f * k_pi / ps::k_turn);
}

// An in plane world direction, as a unit vector in the ship's local frame.
// `ahead` is how far the direction leads the ship's own bearing, in radians.
//
// This is the frame's handedness, written once. Everything that places
// something by a bearing (the ship's nose, the pad, the prograde marker, a
// patch of ground) goes through it or through the same pair of expressions,
// and the whole flight view was mirrored against the map because the sign of
// the first one was the other way round.
void local_dir(float ahead, float& x, float& y) {
    x = -std::sin(ahead);
    y = std::cos(ahead);
}

// The roll that turns the model's nose, which points up its own +y, onto a
// world direction leading the ship's bearing by `ahead`. draw_mesh's roll
// turns +x toward +y, so it takes (0,1) to (-sin, cos), which is local_dir
// exactly: the roll IS the lead angle.
float roll_for(float ahead) { return ahead; }

// Everything the frame needs, worked out once.
struct Frame {
    float scale;          // view units per metre
    float body_dist;      // view units from the ship down to the body centre
    int32_t bearing;      // the ship's angle round the body, angle units
    int32_t terrain0;     // the terrain right under the ship, metres
    int32_t alt_m;        // the ship's origin above that terrain
    float sun_delta;      // where the sun is, relative to the ship's bearing
    float space;          // 0 in thick air, 1 in vacuum
    float cam_y, cam_z;   // where the camera ended up, for the reach clamp
    uint8_t ref;
};

// How far from the camera anything is allowed to be. See k_project_limit: the
// engine's fixed point projection overflows past about a thousand view units,
// and at pad scale the horizon is thirteen hundred of them away.
constexpr float k_reach = 620.0f;

// Pull a point back along the ray it sits on until it is inside that reach.
//
// Along the RAY, which is the whole trick: a point moved toward the camera on
// its own sight line projects to exactly the same pixel it did before, so the
// far ground lands where it belongs and only its depth changes, and its depth
// only has to be further than everything else. Clamping the coordinates
// instead, which is the obvious thing, bends the horizon into whichever axis
// hit the limit first.
void clamp_reach(const Frame& f, float& x, float& y, float& z) {
    const float vx = x, vy = y - f.cam_y, vz = z - f.cam_z;
    const float d2 = vx * vx + vy * vy + vz * vz;
    if (d2 <= k_reach * k_reach) return;
    const float k = k_reach / std::sqrt(d2);
    x = vx * k;
    y = f.cam_y + vy * k;
    z = f.cam_z + vz * k;
}

// A point on the body's surface, in the local frame, given its offset from
// the ship in the two surface angles and its radius from the body centre.
//
// `drop` is how far BELOW the ship's radius this point's radius is, and it is
// passed in rather than derived because deriving it means subtracting two
// numbers that are both about 77,000 view units at launch and differ by ten.
// A float has seven digits: the first version formed exactly that difference
// and the ground came out as a picket fence of alternating gaps, one per
// sample, because each sample's height was quantised to whatever the seventh
// digit happened to round to.
//
// Written this way there is no cancellation anywhere. The two corrections are
// both formed from half angle sines, which are small when the angles are.
void surface_point(float alpha, float gamma, float rad, float drop,
                   float& x, float& y, float& z) {
    const float sa = std::sin(alpha), ca = std::cos(alpha);
    const float sg = std::sin(gamma), cg = std::cos(gamma);
    const float ha = std::sin(alpha * 0.5f), hg = std::sin(gamma * 0.5f);
    // 1 - cos(a)cos(g) == 2 sin^2(a/2) + cos(a) * 2 sin^2(g/2)
    const float bend = 2.0f * ha * ha + ca * 2.0f * hg * hg;
    // Negated for the same reason local_dir's x is: a sample further round
    // the body in the direction of increasing bearing lies to the LEFT.
    x = -rad * sa * cg;
    y = -drop - rad * bend;
    z = rad * sg;
}

// Anything past this in view units cannot be projected: Renderer3D carries the
// view projection in 10 bit fixed point, so a coordinate multiplied by a matrix
// entry overflows an int32 somewhere past a couple of thousand. It comes back
// as a vertex in a random place rather than as an error.
//
// It bites here and not in other games because this one draws a scene whose
// scale changes by five orders of magnitude: a moon 400 km away is a third of
// a million view units out when the world is drawn at pad scale. Nothing that
// far off is on screen anyway, so the guard costs a comparison and buys the
// difference between a moon that is not drawn and a moon drawn across the
// rocket.
// 900 rather than a round thousand because the bound is not arbitrary: the
// largest entry in the view projection is the focal length, about 1.9, which
// is 1966 in the engine's 10 bit fixed point, and 2^31 / (1024 * 1966) is
// 1066. Anything under that cannot overflow whatever the matrix holds.
constexpr float k_project_limit = 900.0f;

bool in_range(float x, float y, float z) {
    return std::fabs(x) < k_project_limit && std::fabs(y) < k_project_limit &&
           std::fabs(z) < k_project_limit;
}

bool project_local(float x, float y, float z, int& sx, int& sy) {
    if (!in_range(x, y, z)) return false;
    int depth;
    return g_renderer.project(x, y, z, sx, sy, depth);
}

void emit_tri(float ax, float ay, float az, Rgb ac,
              float bx, float by, float bz, Rgb bc,
              float cx, float cy, float cz, Rgb cc) {
    int x0, y0, z0, x1, y1, z1, x2, y2, z2;
    if (!in_range(ax, ay, az) || !in_range(bx, by, bz) ||
        !in_range(cx, cy, cz) ||
        !g_renderer.project(ax, ay, az, x0, y0, z0) ||
        !g_renderer.project(bx, by, bz, x1, y1, z1) ||
        !g_renderer.project(cx, cy, cz, x2, y2, z2)) {
        return;
    }
    pse::ScreenTriangle tri;
    tri.x0 = static_cast<int16_t>(clamp_i(x0, -20000, 20000));
    tri.y0 = static_cast<int16_t>(clamp_i(y0, -20000, 20000));
    tri.x1 = static_cast<int16_t>(clamp_i(x1, -20000, 20000));
    tri.y1 = static_cast<int16_t>(clamp_i(y1, -20000, 20000));
    tri.x2 = static_cast<int16_t>(clamp_i(x2, -20000, 20000));
    tri.y2 = static_cast<int16_t>(clamp_i(y2, -20000, 20000));
    tri.z0 = static_cast<uint16_t>(z0);
    tri.z1 = static_cast<uint16_t>(z1);
    tri.z2 = static_cast<uint16_t>(z2);
    tri.r0 = ac.r; tri.g0 = ac.g; tri.b0 = ac.b;
    tri.r1 = bc.r; tri.g1 = bc.g; tri.b1 = bc.b;
    tri.r2 = cc.r; tri.g2 = cc.g; tri.b2 = cc.b;
    tri.tex = 0;
    g_raster.draw(tri);
}

// ---- 2D primitives --------------------------------------------------------
//
// The map is circles and dots, and the flight view finishes with a plume and a
// marker ring. None of that is geometry: a filled disc through the triangle
// queue is a fan of slivers, and a scanline rasterizer pays a full per triangle
// bill for every one of them. Spans straight into the framebuffer cost a
// multiply a row.

void span(const pse::RenderTarget& t, int x0, int x1, int y, Rgb c) {
    if (y < 0 || y >= t.height) return;
    x0 = clamp_i(x0, 0, t.width - 1);
    x1 = clamp_i(x1, 0, t.width - 1);
    if (x1 < x0) return;
    pse::fill_rect(t, x0, y, x1 - x0 + 1, 1, c.r, c.g, c.b);
}

void disc(const pse::RenderTarget& t, float cx, float cy, float r, Rgb c) {
    if (r < 0.5f) {
        pse::plot_pixel(t, round_i(cx), round_i(cy), c.r, c.g, c.b);
        return;
    }
    const int y0 = clamp_i(round_i(cy - r), 0, t.height - 1);
    const int y1 = clamp_i(round_i(cy + r), 0, t.height - 1);
    for (int y = y0; y <= y1; y++) {
        const float dy = y - cy;
        const float k = r * r - dy * dy;
        if (k < 0.0f) continue;
        const float half = std::sqrt(k);
        span(t, round_i(cx - half), round_i(cx + half), y, c);
    }
}

// A circle outline. `dash` of 0 is solid; anything else draws that many of
// every 2 * dash steps, which is how an orbit reads as a path rather than as
// a solid ring competing with the bodies on it.
void circle(const pse::RenderTarget& t, float cx, float cy, float r, Rgb c,
            int dash = 0) {
    if (r < 0.5f) return;
    const int steps = clamp_i(round_i(r * 6.5f), 16, 400);
    for (int i = 0; i < steps; i++) {
        if (dash > 0 && (i % (dash * 2)) >= dash) continue;
        const float a = i * (2.0f * k_pi) / steps;
        pse::plot_pixel(t, round_i(cx + std::cos(a) * r),
                        round_i(cy + std::sin(a) * r), c.r, c.g, c.b);
    }
}

void mark(const pse::RenderTarget& t, float cx, float cy, Rgb c) {
    pse::fill_rect(t, round_i(cx) - 1, round_i(cy) - 1, 3, 3, c.r, c.g, c.b);
}

void line(const pse::RenderTarget& t, float x0, float y0, float x1, float y1,
          Rgb c) {
    const float dx = x1 - x0, dy = y1 - y0;
    const float len = std::fabs(dx) > std::fabs(dy) ? std::fabs(dx)
                                                    : std::fabs(dy);
    const int steps = clamp_i(round_i(len), 1, 200);
    for (int i = 0; i <= steps; i++) {
        pse::plot_pixel(t, round_i(x0 + dx * i / steps),
                        round_i(y0 + dy * i / steps), c.r, c.g, c.b);
    }
}

// ---- the sky --------------------------------------------------------------

// How far out of the air the ship is, 0 on the ground and 1 in space. Airless
// bodies are always 1, which is what makes a moon's sky black without a
// special case anywhere else.
float sky_fraction(const World& w) {
    const ps::Body& body = ps::k_bodies[w.ref_body];
    if (body.atmo_m == 0) return 1.0f;
    return clamp_f(ps::altitude_m(w) / (body.atmo_m * 0.85f), 0.0f, 1.0f);
}

// Stars, from a hash of the index rather than a table, so the field is the
// same every frame and does not swim.
//
// Drawn AFTER the world, because the gradient clear at the start of the frame
// would wipe anything drawn before it, and hidden by the depth buffer rather
// than by any geometric test: the planet is a real surface at a real depth
// now, and asking whether a pixel is inside its silhouette is a question the
// depth buffer has already answered. 250 is behind everything the renderer
// draws and in front of a cleared pixel.
void draw_stars(const pse::RenderTarget& t, float fade) {
    if (fade <= 0.02f) return;
    uint32_t seed = 0x5CE1u;
    for (int i = 0; i < 52; i++) {
        seed = seed * 1103515245u + 12345u;
        const int x = static_cast<int>((seed >> 16) % t.width);
        seed = seed * 1103515245u + 12345u;
        const int y = static_cast<int>((seed >> 16) % t.height);
        seed = seed * 1103515245u + 12345u;
        const int v = 130 + static_cast<int>((seed >> 16) % 110);
        if (!g_raster.test_and_set_depth(x, y, 250)) continue;
        const int lit = clamp_i(round_i(v * fade), 0, 255);
        pse::plot_pixel(t, x, y, static_cast<uint8_t>(lit),
                        static_cast<uint8_t>(lit),
                        static_cast<uint8_t>(clamp_i(lit + 14, 0, 255)));
    }
}

// ---- the ground -----------------------------------------------------------

// The surface as a patch of the body's real sphere, sampled around the point
// under the ship in both directions.
//
// Near the ground the patch is a few hundred metres of hillside; from orbit it
// is most of a hemisphere, and nothing switches between the two. The height
// comes from ps::terrain_at, whose value in the ship's own plane is exactly
// the ps::terrain_m the sim lands on, so the hill in the picture is the hill
// the gear touches.
void draw_ground(const Frame& f) {
    const ps::Body& body = ps::k_bodies[f.ref];
    const float rv = body.radius_m * f.scale;
    if (rv < 0.4f) return;

    // Out to the limb, and no further. The limb IS the horizon: it is the
    // angle at which the surface turns away from the ship, so a patch that
    // reaches it reaches exactly as far as there is ground to see, at six
    // metres up and at ninety kilometres alike. An earlier version capped the
    // span at a fixed arc instead, which at low altitude was a hundredth of
    // the way to the horizon and drew the ground as a flat slab ending in a
    // straight line halfway up the screen.
    const float span = clamp_f(
        std::acos(clamp_f(
            body.radius_m / static_cast<float>(body.radius_m + f.alt_m + 1),
            -1.0f, 1.0f)) * 1.05f,
        0.0004f, k_pi * 0.49f);

    // How far the patch may reach back TOWARD the camera. The engine's
    // projection has no near plane clipping: it rejects a triangle outright if
    // any corner is behind the camera, so a quad straddling that plane is not
    // clipped, it is a hole. On the pad the near edge of a full span patch is
    // over a kilometre behind the camera, and the hole it left was a wedge of
    // sky across the bottom of the screen with the rocket standing in it.
    //
    // From orbit the whole hemisphere is in front and this comes out at the
    // full span, so there is no case to special case.
    const float behind = clamp_f(38.0f / rv, 0.0f, 1.0f);
    const float near_span = span < std::asin(behind) ? span : std::asin(behind);

    // How much the ground is checkered. Near the surface a flat colour is a
    // flat colour whatever the geometry under it is doing: the lambert varies
    // by nothing across a hundred metres of a ninety kilometre sphere, so the
    // picture came out as one green rectangle in a genuinely three dimensional
    // scene. Alternating cells give the eye the receding grid it reads
    // perspective from. It fades out with altitude, because from orbit a
    // chequerboard is a chequerboard and not a planet.
    const float check = clamp_f(1.0f - f.alt_m / 6000.0f, 0.0f, 1.0f) * 0.13f;

    struct Vtx { float x, y, z; Rgb c; };
    Vtx prev[k_patch_along + 1];
    Vtx cur[k_patch_along + 1];

    // Samples are spread quadratically rather than evenly. Perspective
    // compresses everything near the horizon into a few rows, so an even
    // spread spends half its samples where they cannot be seen and leaves the
    // foreground, which fills most of the screen, as four enormous cells.
    const auto place = [span](int i, int n) {
        const float t = 2.0f * i / n - 1.0f;
        return span * t * std::fabs(t);
    };

    for (int j = 0; j <= k_patch_across; j++) {
        // Asymmetric across the track: as far forward as there is ground, and
        // only as far back as the camera can see.
        const float t = place(j, k_patch_across) / (span > 0.0f ? span : 1.0f);
        const float gamma = t < 0.0f ? t * near_span : t * span;
        for (int i = 0; i <= k_patch_along; i++) {
            const float alpha = place(i, k_patch_along);
            const int32_t along = f.bearing + static_cast<int32_t>(
                std::lround(alpha * ps::k_turn / (2.0f * k_pi)));
            const int32_t cross = static_cast<int32_t>(
                std::lround(gamma * ps::k_turn / (2.0f * k_pi)));
            const int32_t height = ps::terrain_at(f.ref, along, cross);
            const float rad = (body.radius_m + height) * f.scale;
            const float drop = (f.alt_m + f.terrain0 - height) * f.scale;
            surface_point(alpha, gamma, rad, drop,
                          cur[i].x, cur[i].y, cur[i].z);
            clamp_reach(f, cur[i].x, cur[i].y, cur[i].z);

            // Lambert against a sun fixed in world space. On a sphere that is
            // a terminator, and the terminator is the whole reason the planet
            // reads as a ball from orbit rather than as a coloured circle.
            const float lambert = std::cos(gamma) *
                                  std::cos(alpha - f.sun_delta);
            const float shade =
                k_ambient + (1.0f - k_ambient) * clamp_f(lambert, 0.0f, 1.0f) +
                0.22f * clamp_f(static_cast<float>(height) /
                                (body.relief_m + 1), -1.0f, 1.0f) +
                (((i + j) & 1) ? check : -check);
            cur[i].c = scale_rgb(Rgb{body.r, body.g, body.b}, shade);
        }
        if (j > 0) {
            for (int i = 0; i < k_patch_along; i++) {
                const Vtx& a = prev[i];
                const Vtx& b = prev[i + 1];
                const Vtx& c = cur[i + 1];
                const Vtx& d = cur[i];
                // Both windings. Which way round a quad comes out depends on
                // which side of the ship it is and how far over the limb, and
                // a backface culled quad is a hole in the ground. The culled
                // copy never reaches the queue, so this costs setup and not
                // the budget that matters.
                emit_tri(a.x, a.y, a.z, a.c, b.x, b.y, b.z, b.c,
                         c.x, c.y, c.z, c.c);
                emit_tri(a.x, a.y, a.z, a.c, c.x, c.y, c.z, c.c,
                         d.x, d.y, d.z, d.c);
                emit_tri(c.x, c.y, c.z, c.c, b.x, b.y, b.z, b.c,
                         a.x, a.y, a.z, a.c);
                emit_tri(d.x, d.y, d.z, d.c, c.x, c.y, c.z, c.c,
                         a.x, a.y, a.z, a.c);
            }
        }
        for (int i = 0; i <= k_patch_along; i++) prev[i] = cur[i];
    }
}

// ---- the ship -------------------------------------------------------------

// Roll takes the model's nose, which points up its own +y, to wherever the sim
// says the ship is pointing. In the local frame "up" is the way the ship would
// fall, so the roll is the attitude measured against the ship's own bearing,
// and at launch it is zero whichever quarter of the planet the pad is on.
float ship_roll(const World& w, const Frame& f) {
    return roll_for(radians_of(w.angle / ps::k_fp16 - f.bearing));
}

void draw_ship(const World& w, const Frame& f) {
    const float roll = ship_roll(w, f);
    g_stats.booster_drawn = w.stage == 0;
    g_stats.legs_drawn = w.stage > 0;
    if (w.stage == 0) {
        g_renderer.draw_mesh(models::picospace::booster, 0.0f, 0.0f, 0.0f,
                             0.0f, k_ship_draw, 255, 255, 255, 0.0f, 0, roll);
    } else {
        g_renderer.draw_mesh(models::picospace::legs, 0.0f, 0.0f, 0.0f,
                             0.0f, k_ship_draw, 255, 255, 255, 0.0f, 0, roll);
    }
    g_renderer.draw_mesh(models::picospace::lander, 0.0f, 0.0f, 0.0f,
                         0.0f, k_ship_draw, 255, 255, 255, 0.0f, 0, roll);
}

// A point in the sim's plane, as an offset from the ship, in the local frame.
void plane_offset(const World& w, const Frame& f, float wx, float wy,
                  float& x, float& y) {
    const float dx = wx - static_cast<float>(w.x >> 16);
    const float dy = wy - static_cast<float>(w.y >> 16);
    const float b = radians_of(f.bearing);
    const float ux = std::cos(b), uy = std::sin(b);
    // +y is the local up, +x is along the track.
    y = (dx * ux + dy * uy) * f.scale;
    x = (dx * uy - dy * ux) * f.scale;
}

// The spent booster, drawn where it really is and tumbling the way the sim is
// tumbling it. It is scenery with a trajectory: nothing collides with it and
// nothing reads it but this.
void draw_debris(const World& w, const Frame& f) {
    if (!w.debris) return;
    float x, y;
    plane_offset(w, f, static_cast<float>(w.dx >> 16),
                 static_cast<float>(w.dy >> 16), x, y);
    if (std::fabs(x) > 90.0f || std::fabs(y) > 90.0f) return;
    g_renderer.draw_mesh(models::picospace::booster, x, y, 0.0f, 0.0f,
                         k_ship_draw * 0.8f, 200, 200, 210, 0.0f, 0,
                         roll_for(radians_of(w.debris_angle -
                                             f.bearing)));
}

// The pad, where it really stands. Drawn only while the world is near enough
// to life size for it to be a building rather than a dot: from orbit it is ten
// metres of concrete on a 90 km planet, and asking the rasterizer which sub
// pixel that lands on costs 68 triangles for nothing.
void draw_pad(const Frame& f) {
    g_stats.pad_drawn = false;
    if (f.ref != ps::kPicopiter || f.scale < k_ship_draw * 0.04f) return;
    const float alpha = radians_of(ps::k_turn / 4 - f.bearing);
    if (std::fabs(alpha) > 0.35f) return;

    const ps::Body& body = ps::k_bodies[ps::kPicopiter];
    const float rad = body.radius_m * f.scale;
    const float drop = static_cast<float>(f.alt_m + f.terrain0) * f.scale;
    float x, y, z;
    surface_point(alpha, 0.0f, rad, drop, x, y, z);
    if (std::fabs(x) > 130.0f || std::fabs(y) > 180.0f) return;

    g_stats.pad_drawn = true;
    g_renderer.draw_mesh(models::picospace::pad, x, y, z, 0.0f, f.scale,
                         255, 255, 255, 0.0f, 0, roll_for(alpha));
}

// ---- the plume ------------------------------------------------------------
//
// Plotted straight into the framebuffer rather than queued as geometry. A
// flame is a soft blob and the queue is for surfaces; forty sliver triangles
// to draw one is the trade the tower in Tom Lander lost.
void draw_plume(const World& w, const Frame& f, const pse::RenderTarget& t,
                uint32_t time_ms) {
    if (w.throttle == 0 || w.fuel_kg <= 0) return;
    const float power = w.throttle / 255.0f;
    // Down the ship's own axis, in the local frame, from whichever engine is
    // lit. The two ends are PROJECTED rather than laid out in screen space, so
    // the flame is foreshortened exactly as the rocket it comes out of is.
    const float a = ship_roll(w, f) + k_pi * 0.5f;
    const float nx = std::cos(a), ny = std::sin(a);
    const float bell = (w.stage == 0 ? 11.0f : 0.95f) * k_ship_draw;
    const float length = (w.stage == 0 ? 15.0f : 6.0f) * k_ship_draw * power;

    int hx, hy, tx, ty;
    if (!project_local(-nx * bell, -ny * bell, 0.0f, hx, hy)) return;
    if (!project_local(-nx * (bell + length), -ny * (bell + length), 0.0f,
                       tx, ty)) {
        return;
    }
    const float dx = static_cast<float>(tx - hx), dy = static_cast<float>(ty - hy);
    const float run = std::sqrt(dx * dx + dy * dy);
    if (run < 0.5f) return;
    const float px = -dy / run, py = dx / run;      // across the flame

    // A flicker that is a function of the clock rather than of a random
    // number, so a preview frame and a device frame at the same millisecond
    // are the same picture and the harness can check one.
    const int phase = static_cast<int>((time_ms / 40) & 3);
    const int steps = clamp_i(round_i(run), 2, 48);
    const float wide = (w.stage == 0 ? 3.4f : 2.2f) * power;
    for (int i = 0; i <= steps; i++) {
        const float u = static_cast<float>(i) / steps *
                        (0.86f + 0.14f * ((phase + i) & 1));
        const Rgb c = u < 0.28f ? Rgb{255, 244, 214}
                                : (u < 0.62f ? Rgb{255, 182, 60}
                                             : Rgb{236, 94, 44});
        const int half = clamp_i(round_i(wide * (1.0f - 0.55f * u)), 0, 6);
        const float cx = hx + dx * u, cy = hy + dy * u;
        for (int o = -half; o <= half; o++) {
            pse::plot_pixel(t, round_i(cx + px * o), round_i(cy + py * o),
                            c.r, c.g, c.b);
        }
    }
}

// ---- the marker ring ------------------------------------------------------
//
// A ring round the ship in the plane it actually flies in, with a filled dot
// for where it is going and a hollow one for the other way. It is a circle in
// the world and therefore an ellipse on screen, which is what tells a player
// at a glance which way the plane is tilted.
//
// It sits ON the ship rather than in a corner because turning the nose onto one
// of those two dots is the whole of flying this, and a marker a player has to
// look away from is a marker they fly without.
void draw_ring(const World& w, const Frame& f, const pse::RenderTarget& t) {
    constexpr float k_ring = 10.5f;
    g_stats.prograde_x = -1;
    g_stats.prograde_y = -1;

    int px = 0, py = 0;
    bool have = false;
    for (int i = 0; i <= 28; i++) {
        const float a = i * (2.0f * k_pi / 28);
        int sx, sy;
        if (!project_local(std::cos(a) * k_ring, std::sin(a) * k_ring, 0.0f,
                           sx, sy)) {
            have = false;
            continue;
        }
        if (have && (i & 1)) {
            line(t, static_cast<float>(px), static_cast<float>(py),
                 static_cast<float>(sx), static_cast<float>(sy),
                 Rgb{62, 76, 108});
        }
        px = sx; py = sy; have = true;
    }

    int32_t ux, uy;
    ps::prograde(w, ux, uy);
    if (ux != 0 || uy != 0) {
        // The velocity is in the sim's plane; the ring is too. Turn it into
        // the local frame the same way every other in plane direction is.
        const float dir = radians_of(ps::prograde_angle(w) - f.bearing);
        float ux2, uy2;
        local_dir(dir, ux2, uy2);
        const float lx = ux2 * k_ring, ly = uy2 * k_ring;
        int sx, sy;
        if (project_local(lx, ly, 0.0f, sx, sy)) {
            mark(t, static_cast<float>(sx), static_cast<float>(sy),
                 Rgb{60, 226, 122});
            g_stats.prograde_x = static_cast<int16_t>(sx);
            g_stats.prograde_y = static_cast<int16_t>(sy);
        }
        // Retrograde, hollow, so the two are never confused at a glance even
        // when the ring is only twenty pixels across.
        if (project_local(-lx, -ly, 0.0f, sx, sy)) {
            const Rgb c{60, 226, 122};
            pse::fill_rect(t, sx - 1, sy - 1, 3, 1, c.r, c.g, c.b);
            pse::fill_rect(t, sx - 1, sy + 1, 3, 1, c.r, c.g, c.b);
            pse::plot_pixel(t, sx - 1, sy, c.r, c.g, c.b);
            pse::plot_pixel(t, sx + 1, sy, c.r, c.g, c.b);
        }
    }

    // Where the nose is, just outside the ring.
    const float nose = ship_roll(w, f) + k_pi * 0.5f;
    int nx, ny;
    if (project_local(std::cos(nose) * (k_ring + 3.4f),
                      std::sin(nose) * (k_ring + 3.4f), 0.0f, nx, ny)) {
        pse::plot_pixel(t, nx, ny, 255, 210, 74);
    }
}

// The other bodies, as discs where they really are. Two dozen pixels of span
// fill, and without them a transfer is flown at a moon that is never once
// visible out of the window.
void draw_other_bodies(const World& w, const Frame& f,
                       const pse::RenderTarget& t) {
    for (uint8_t b = 0; b < ps::kBodyCount; b++) {
        if (b == f.ref) continue;
        int32_t bx, by;
        ps::body_position(w, b, bx, by);
        float x, y;
        plane_offset(w, f, static_cast<float>(bx), static_cast<float>(by),
                     x, y);
        int sx, sy;
        if (!project_local(x, y, 0.0f, sx, sy)) continue;
        // The radius on screen, from two projected points rather than a
        // scale factor: the camera is tilted and a body off to one side is
        // further away than one straight ahead.
        int ex, ey;
        const float rv = ps::k_bodies[b].radius_m * f.scale;
        if (!project_local(x + rv, y, 0.0f, ex, ey)) continue;
        const float r = std::fabs(static_cast<float>(ex - sx));
        if (sx < -r - 2 || sx > t.width + r + 2) continue;
        if (sy < -r - 2 || sy > t.height + r + 2) continue;
        const ps::Body& body = ps::k_bodies[b];
        disc(t, static_cast<float>(sx), static_cast<float>(sy),
             r < 1.0f ? 1.0f : r, scale_rgb(Rgb{body.r, body.g, body.b}, 0.85f));
    }
}

// ---- the flight view ------------------------------------------------------

void draw_flight(const World& w, const pse::RenderTarget& target,
                 uint32_t time_ms) {
    Frame f{};
    f.ref = w.ref_body;
    f.bearing = ps::bearing_from(w, f.ref);
    f.terrain0 = ps::terrain_at(f.ref, f.bearing, 0);
    f.alt_m = ps::altitude_m(w);
    f.space = sky_fraction(w);
    f.scale = k_horizon_drop /
              (f.alt_m > k_scale_floor_m ? f.alt_m : k_scale_floor_m);
    f.sun_delta = radians_of(k_sun_angle - f.bearing);
    g_stats.world_scale_fp16 = static_cast<int32_t>(f.scale * 65536.0f);

    int32_t bx, by;
    ps::body_position(w, f.ref, bx, by);
    const float rx = static_cast<float>(w.x >> 16) - bx;
    const float ry = static_cast<float>(w.y >> 16) - by;
    f.body_dist = std::sqrt(rx * rx + ry * ry) * f.scale;

    g_queue.reset();
    g_raster.begin_frame_collect(target, g_queue);
    g_renderer.set_fov(k_fov);
    g_renderer.set_depth_range(6.0f, 320.0f);
    // Behind and above the ship, looking at it. The lift falls away with
    // altitude: a three quarter view over the terrain is what a launch and a
    // landing want, and a near edge on one is what puts a ship in orbit
    // against the limb instead of flat on top of the planet.
    const float lift = k_cam_high_low +
                       (k_cam_high_space - k_cam_high_low) * f.space;
    f.cam_y = lift;
    f.cam_z = -k_cam_dist;
    g_renderer.set_orbit_camera(0.0f, 0.0f, 0.0f, 0.0f, k_cam_dist, lift,
                                0.0f);

    draw_ship(w, f);
    draw_debris(w, f);
    draw_pad(f);
    draw_ground(f);

    const Rgb top = mix(k_sky_high, k_space, f.space);
    const Rgb low = mix(k_sky_low, k_space, f.space);
    pse::run_split(g_raster, g_queue,
                   pse::SkyGradient{top.r, top.g, top.b, low.r, low.g, low.b});
    g_raster.end_collect();

    g_stats.queued = g_queue.count;
    g_stats.dropped = g_queue.dropped;

    draw_stars(target, f.space);
    draw_other_bodies(w, f, target);
    draw_plume(w, f, target, time_ms);
    draw_ring(w, f, target);

    int sx, sy;
    if (project_local(0.0f, 0.0f, 0.0f, sx, sy)) {
        g_stats.ship_x = static_cast<int16_t>(sx);
        g_stats.ship_y = static_cast<int16_t>(sy);
    }
    // Where the ground directly under the ship ended up. The whole framing
    // hangs off this: nothing else downstream can see whether the world came
    // out at the right scale, because a wrong scale is still a picture.
    g_stats.horizon_y = -1;
    {
        const float rad = ps::k_bodies[f.ref].radius_m * f.scale;
        float gx, gy, gz;
        surface_point(0.0f, 0.0f, rad,
                      static_cast<float>(f.alt_m + f.terrain0) * f.scale,
                      gx, gy, gz);
        int hx, hy;
        if (project_local(gx, gy, gz, hx, hy) && hy >= 0 &&
            hy < target.height) {
            g_stats.horizon_y = static_cast<int16_t>(hy);
        }
    }
}

// ---- the map --------------------------------------------------------------

// The trajectory, from the conic the sim already worked out: one polar
// equation covers a circle, an ellipse and an escape alike, so there is no
// second code path for the half of a mission that is flown on a hyperbola.
void draw_conic(const pse::RenderTarget& t, const ps::Elements& el,
                float cx, float cy, float px_per_m, Rgb colour) {
    if (el.semi_latus_m <= 0) return;
    const float p = static_cast<float>(el.semi_latus_m);
    const float e = el.ecc_fp16 / 65536.0f;
    const float peri = radians_of(el.peri_angle);

    // A hyperbola runs off to infinity at the asymptote. Stop short of it, or
    // the last sample is a line to somewhere off the edge of the world.
    float limit = k_pi;
    if (e > 1.0f) limit = std::acos(clamp_f(-1.0f / e, -1.0f, 1.0f)) * 0.94f;

    bool have = false;
    float lx = 0.0f, ly = 0.0f;
    constexpr int k_samples = 72;
    for (int i = 0; i <= k_samples; i++) {
        const float nu = -limit + (2.0f * limit * i) / k_samples;
        const float denom = 1.0f + e * std::cos(nu);
        if (denom < 0.02f) { have = false; continue; }
        const float r = p / denom;
        const float a = peri + nu;
        const float x = cx + std::cos(a) * r * px_per_m;
        const float y = cy - std::sin(a) * r * px_per_m;
        if (x < -400.0f || x > 400.0f || y < -400.0f || y > 400.0f) {
            have = false;
            continue;
        }
        // Filled between samples, so a wide orbit is a line and not a dotted
        // arc: 72 samples round a 100 pixel ellipse leaves four pixel gaps.
        if (have) line(t, lx, ly, x, y, colour);
        lx = x; ly = y; have = true;
    }
}

void draw_map(const World& w, const pse::RenderTarget& target) {
    const uint8_t ref = w.ref_body;
    const ps::Body& body = ps::k_bodies[ref];
    const float cx = (target.width - 1) * 0.5f;
    const float cy = (target.height - 1) * 0.5f;
    const float room = (target.height * 0.5f) - 8.0f;

    // What has to fit: the ship, its orbit, and the moon it is aimed at.
    int32_t bx, by;
    ps::body_position(w, ref, bx, by);
    const float rx = static_cast<float>(w.x >> 16) - bx;
    const float ry = static_cast<float>(w.y >> 16) - by;
    float span = std::sqrt(rx * rx + ry * ry);
    if (span < body.radius_m * 1.6f) span = body.radius_m * 1.6f;
    const ps::Elements el = ps::elements(w);
    if (el.closed && el.apoapsis_m > span) {
        span = static_cast<float>(el.apoapsis_m);
    }
    const uint8_t goal = ps::target_body(w);
    if (ref == ps::kPicopiter && goal != ps::kPicopiter) {
        const float orbit = static_cast<float>(ps::k_bodies[goal].orbit_m);
        if (orbit > span) span = orbit;
    }
    const float px_per_m = room / (span * 1.12f);

    pse::fill_rect(target, 0, 0, target.width, target.height, 4, 5, 12);
    uint32_t seed = 0x51A2u;
    for (int i = 0; i < 40; i++) {
        seed = seed * 1103515245u + 12345u;
        const int x = static_cast<int>((seed >> 16) % target.width);
        seed = seed * 1103515245u + 12345u;
        const int y = static_cast<int>((seed >> 16) % target.height);
        const float dx = x - cx, dy = y - cy;
        const float r = body.radius_m * px_per_m;
        if (dx * dx + dy * dy < r * r) continue;
        pse::plot_pixel(target, x, y, 150, 150, 168);
    }

    // Moon orbits and moons, when the map is about Picopiter. Inside a moon's
    // own frame none of that is where the ship is looking.
    if (ref == ps::kPicopiter) {
        for (uint8_t b = 1; b < ps::kBodyCount; b++) {
            const ps::Body& moon = ps::k_bodies[b];
            const bool is_goal = b == goal;
            circle(target, cx, cy, moon.orbit_m * px_per_m,
                   is_goal ? Rgb{58, 88, 132} : Rgb{34, 44, 68});
            int32_t mx, my;
            ps::body_position(w, b, mx, my);
            const float sx = cx + mx * px_per_m;
            const float sy = cy - my * px_per_m;
            // The capture ring, which is where the readouts change hands, so
            // aiming at it is aiming at the arrival.
            if (is_goal) {
                circle(target, sx, sy, moon.ref_m * px_per_m,
                       Rgb{58, 110, 82}, 2);
            }
            const float r = moon.radius_m * px_per_m;
            disc(target, sx, sy, r < 1.6f ? 1.6f : r,
                 Rgb{moon.r, moon.g, moon.b});
        }
    }

    disc(target, cx, cy, body.radius_m * px_per_m, Rgb{body.r, body.g, body.b});
    if (body.atmo_m > 0) {
        circle(target, cx, cy, (body.radius_m + body.atmo_m) * px_per_m,
               Rgb{74, 118, 176});
    }

    draw_conic(target, el, cx, cy, px_per_m, Rgb{255, 163, 0});

    // The two ends of the orbit, named by colour: warm for the top of the arc,
    // cold for the bottom, the same two colours the HUD prints them in.
    if (el.closed) {
        const float peri = radians_of(el.peri_angle);
        mark(target, cx + std::cos(peri) * el.periapsis_m * px_per_m,
             cy - std::sin(peri) * el.periapsis_m * px_per_m,
             Rgb{127, 210, 255});
        mark(target, cx - std::cos(peri) * el.apoapsis_m * px_per_m,
             cy + std::sin(peri) * el.apoapsis_m * px_per_m,
             Rgb{255, 210, 74});
    }

    // The ship, with a hole punched in it so it reads as a ship and not as
    // another apsis marker.
    const float sx = cx + rx * px_per_m, sy = cy - ry * px_per_m;
    mark(target, sx, sy, Rgb{60, 226, 122});
    pse::plot_pixel(target, round_i(sx), round_i(sy), 4, 5, 12);

    g_stats.ship_x = static_cast<int16_t>(round_i(sx));
    g_stats.ship_y = static_cast<int16_t>(round_i(sy));
    g_stats.horizon_y = -1;
    g_stats.world_scale_fp16 = static_cast<int32_t>(px_per_m * 65536.0f);
}

}  // namespace

FrameStats last_frame_stats() { return g_stats; }

void render_scene(const World& world, const pse::RenderTarget& target,
                  View view, uint32_t time_ms) {
    g_stats = FrameStats{};
    if (view == View::Map) {
        draw_map(world, target);
    } else {
        draw_flight(world, target, time_ms);
    }
}

}  // namespace psr
