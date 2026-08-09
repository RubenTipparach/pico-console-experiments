#include "render.hpp"

#include <cmath>

#include "pse/parallel.hpp"
#include "pse/raster.hpp"
#include "pse/renderer3d.hpp"
#include "pse/shared_render.hpp"
#include "pse/text.hpp"
#include "fixed.hpp"
#include "twinflare/cockpit_anvil.hpp"
#include "twinflare/cockpit_fang.hpp"
#include "twinflare/cockpit_needle.hpp"
#include "twinflare/cockpit_nightjar.hpp"
#include "twinflare/cockpit_scarab.hpp"
#include "twinflare/cockpit_wisp.hpp"
#include "twinflare/engine_anvil.hpp"
#include "twinflare/engine_fang.hpp"
#include "twinflare/engine_needle.hpp"
#include "twinflare/engine_nightjar.hpp"
#include "twinflare/engine_scarab.hpp"
#include "twinflare/engine_wisp.hpp"
#include "twinflare/rock.hpp"

namespace twinflare {
namespace {

constexpr int k_w = 120;
constexpr int k_h = 120;

// The camera. Chase, low and close, because that is what sells speed on a
// small screen: a high camera turns a race into a diagram.
//
// The depth range is not a taste decision and it is not the pair of numbers a
// reader expects. Renderer3D::project rejects a vertex once NDC z reaches 0,
// which is the HARMONIC MEAN of the near and far planes rather than the near
// plane: set_depth_range(0.25, 400) really clips at 0.50, and (2, 170) really
// clips at 3.95. Since the engine has no clipping at all, a triangle with one
// corner past that line is dropped WHOLE, so this number decides where the
// road is allowed to start, and the road has to start behind the pod.
//
// 170 out for the far plane because the depth buffer is one byte on a
// hyperbolic curve: a bracket much wider than the scene collapses everything
// past a quarter of the range into one depth step, and then a rock ties with
// the ground it stands on and one of them vanishes.
constexpr float k_near = 2.0f;
constexpr float k_far = 170.0f;
constexpr float k_fov = 74.0f;
constexpr float k_cam_dist = 8.4f;
constexpr float k_cam_high = 2.4f;
constexpr float k_cam_lift = 1.2f;
constexpr float k_boost_pull = 4.2f;

// How much of the pod's bank the horizon takes. A third: enough that a corner
// visibly rolls the world, not so much that the player loses which way is up.
constexpr float k_roll_share = 0.30f;

// Where the parts of a pod sit in its own frame. The engines lead, the cockpit
// trails, and that gap is the podracer.
constexpr float k_engine_x = 2.35f;
constexpr float k_engine_y = 0.45f;
constexpr float k_engine_z = 3.30f;
constexpr float k_pod_z = -1.55f;

// How far the engines turn ahead of the pod, per radian a second of yaw rate,
// and how much of the cockpit's swing shows as it pointing away from them.
// These two are the whole visible difference between a podracer and a plane.
// Where a cable meets each part, in that part's own frame.
//
// On the REAR top of the cockpit, not the front. Being inside the mesh is not
// enough: the chase camera sits behind and above, so an anchor on the front
// top face is on a face the player never sees, and the cable lands above the
// cockpit's visible silhouette and reads as floating even though it is welded
// on. Anchoring just behind the middle puts it on the edge the camera can
// actually see.
constexpr float k_cable_pod_x = 0.40f;
constexpr float k_cable_pod_y = 0.30f;
constexpr float k_cable_pod_z = -0.15f;
constexpr float k_cable_eng_in = 0.22f;   // inboard, toward the cockpit
constexpr float k_cable_eng_y = -0.06f;
constexpr float k_cable_eng_z = -0.85f;   // aft of the engine's middle

// Half extents of each mesh, for measuring whether an anchor is really on the
// part. A sphere was too generous to catch the front face mistake above: the
// cockpit's half diagonal is 1.44, so a radius of 1.2 passed anchors that were
// nowhere the camera could see them.
// The TIGHTEST parts on the roster, not an average and not the ones the cables
// were first drawn against. Six racers fly six different engines now, and
// NEEDLE's tapers to a third of a unit across where the cable meets it while
// ANVIL's is two and a half times that: an anchor checked against a box the
// size of ANVIL is an anchor hanging in the air on two of the six. Measured off
// the generated meshes, inradius rather than circumradius, because a hexagon is
// narrowest across its flats.
constexpr float k_cockpit_half[3] = {0.42f, 0.30f, 0.95f};
constexpr float k_engine_half[3] = {0.30f, 0.22f, 1.40f};

constexpr float k_engine_lead = 0.34f;
constexpr float k_cockpit_trail = 0.80f;
// How far the cockpit is thrown sideways per radian of swing. It was 3.4,
// which at the swing limit put the cockpit two units out: past the inboard
// face of an engine, so the pod looked like it had come apart rather than
// like it was cornering hard.
constexpr float k_swing_throw = 2.1f;

// Segments in the binder arc. Six and not four, because a jittered strand is
// only as ragged as it has joints, and four reads as a bent stick.
constexpr int k_arc_steps = 6;

constexpr float k_brad = 6.2831853f / 65536.0f;

float to_rad(int32_t brads) { return brads * k_brad; }
float to_world(int32_t fixed) { return fixed * (1.0f / 65536.0f); }

struct Camera {
    float x, y, z;
    float yaw, pitch, roll;
};

// Eased rather than welded to the pod. Welded, a twitchy pod makes an
// unreadable frame; eased, the pod visibly slides across the screen through a
// corner, which is the drift made visible and most of why this reads as a
// podracer rather than as a plane with two engines drawn on it.
struct CameraState {
    float yaw = 0.0f, pitch = -0.10f, roll = 0.0f;
    bool primed = false;
};
CameraState g_cam;

float wrap_rad(float a) {
    while (a > 3.14159265f) a -= 6.2831853f;
    while (a < -3.14159265f) a += 6.2831853f;
    return a;
}

Camera follow(const Pod& pod, float dt) {
    const float target_yaw = to_rad(pod.yaw);
    if (!g_cam.primed) {
        g_cam.yaw = target_yaw;
        g_cam.primed = true;
    }
    g_cam.yaw += wrap_rad(target_yaw - g_cam.yaw) * (dt * 5.0f > 1.0f ? 1.0f : dt * 5.0f);
    const float want_roll = to_rad(pod.roll) * k_roll_share;
    g_cam.roll += (want_roll - g_cam.roll) * (dt * 4.0f > 1.0f ? 1.0f : dt * 4.0f);
    const float clear = to_world(pod.clearance);
    const float want_pitch = -0.10f + to_rad(pod.pitch) * 0.30f + (clear > 6.0f ? -0.10f : 0.0f);
    g_cam.pitch += (want_pitch - g_cam.pitch) * (dt * 3.4f > 1.0f ? 1.0f : dt * 3.4f);

    const float speed = to_world(pod_speed(pod)) * k_tick_hz;
    float pull = k_cam_dist + (pod.boost_ticks > 0 ? k_boost_pull : 0.0f)
               + (speed * 0.045f > 6.0f ? 6.0f : speed * 0.045f);
    const float cp = std::cos(g_cam.pitch), sp = std::sin(g_cam.pitch);
    Camera c;
    c.x = to_world(pod.x) - std::sin(g_cam.yaw) * cp * pull;
    c.y = to_world(pod.y) + k_cam_high - sp * pull;
    c.z = to_world(pod.z) - std::cos(g_cam.yaw) * cp * pull;
    // Under a roof, stay under it. The camera rides two and a half units above
    // the pod and further on a nose down attitude, which is over a tunnel's
    // seven units of headroom whenever the sim has pressed the pod against the
    // ceiling. Outside the tunnel that is a camera looking down at the roof of
    // the thing it is supposed to be inside.
    if (pod.roofed) {
        const float roof = to_world(pod.y - pod.clearance + k_tunnel_height);
        if (c.y > roof - 0.5f) c.y = roof - 0.5f;
    }
    c.yaw = g_cam.yaw;
    c.pitch = g_cam.pitch;
    c.roll = g_cam.roll;
    return c;
}

// Roll is why this is a BASIS and not a yaw and a pitch: Renderer3D's angle
// camera pins `right` to the world horizontal plane and so cannot roll at all.
pse::Basis camera_basis(const Camera& c) {
    const float cy = std::cos(c.yaw), sy = std::sin(c.yaw);
    const float cp = std::cos(c.pitch), sp = std::sin(c.pitch);
    const float cr = std::cos(c.roll), sr = std::sin(c.roll);
    const float f[3] = {sy * cp, sp, cy * cp};
    const float r0[3] = {cy, 0.0f, -sy};
    const float u0[3] = {f[1] * r0[2] - f[2] * r0[1],
                         f[2] * r0[0] - f[0] * r0[2],
                         f[0] * r0[1] - f[1] * r0[0]};
    pse::Basis b{};
    for (int i = 0; i < 3; ++i) {
        b.m[i * 3 + 0] = r0[i] * cr + u0[i] * sr;
        b.m[i * 3 + 1] = -r0[i] * sr + u0[i] * cr;
        b.m[i * 3 + 2] = f[i];
    }
    return b;
}

// ---- colour ----------------------------------------------------------------

struct Rgb { uint8_t r, g, b; };

Rgb shade(const uint8_t base[3], float nx, float ny, float nz) {
    // One directional light on a floor of ambient, matching what draw_mesh
    // does with a baked normal.
    //
    // The floor is 0.55 and not the 0.46 it started at. At four bits a channel
    // an unlit face at 0.46 of a pale colour lands in the same few steps as an
    // unlit face of a dark one, so every wall on every track came out the same
    // charcoal and the ice canyon read as a hole rather than as ice.
    float d = nx * 0.36f + ny * 0.86f + nz * -0.36f;
    d = 0.55f + 0.45f * (d > 0.0f ? d : 0.0f);
    const auto ch = [d](uint8_t v) {
        const float f = v * d;
        return static_cast<uint8_t>(f > 255.0f ? 255 : f);
    };
    return {ch(base[0]), ch(base[1]), ch(base[2])};
}

void normal_of(const float a[3], const float b[3], const float c[3], float out[3]) {
    const float ux = b[0] - a[0], uy = b[1] - a[1], uz = b[2] - a[2];
    const float vx = c[0] - a[0], vy = c[1] - a[1], vz = c[2] - a[2];
    out[0] = uy * vz - uz * vy;
    out[1] = uz * vx - ux * vz;
    out[2] = ux * vy - uy * vx;
    const float m = std::sqrt(out[0] * out[0] + out[1] * out[1] + out[2] * out[2]);
    if (m > 0.0001f) { out[0] /= m; out[1] /= m; out[2] /= m; }
}

pse::Renderer3D* g_renderer = nullptr;

// ---- floating origin --------------------------------------------------------
//
// Every vertex is handed to the projector RELATIVE TO THE CAMERA, and the
// camera itself sits at the origin. This is not tidiness, it is the fix for
// visible jitter.
//
// Renderer3D::project starts with `to_fixed(wx)`, which is the 1024 scale
// fixed point the whole projection runs in, and then multiplies by the view
// projection matrix and divides. The error in that chain grows with the
// MAGNITUDE of the coordinate going in, and a lap here is 2,400 units around,
// so a pod on the back straight was feeding the projector numbers around 600
// while the pod at the start line fed it numbers around 20. The road visibly
// shimmered at one end of the track and was rock steady at the other.
//
// Subtracting the camera first bounds every coordinate the projector ever sees
// by the far plane, 170, wherever on the track the race actually is. It costs
// three subtractions per vertex and it is the difference between a track that
// jitters and one that does not.
float g_origin[3] = {0.0f, 0.0f, 0.0f};
float g_forward[3] = {0.0f, 0.0f, 1.0f};
// The palette this frame is being drawn in. Frame state alongside the origin,
// for the two things that need a colour a long way from where the track is in
// scope: the spray a pod throws, and nothing else yet.
const Palette* g_palette = nullptr;
RenderStats g_stats{};

void note_coordinate(const float p[3]) {
    for (int i = 0; i < 3; ++i) {
        const float a = p[i] < 0.0f ? -p[i] : p[i];
        if (a > g_stats.max_coordinate) g_stats.max_coordinate = a;
    }
}

// The real near plane, which is not k_near.
//
// project() rejects a vertex once its NDC z reaches 0, and for this projection
// that happens at the HARMONIC MEAN of the two planes rather than at the near
// one: 2*n*f/(n+f). With 2 and 170 that is 3.953 units, not 2. Clipping has to
// happen at the line the engine actually rejects on, or the clipped vertex is
// rejected in turn and the polygon vanishes anyway.
constexpr float k_near_view = 2.0f * k_near * k_far / (k_near + k_far);
constexpr float k_clip_at = k_near_view * 1.02f;   // a whisker inside it

// Depth along the view axis of a camera relative point.
float view_z(const float p[3]) {
    return p[0] * g_forward[0] + p[1] * g_forward[1] + p[2] * g_forward[2];
}

// A convex polygon in camera relative space, flat shaded, CLIPPED against the
// near plane rather than dropped at it.
//
// The engine has no clipping of any kind: draw_mesh projects all three corners
// and bins the triangle if any one fails, and that is documented as deliberate.
// It is survivable for a model, which is small and either in front of you or
// not. It is not survivable for a road, because the strip under the nose is
// exactly the one whose near edge crosses the plane, so the ground vanished
// from under the pod the moment the camera got low or the pod pitched down. The
// road did not need to be dropped; it needed to be CUT, and cutting it is nine
// lines of Sutherland-Hodgman against one plane.
// `lit` is what tells a surface from a light. Everything in the world is
// lambert shaded off its own normal, which is right for road and rock and
// wrong for the two things here that emit: the plasma binder and the spray off
// the engines. Both are drawn as billboards, so their normals are horizontal,
// so the shader gave them the ambient floor and nothing else and they came out
// at 55 percent: white foam rendered as grey gravel and a plasma arc as a dull
// purple wire. An unlit polygon takes the colour it was authored in.
void poly(const float (*pts)[3], int count, const uint8_t colour[3],
          bool lit = true) {
    float clipped[10][3];
    int n = 0;
    for (int i = 0; i < count && n < 9; ++i) {
        const float* a = pts[i];
        const float* b = pts[(i + 1) % count];
        const float za = view_z(a), zb = view_z(b);
        const bool in_a = za >= k_clip_at, in_b = zb >= k_clip_at;
        if (in_a) {
            clipped[n][0] = a[0]; clipped[n][1] = a[1]; clipped[n][2] = a[2];
            ++n;
        }
        if (in_a != in_b && n < 9) {
            const float t = (k_clip_at - za) / (zb - za);
            clipped[n][0] = a[0] + (b[0] - a[0]) * t;
            clipped[n][1] = a[1] + (b[1] - a[1]) * t;
            clipped[n][2] = a[2] + (b[2] - a[2]) * t;
            ++n;
        }
    }
    if (n < 3) return;
    if (n != count) ++g_stats.clipped;

    int sx[10], sy[10], sz[10];
    for (int i = 0; i < n; ++i) {
        note_coordinate(clipped[i]);
        if (!g_renderer->project(clipped[i][0], clipped[i][1], clipped[i][2],
                                 sx[i], sy[i], sz[i])) {
            ++g_stats.dropped_far;
            return;   // past the FAR plane, which is a real reason to drop it
        }
        // ScreenTriangle stores screen x and y as int16 and renderer3d casts
        // the projected int straight in with no clamp, so a wide quad close to
        // the near plane can project past 32767 and wrap to the other side of
        // the screen. Clipping makes this rarer; it does not make it
        // impossible, because a vertex can be far off to the side and still in
        // front.
        sx[i] = sx[i] < -4000 ? -4000 : (sx[i] > 4000 ? 4000 : sx[i]);
        sy[i] = sy[i] < -4000 ? -4000 : (sy[i] > 4000 ? 4000 : sy[i]);
    }

    Rgb c{colour[0], colour[1], colour[2]};
    if (lit) {
        float nrm[3];
        normal_of(clipped[0], clipped[1], clipped[2], nrm);
        c = shade(colour, nrm[0], nrm[1], nrm[2]);
    }
    pse::ScreenTriangle tri{};
    tri.r0 = tri.r1 = tri.r2 = c.r;
    tri.g0 = tri.g1 = tri.g2 = c.g;
    tri.b0 = tri.b1 = tri.b2 = c.b;

    // Fanned from the first vertex, and wound to face the camera using
    // Rasterizer::rejected's own signed area expression character for
    // character. Written as its negative it flips exactly the triangles that
    // were already correct.
    for (int i = 1; i + 1 < n; ++i) {
        int a = 0, b = i, d = i + 1;
        const long area = static_cast<long>(sx[d] - sx[a]) * (sy[b] - sy[a])
                        - static_cast<long>(sy[d] - sy[a]) * (sx[b] - sx[a]);
        if (area == 0) continue;
        if (area < 0) { const int t = b; b = d; d = t; }
        tri.x0 = static_cast<int16_t>(sx[a]); tri.y0 = static_cast<int16_t>(sy[a]);
        tri.x1 = static_cast<int16_t>(sx[b]); tri.y1 = static_cast<int16_t>(sy[b]);
        tri.x2 = static_cast<int16_t>(sx[d]); tri.y2 = static_cast<int16_t>(sy[d]);
        tri.z0 = static_cast<uint16_t>(sz[a]);
        tri.z1 = static_cast<uint16_t>(sz[b]);
        tri.z2 = static_cast<uint16_t>(sz[d]);
        g_renderer->rasterizer().draw(tri);
        ++g_stats.triangles;
    }
}

void quad(const float p0[3], const float p1[3], const float p2[3],
          const float p3[3], const uint8_t colour[3], bool lit = true) {
    const float pts[4][3] = {
        {p0[0], p0[1], p0[2]}, {p1[0], p1[1], p1[2]},
        {p2[0], p2[1], p2[2]}, {p3[0], p3[1], p3[2]},
    };
    poly(pts, 4, colour, lit);
}

// ---- the road ---------------------------------------------------------------

// Eighteen segments ahead and three behind. Each is ten to fourteen triangles,
// so this is where the frame's ground budget goes, and it is spent on the one
// thing that tells the player where the track goes.
constexpr int k_view_segments = 18;
constexpr int k_view_behind = 3;

// The swell, as a TRIANGLE wave and not a sine. A sine here is one software
// float call per segment end per frame on a chip with no FPU, spent on a
// displacement a quarter of a unit tall that is two pixels at the near end and
// none at all at the far end. The shape of a wave at 120 pixels is carried by
// the fact that it MOVES, not by whether its crest is round.
float ripple(int index, uint32_t tick) {
    const int p = index * 5 + static_cast<int>(tick / 3);
    const int q = ((p % 12) + 12) % 12;
    const int up = q < 6 ? q : 12 - q;          // 0..6..0
    return (up - 3) * 0.085f;                   // about a quarter unit either way
}

// A point on the ground, `lateral` world units off the centreline at this
// node, `dy` above the node's own height, camera relative on the way out.
//
// Absolute units and not multiples of the half width, which is what the old
// one took. A multiple is fine for the road, whose whole geometry is a
// fraction of its own width, and wrong for everything beyond it: the shoulder
// falls over twelve UNITS whatever the road is doing, so expressing it as 1.35
// half widths made it three units wide on a normal stretch and four on the
// shortcut, and neither of those is twelve.
// The direction a node's cross section is laid out along: the average of the
// segment arriving and the segment leaving, not the one leaving.
//
// A MITRE, and without it no two strips in the game shared an edge. Every band
// took its normal from its own outgoing segment, so at a bend node i's outer
// edge and node i-1's outer edge landed in different places: a wedge of open
// sky between consecutive strips on the outside of every corner, and a wedge
// of overlap on the inside. The road had them too, a notch out of its own edge
// at every corner node, about a unit and a half wide at nine and a half of
// half width. Averaging the two makes the boundary of one strip literally the
// same points as the boundary of the next, so there is nothing left to fall
// between.
void node_dir(const Track& t, int index, float& dx, float& dz) {
    const int n = t.node_count;
    const TrackNode& prev = t.nodes[(index + n - 1) % n];
    const TrackNode& here = t.nodes[index];
    const TrackNode& next = t.nodes[(index + 1) % n];
    float ax = to_world(node_x(here) - node_x(prev));
    float az = to_world(node_z(here) - node_z(prev));
    float bx = to_world(node_x(next) - node_x(here));
    float bz = to_world(node_z(next) - node_z(here));
    const float ma = std::sqrt(ax * ax + az * az);
    const float mb = std::sqrt(bx * bx + bz * bz);
    if (ma > 0.0001f) { ax /= ma; az /= ma; }
    if (mb > 0.0001f) { bx /= mb; bz /= mb; }
    dx = ax + bx; dz = az + bz;
    const float m = std::sqrt(dx * dx + dz * dz);
    if (m > 0.0001f) { dx /= m; dz /= m; } else { dx = bx; dz = bz; }
}

void edge_at(const Track& t, int index, float side, float lateral, float dy,
             float out[3]) {
    const TrackNode& a = t.nodes[index];
    float dx, dz;
    node_dir(t, index, dx, dz);
    out[0] = to_world(node_x(a)) + dz * lateral * side - g_origin[0];
    out[1] = to_world(node_y(a)) + dy - g_origin[1];
    out[2] = to_world(node_z(a)) - dx * lateral * side - g_origin[2];
}

void edge_point(const Track& t, int index, float side, float out[3], float widen = 1.0f) {
    const float half = to_world(node_half_width(t.nodes[index])) * widen;
    edge_at(t, index, side < 0.0f ? -1.0f : 1.0f,
            half * (side < 0.0f ? -side : side), 0.0f, out);
}

// How far out this node's ground may reach on this side before its strip folds
// back over the road.
//
// Not a tidiness rule. The outward offsets of two consecutive nodes cross once
// the offset passes the local turn radius, and past that a strip is drawn over
// ground it does not own, at its OWN node's height. Measured on the desert,
// the fourteen half width plain that used to be here put the drawn ground as
// much as 7.5 units ABOVE the surface the pod was hovering on, at 38 of 197
// sampled positions: the pod buried in scenery belonging to a corner it had
// not reached yet. That is the "sinking into the ground off road" report, and
// no amount of care in the sim could have fixed it, because the sim was right.
//
// Three quarters of the radius, so a strip stops well short of the fold rather
// than at it. On a straight the radius is enormous and nothing is clamped,
// which is most of a lap.
float reach_limit(const Track& t, int index, float side) {
    const int n = t.node_count;
    const TrackNode& prev = t.nodes[(index + n - 1) % n];
    const TrackNode& here = t.nodes[index];
    const TrackNode& next = t.nodes[(index + 1) % n];
    float ax = to_world(node_x(here) - node_x(prev));
    float az = to_world(node_z(here) - node_z(prev));
    float bx = to_world(node_x(next) - node_x(here));
    float bz = to_world(node_z(next) - node_z(here));
    const float ma = std::sqrt(ax * ax + az * az);
    const float mb = std::sqrt(bx * bx + bz * bz);
    if (ma < 0.0001f || mb < 0.0001f) return 1.0e9f;
    ax /= ma; az /= ma; bx /= mb; bz /= mb;
    // For unit directions the length of the difference is the heading change,
    // so the radius is the arc length over it.
    const float tx = bx - ax, tz = bz - az;
    const float turn = std::sqrt(tx * tx + tz * tz);
    if (turn < 0.0001f) return 1.0e9f;
    // Which way this side points, in the frame edge_at offsets in.
    float mx, mz;
    node_dir(t, index, mx, mz);
    const float nx = mz * side, nz = -mx * side;
    if (tx * nx + tz * nz <= 0.0f) return 1.0e9f;   // the outside of the turn
    return 0.9f * mb / turn;
}

// How far the drawn plain reaches past the road edge, before the fold clamp.
constexpr float k_plain_reach = 46.0f;

// One node's ground on one side, as the three boundary points the strips are
// built between: the road edge, the foot of the shoulder, and the outer edge
// of the plain. draw_road builds its quads from this and drawn_ground() probes
// it, so a test cannot be measuring a different world from the one on screen.
struct GroundBand { float base[3], lip[3], shoulder[3], plain[3]; float wall; };

// Height of a flat-ish quad at a point in the XZ plane, or false if the point
// is outside it. Only the ground probe uses this; the renderer projects.
bool tri_height(const float a[3], const float b[3], const float c[3],
                float x, float z, float& y) {
    const float d = (b[2] - c[2]) * (a[0] - c[0]) + (c[0] - b[0]) * (a[2] - c[2]);
    if (d > -1e-6f && d < 1e-6f) return false;
    const float l0 = ((b[2] - c[2]) * (x - c[0]) + (c[0] - b[0]) * (z - c[2])) / d;
    const float l1 = ((c[2] - a[2]) * (x - c[0]) + (a[0] - c[0]) * (z - c[2])) / d;
    const float l2 = 1.0f - l0 - l1;
    if (l0 < 0.0f || l1 < 0.0f || l2 < 0.0f) return false;
    y = l0 * a[1] + l1 * b[1] + l2 * c[1];
    return true;
}

bool quad_height(const float p0[3], const float p1[3], const float p2[3],
                 const float p3[3], float x, float z, float& y) {
    return tri_height(p0, p1, p2, x, z, y) || tri_height(p0, p2, p3, x, z, y);
}

// The waterline, camera relative, or nothing on a dry planet. Every band point
// is raised to it, because surface_at is: the sea wins wherever the rock is
// lower. The moving ripple goes on top of this in draw_road, so what is drawn
// and what is driven differ by at most a quarter of a unit of swell.
float sea_level(const Track& t) {
    return has_water(t) ? to_world(water_level(t)) - g_origin[1] : -1.0e9f;
}

void ground_band(const Track& t, int index, float side, GroundBand& out) {
    const float half = to_world(node_half_width(t.nodes[index]));
    const float run = to_world(k_shoulder_run);
    const float limit = reach_limit(t, index, side);
    const float reach = k_plain_reach < limit ? k_plain_reach : limit;
    const int32_t wall = node_wall(t.nodes[index]);
    out.wall = to_world(wall);
    // Every height comes out of ground_offset, which is the sim's own function.
    // Nothing here decides what the ground does; it only decides where to put
    // the corners, and the corners are at the profile's knees.
    edge_at(t, index, side, half, 0.0f, out.base);
    edge_at(t, index, side, half, out.wall, out.lip);
    edge_at(t, index, side, half + run,
            to_world(ground_offset(wall, k_shoulder_run)), out.shoulder);
    edge_at(t, index, side, half + reach,
            to_world(ground_offset(wall, static_cast<int32_t>(reach * 65536.0f))),
            out.plain);
    const float sea = sea_level(t);
    float* points[4] = {out.base, out.lip, out.shoulder, out.plain};
    for (float* p : points) if (p[1] < sea) p[1] = sea;
}
// And the lip that a canyon rim turns outward at the top, so a wall reads as
// the edge of something rather than as a sheet standing on the desert.
constexpr float k_rim = 9.0f;

// A hole in the road, drawn as a hole. The sim has said for a long time that a
// gap has no surface at ANY distance to the side, which makes it a canyon
// across the world rather than a pit in the tarmac. Nothing drew that: the
// plain was laid straight across it, so a player who went round the jump flew
// over solid desert and fell through it, and a player who went at it saw the
// road stop at nothing at all.
//
// Two cliff faces and a floor. The near lip is drawn only where the run of gap
// nodes starts and the far lip only where it ends, so a fifty six unit chasm
// costs two faces and not fourteen.
void draw_chasm(const Track& t, int i, int band, float reach) {
    const int n = t.node_count;
    const int j = (i + 1) % n;
    const Palette& pal = t.palette;
    const float depth = to_world(k_chasm_depth);
    const float half_i = to_world(node_half_width(t.nodes[i]));
    const float half_j = to_world(node_half_width(t.nodes[j]));

    // The floor, in the darkest thing the palette has. It is below the crash
    // floor, so what the player is looking at is somewhere they have already
    // died by reaching.
    const uint8_t deep[3] = {
        static_cast<uint8_t>(pal.rock[1][0] * 2 / 5),
        static_cast<uint8_t>(pal.rock[1][1] * 2 / 5),
        static_cast<uint8_t>(pal.rock[1][2] * 2 / 5),
    };
    float fl[3], fr[3], nl[3], nr[3];
    edge_at(t, i, -1.0f, half_i + reach, -depth, nl);
    edge_at(t, i, 1.0f, half_i + reach, -depth, nr);
    edge_at(t, j, -1.0f, half_j + reach, -depth, fl);
    edge_at(t, j, 1.0f, half_j + reach, -depth, fr);
    quad(nl, fl, fr, nr, deep);

    // A lip is a wall across the whole width, from the ground down to the
    // floor. Only at the two ends of the run.
    const bool near_lip = !(t.nodes[(i + n - 1) % n].flags & kGap);
    const bool far_lip = !(t.nodes[j].flags & kGap);
    for (int end = 0; end < 2; ++end) {
        if (end == 0 ? !near_lip : !far_lip) continue;
        const int at = end == 0 ? i : j;
        const float half = end == 0 ? half_i : half_j;
        float tl[3], tr[3], bl[3], br[3];
        edge_at(t, at, -1.0f, half + reach, 0.0f, tl);
        edge_at(t, at, 1.0f, half + reach, 0.0f, tr);
        edge_at(t, at, -1.0f, half + reach, -depth, bl);
        edge_at(t, at, 1.0f, half + reach, -depth, br);
        // In SHADOW, and that is the whole legibility of a hole in the ground.
        // A cliff face painted from pal.wall is a light tan wall across a light
        // tan desert: the geometry was all there and the jump still read as a
        // shallow dip in the road. The one face the player actually looks at is
        // the far one, across the gap, and it has to be the darkest thing in
        // the frame or there is nothing to tell them to pull up.
        uint8_t face[3];
        for (int c = 0; c < 3; ++c)
            face[c] = static_cast<uint8_t>(pal.rock[1][c] * 3 / 5);
        quad(tl, tr, br, bl, face);
        ++g_stats.cliffs;
    }
}

void draw_road(const Track& t, const Pod& pod, uint32_t tick) {
    const Palette& pal = t.palette;
    // The sea, camera relative, exactly as surface_at reads it. One number
    // shared by the whole of this function so the drawn shoreline and the
    // driven one cannot end up in different places.
    const bool wet = has_water(t);
    const float sea = wet ? to_world(water_level(t)) - g_origin[1] : 0.0f;
    // The cross section, straight out of the sim's own constants. Anything
    // here that measured the shoulder for itself is how the two came to
    // disagree in the first place.
    const float shoulder_run = to_world(k_shoulder_run);
    const float shoulder_drop = to_world(k_shoulder_drop);
    const float roof = to_world(k_tunnel_height);

    for (int s = -k_view_behind; s < k_view_segments; ++s) {
        const int i = ((pod.node + s) % t.node_count + t.node_count) % t.node_count;
        const int j = (i + 1) % t.node_count;
        const TrackNode& a = t.nodes[i];
        const TrackNode& b_node = t.nodes[j];
        const int band = (i >> 1) & 1;
        const bool near = s <= 11;
        const float half_i = to_world(node_half_width(a));
        const float half_j = to_world(node_half_width(b_node));

        float li[3], lj[3], ri[3], rj[3];
        edge_at(t, i, -1.0f, half_i, 0.0f, li);
        edge_at(t, j, -1.0f, half_j, 0.0f, lj);
        edge_at(t, i, 1.0f, half_i, 0.0f, ri);
        edge_at(t, j, 1.0f, half_j, 0.0f, rj);

        // Where this segment stands relative to the sea. Whole segment or
        // nothing, so a shoreline lands on a node boundary and the eight units
        // of a segment become the beach. Cutting a quad along the waterline
        // would be more exact and would cost a clipper per segment for an edge
        // that is three pixels long.
        const float yi = to_world(node_y(a));
        const float yj = to_world(node_y(b_node));
        const bool road_wet = wet && yi < sea && yj < sea;
        const bool plain_wet = wet && yi - shoulder_drop < sea
                                   && yj - shoulder_drop < sea;
        const float wave_i = wet ? sea + ripple(i, tick) : 0.0f;
        const float wave_j = wet ? sea + ripple(j, tick) : 0.0f;
        // The road itself comes up to the waterline where it is under it, for
        // the same reason and by the same rule as the shoulder does. Skipping
        // this left a segment with one end above the sea and one below drawn
        // at its true height while the field held the pod at sea level, so on
        // the way into TIDEBREAK's trench the pod hovered up to seventeen
        // units over the road it could see.
        if (wet) {
            if (wave_i > li[1]) li[1] = wave_i;
            if (wave_i > ri[1]) ri[1] = wave_i;
            if (wave_j > lj[1]) lj[1] = wave_j;
            if (wave_j > rj[1]) rj[1] = wave_j;
        }

        const bool tunnel = (a.flags & kTunnel) != 0;
        // A hole under water is not a hole, it is more water.
        const bool chasm = (a.flags & kGap) != 0 && !road_wet;

        if (chasm) {
            const float reach = k_plain_reach < reach_limit(t, i, 1.0f)
                              ? k_plain_reach : reach_limit(t, i, 1.0f);
            draw_chasm(t, i, band, reach);
            continue;
        }

        // Ground and shoulders only where they can still be seen. Past about
        // ninety units out the shoulder is under a pixel tall and the ground
        // beyond it is the same few pixels as the horizon wall behind it, so
        // six of a far segment's fourteen triangles were drawing the horizon a
        // second time.
        if (near) {
            for (int side = 0; side < 2; ++side) {
                const float sgn = side ? 1.0f : -1.0f;
                const float* base_i = side ? ri : li;
                const float* base_j = side ? rj : lj;

                // The shoulder and the plain, at the offsets ground_offset has
                // its knees at, so the drawn ground and the driven ground meet
                // exactly rather than approximately.
                GroundBand gi, gj;
                ground_band(t, i, sgn, gi);
                ground_band(t, j, sgn, gj);

                // A CANYON WALL, and it is a vertical face standing on the
                // road edge rather than the four unit kerb this used to be.
                // The sim stops a pod dead at that line; a barrier the player
                // can see over does not say so, and being bounced off
                // something shorter than the vehicle reads as the game
                // cheating rather than as a wall.
                //
                // Drawn whenever EITHER end of the segment has rock on it, so a
                // canyon rises out of the desert over one node's spacing and
                // sinks back into it the same way. The sim interpolates the
                // same taper, so the mouth of a canyon is drivable rock at
                // exactly the heights it is drawn at.
                if (gi.wall > 0.0f || gj.wall > 0.0f) {
                    // Banded, like the road is. A canyon wall in one flat
                    // colour is a brown rectangle beside a brown rectangle:
                    // nothing crosses the edge of vision, so a hundred and
                    // eighty units of rock go past at three hundred and fifty
                    // and read as standing still.
                    uint8_t face[3];
                    for (int c = 0; c < 3; ++c)
                        face[c] = band ? pal.wall[c]
                                       : static_cast<uint8_t>(pal.wall[c] * 3 / 4);
                    quad(base_i, base_j, gj.lip, gi.lip, face);
                    ++g_stats.cliffs;
                }

                float wi[3], wj[3], fi[3], fj[3];
                for (int c = 0; c < 3; ++c) {
                    wi[c] = gi.shoulder[c]; wj[c] = gj.shoulder[c];
                    fi[c] = gi.plain[c];    fj[c] = gj.plain[c];
                }

                const uint8_t* plain_col = pal.ground[band];
                const uint8_t* bank_col = pal.rock[band];
                if (wet) {
                    // PER VERTEX, and the rule is surface_at's own: the sea
                    // wins wherever the rock is lower than it. Flipping a whole
                    // band to sea level the moment its outer edge went under
                    // drew water over a beach that was still a metre and a half
                    // above the waterline, and a pod driving there hovered
                    // sixteen units over the drawn surface.
                    if (wave_i > wi[1]) wi[1] = wave_i;
                    if (wave_j > wj[1]) wj[1] = wave_j;
                    if (wave_i > fi[1]) fi[1] = wave_i;
                    if (wave_j > fj[1]) fj[1] = wave_j;
                    if (plain_wet) {
                        plain_col = pal.water[band];
                        // The bank between the road edge and the plain. Where
                        // the road is dry and the plain is not, this is the
                        // shoreline, and it wants the foam colour: a beach is
                        // the one edge in the frame a player uses to judge how
                        // much road is left.
                        if (!road_wet) bank_col = pal.foam;
                        ++g_stats.sea;
                    }
                }
                // The rim and the plateau above a wall, or the bank and the
                // plain beside open road: the same two quads either way,
                // because ground_offset already said which one this is.
                if (gi.wall > 0.0f || gj.wall > 0.0f) {
                    bank_col = pal.rock[band];
                    plain_col = pal.rock[band ^ 1];
                }
                quad(gi.lip, gj.lip, wj, wi, bank_col);
                quad(wi, wj, fj, fi, plain_col);
            }
        }

        // Submerged road is SEA, drawn at sea level and not where the tarmac
        // is. That is not a decoration: surface_at hands the hover field the
        // waterline out here, so the pod is skimming twelve units above the
        // road at the bottom of TIDEBREAK's trench, and drawing the road it is
        // nowhere near would put the whole pod under the scenery.
        if (road_wet) {
            // The lane, in the shallows colour with foam down both edges. The
            // first version drew plain sea here and the racing line stopped
            // existing for a third of the lap: a player cannot aim at a track
            // that is not drawn, and "the road is under water" is not a reason
            // to stop telling them where it is. Same three strips the dashed
            // road uses, so a submerged straight and a dry one are laid out
            // the same way and read the same way.
            constexpr float k_surf = 0.12f;   // of a half width, each side
            float ai[3], aj[3], bi[3], bj[3];
            edge_at(t, i, -1.0f, half_i * (1.0f - k_surf), 0.0f, ai);
            edge_at(t, j, -1.0f, half_j * (1.0f - k_surf), 0.0f, aj);
            edge_at(t, i, 1.0f, half_i * (1.0f - k_surf), 0.0f, bi);
            edge_at(t, j, 1.0f, half_j * (1.0f - k_surf), 0.0f, bj);
            float li2[3], lj2[3], ri2[3], rj2[3];
            for (int c = 0; c < 3; ++c) {
                li2[c] = li[c]; lj2[c] = lj[c]; ri2[c] = ri[c]; rj2[c] = rj[c];
            }
            if (near) {
                quad(li2, lj2, aj, ai, pal.foam);
                quad(ai, aj, bj, bi, pal.shallow[band]);
                quad(bi, bj, rj2, ri2, pal.foam);
            } else {
                quad(li2, lj2, rj2, ri2, pal.shallow[band]);
            }
            ++g_stats.sea;
            continue;
        }

        const uint8_t* col = pal.road[band];
        uint8_t boost_col[3] = {90, 190, 255};
        if (a.flags & kBoost) col = boost_col;
        // Inside a tunnel everything is in shadow. There is no light model to
        // ask, so the shadow is the colour: two thirds of it, on the road and
        // on the roof, which at four bits a channel is two clear steps darker
        // and reads as being under something.
        uint8_t shade_col[3];
        if (tunnel) {
            for (int c = 0; c < 3; ++c)
                shade_col[c] = static_cast<uint8_t>(col[c] * 2 / 3);
            col = shade_col;
        }

        // The roof, drawn after the walls and before the road so a tunnel is
        // closed. It is the one piece of geometry in the game that is above the
        // pod, and the sim knows: race_tick clamps the pod under it, which is
        // what makes a tunnel a place you cannot glide out of.
        if (tunnel) {
            float li3[3], lj3[3], ri3[3], rj3[3];
            edge_at(t, i, -1.0f, half_i, roof, li3);
            edge_at(t, j, -1.0f, half_j, roof, lj3);
            edge_at(t, i, 1.0f, half_i, roof, ri3);
            edge_at(t, j, 1.0f, half_j, roof, rj3);
            uint8_t roof_col[3];
            for (int c = 0; c < 3; ++c)
                roof_col[c] = static_cast<uint8_t>(pal.rock[band][c] * 3 / 4);
            quad(li3, lj3, rj3, ri3, roof_col);
            ++g_stats.cliffs;
        }

        // The road is cut into three strips ACROSS rather than drawn as one
        // quad with a stripe laid on top, and the engine forces that rather
        // than anyone preferring it. One byte of depth over the 2..170 bracket
        // is about 0.66 units of view distance a step, so a stripe floated a
        // few centimetres above the tarmac has the SAME depth value as the
        // tarmac, ties go to whoever drew first, and the stripe simply never
        // appears. Coplanar strips that do not overlap have no tie to lose.
        const bool dash = ((i >> 1) & 1) == 1;
        if (!dash || !near) {
            quad(li, lj, rj, ri, col);
        } else {
            constexpr float k_lip = 0.22f;
            float ai[3], aj[3], bi[3], bj[3];
            edge_at(t, i, -1.0f, half_i * (1.0f - k_lip), 0.0f, ai);
            edge_at(t, j, -1.0f, half_j * (1.0f - k_lip), 0.0f, aj);
            edge_at(t, i, 1.0f, half_i * (1.0f - k_lip), 0.0f, bi);
            edge_at(t, j, 1.0f, half_j * (1.0f - k_lip), 0.0f, bj);
            if (wet) {
                if (wave_i > ai[1]) ai[1] = wave_i;
                if (wave_i > bi[1]) bi[1] = wave_i;
                if (wave_j > aj[1]) aj[1] = wave_j;
                if (wave_j > bj[1]) bj[1] = wave_j;
            }
            const uint8_t* edge_col = pal.edge;
            uint8_t edge_shade[3];
            if (tunnel) {
                for (int c = 0; c < 3; ++c)
                    edge_shade[c] = static_cast<uint8_t>(pal.edge[c] * 2 / 3);
                edge_col = edge_shade;
            }
            quad(li, lj, aj, ai, edge_col);
            quad(ai, aj, bj, bi, col);
            quad(bi, bj, rj, ri, edge_col);
        }
    }
}

// ---- the horizon -------------------------------------------------------------

// Straight out of Dust Rider's draw_horizon, and the reason to copy it rather
// than model terrain is that it is pinned to the CAMERA at a fixed depth: it
// stays on the skyline however far the road wanders, costs 64 triangles, and
// never has a corner cross the near plane. Corner heights come from a hash of
// the column index, so a ridge is in the same place every lap without a byte
// of it being stored.
void draw_horizon(const Track& t, const Camera& cam) {
    struct Wall { float depth, amp, base; uint32_t phase; const uint8_t* col; };
    const Wall walls[2] = {
        {128.0f, 22.0f, 7.0f, 11, t.palette.rock[0]},
        {92.0f, 14.0f, 3.0f, 0, t.palette.rock[1]},
    };
    for (const Wall& w : walls) {
        const float dx = std::sin(cam.yaw), dz = std::cos(cam.yaw);
        const float rx = dz, rz = -dx;
        // The camera is the origin now, so the wall's own height is simply
        // below it. It was pinned to the camera already; this just says so.
        const float base = -26.0f;
        const auto height = [&](int k) {
            const uint32_t h = static_cast<uint32_t>(k + w.phase) * 2654435761u;
            return w.base + w.amp * (((h >> 13) & 255) / 255.0f);
        };
        constexpr int k_cols = 16;
        constexpr float k_span = 26.0f;
        for (int i = 0; i < k_cols; ++i) {
            const float t0 = (i - k_cols / 2) * k_span, t1 = t0 + k_span;
            const int k0 = static_cast<int>(cam.x * 0.02f) + i;
            const float h0 = height(k0), h1 = height(k0 + 1);
            const float p0[3] = {dx * w.depth + rx * t0, base, dz * w.depth + rz * t0};
            const float p1[3] = {p0[0], base + h0, p0[2]};
            const float p3[3] = {dx * w.depth + rx * t1, base, dz * w.depth + rz * t1};
            const float p2[3] = {p3[0], base + h1, p3[2]};
            quad(p0, p1, p2, p3, w.col);
        }
    }
}

// ---- scenery -----------------------------------------------------------------

// Hashed from the world grid exactly as Dust Rider's ground detail is, so a
// rock stays put while the camera moves over it and not one byte of RAM holds
// where any of them are.
void draw_props(const Track& t, const Pod& pod) {
    for (int s = 0; s < k_view_segments; s += 7) {
        const int i = ((pod.node + s) % t.node_count + t.node_count) % t.node_count;
        const TrackNode& n = t.nodes[i];
        const uint32_t h = static_cast<uint32_t>(i) * 2654435761u;
        const float side = (h & 1) ? 1.0f : -1.0f;
        const float off = to_world(node_half_width(n)) + 16.0f + ((h >> 8) & 31);
        const float scale = 2.0f + ((h >> 16) & 7) * 0.6f;
        float p[3];
        edge_point(t, i, side, p, 1.0f + off / to_world(node_half_width(n)));
        const uint8_t* col = t.palette.rock[(h >> 3) & 1];
        note_coordinate(p);
        g_renderer->draw_mesh(models::twinflare::rock, p[0], p[1] + 1.0f, p[2],
                              static_cast<float>(h & 255) * 0.024f, scale,
                              col[0], col[1], col[2]);
    }
}

// ---- pods ---------------------------------------------------------------------

struct PodPose {
    float x, y, z;
    float yaw, pitch, roll, swing, yaw_rate;
    uint8_t racer;
    uint8_t dead;
    int32_t engine[2];
    int32_t engine_max;
    bool boosting;
    float throttle;
    uint32_t tick;      // the race clock, for anything that has to move
    bool spray;         // the surface under this pod is water
    float surface;      // world y of that surface
};

// A unit vector across a strand, perpendicular to it and to the line of sight,
// which is what makes a flat ribbon face the camera. False when there is no
// strand at all.
//
// A strand pointing AT the camera has no camera facing width: the cross
// product collapses, and just before it collapses it is a tiny vector whose
// direction is all rounding. Normalising that swings the ribbon's width
// through a right angle between one frame and the next, which is the cables
// and the binder "jittering and randomly teleporting". It is worst exactly
// where it is most visible, because a cable runs fore and aft and the chase
// camera looks fore and aft.
//
// So near end on, take the width off the world's up instead. Any stable axis
// will do; up is the one that keeps a nearly vertical ribbon looking like a
// ribbon.
bool ribbon_axis(const float a[3], const float b[3], float out[3]) {
    const float d[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]};
    // a is already camera relative, so the vector from the camera to it IS it.
    const float vx = a[0], vy = a[1], vz = a[2];
    out[0] = d[1] * vz - d[2] * vy;
    out[1] = d[2] * vx - d[0] * vz;
    out[2] = d[0] * vy - d[1] * vx;
    float m = std::sqrt(out[0] * out[0] + out[1] * out[1] + out[2] * out[2]);
    const float dl = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    const float vl = std::sqrt(vx * vx + vy * vy + vz * vz);
    if (dl < 0.0001f || vl < 0.0001f) return false;
    if (m < 0.15f * dl * vl) {
        out[0] = -d[2];
        out[1] = 0.0f;
        out[2] = d[0];
        m = std::sqrt(out[0] * out[0] + out[2] * out[2]);
        if (m < 0.0001f) { out[0] = 1.0f; out[1] = 0.0f; out[2] = 0.0f; m = 1.0f; }
    }
    for (int i = 0; i < 3; ++i) out[i] /= m;
    return true;
}

// A ribbon that always faces the camera, for the cables and the binder arc. A
// tube this thin is one pixel wide, so a tube's worth of triangles buys
// nothing.
void ribbon(const float a[3], const float b[3], const Camera&, float width,
            const uint8_t col[3], bool lit = true) {
    float n[3];
    if (!ribbon_axis(a, b, n)) return;
    for (float& v : n) v *= width;
    const float p0[3] = {a[0] - n[0], a[1] - n[1], a[2] - n[2]};
    const float p1[3] = {a[0] + n[0], a[1] + n[1], a[2] + n[2]};
    const float p2[3] = {b[0] + n[0], b[1] + n[1], b[2] + n[2]};
    const float p3[3] = {b[0] - n[0], b[1] - n[1], b[2] - n[2]};
    quad(p0, p1, p2, p3, col, lit);
    quad(p3, p2, p1, p0, col, lit);   // both faces: a ribbon has no outside
}

// White water, thrown up where the engines meet the sea.
//
// Billboards rather than geometry, and upright rather than fully camera
// facing: a plume of spray has a definite up, so pinning the quad's vertical
// axis to the world's and only turning it about that axis is both cheaper and
// more like the thing. Eight of them, one quad each, and only on a pod drawn
// at full detail over water, so a dry track pays nothing and a distant rival
// pays nothing.
constexpr int k_spray_count = 8;

void spray(const PodPose& p, const float left[3], const float right[3],
           const uint8_t foam[3]) {
    const float ground = p.surface - g_origin[1];
    const float fx = std::sin(p.yaw), fz = std::cos(p.yaw);
    for (int k = 0; k < k_spray_count; ++k) {
        // Each plume runs its own loop of the same length, offset so they do
        // not all fire together, and the phase is the RACE CLOCK rather than a
        // stored particle: nothing here is remembered between frames, which is
        // what lets this cost no RAM at all.
        const uint32_t seed = static_cast<uint32_t>(k) * 2654435761u;
        const int period = 22 + (seed >> 28);
        const float age = ((p.tick + k * 7) % period) / static_cast<float>(period);
        const float* from = (k & 1) ? right : left;
        // Out to the side, and further back the older it is.
        const float side = ((static_cast<int>(seed >> 8) & 15) - 7) * 0.16f;
        const float back = 1.1f + age * 4.6f;
        const float rise = 4.0f * age * (1.0f - age) * 1.5f;
        float at[3] = {
            from[0] - fx * back + fz * side,
            ground + rise,
            from[2] - fz * back - fx * side,
        };
        // Wider as it breaks up, which is what turns eight squares into a
        // plume rather than eight squares. Small: these sit between the pod
        // and the camera, so a droplet a unit across is fifteen pixels of the
        // hundred and twenty and it hides the vehicle it is coming off.
        const float sz = 0.09f + age * 0.17f;
        // Upright, and turned about the vertical to face the camera. `at` is
        // already camera relative, so it is also the direction to look along.
        float rx = at[2], rz = -at[0];
        const float m = std::sqrt(rx * rx + rz * rz);
        if (m < 0.0001f) continue;
        rx = rx / m * sz; rz = rz / m * sz;
        const float q[4][3] = {
            {at[0] - rx, at[1] + sz, at[2] - rz},
            {at[0] + rx, at[1] + sz, at[2] + rz},
            {at[0] + rx, at[1] - sz, at[2] + rz},
            {at[0] - rx, at[1] - sz, at[2] - rz},
        };
        quad(q[0], q[1], q[2], q[3], foam, false);
        ++g_stats.spray;
    }
}

// An offset in a PART's own frame, from a centre that is already camera
// relative. local_point works from the pod's origin and subtracts the world
// origin on the way out; this works from wherever a part was actually drawn,
// which is what anything attaching to that part needs.
void offset_from(const float centre[3], float yaw, float pitch, float roll,
                 float dx, float dy, float dz, float out[3]) {
    const float cy = std::cos(yaw), sy = std::sin(yaw);
    const float cp = std::cos(pitch), sp = std::sin(pitch);
    const float cr = std::cos(roll), sr = std::sin(roll);
    const float m[9] = {
        cy * cr + sy * sp * sr, -cy * sr + sy * sp * cr, sy * cp,
        cp * sr, cp * cr, -sp,
        -sy * cr + cy * sp * sr, sy * sr + cy * sp * cr, cy * cp,
    };
    out[0] = centre[0] + m[0] * dx + m[1] * dy + m[2] * dz;
    out[1] = centre[1] + m[3] * dx + m[4] * dy + m[5] * dz;
    out[2] = centre[2] + m[6] * dx + m[7] * dy + m[8] * dz;
}

// The back end of an engine mesh: how far aft the nozzle is and how wide it is
// there. Read off the mesh rather than written down beside it, because six
// racers fly six different engines and a table of their lengths is a table
// that goes stale the first time one of them is reshaped. Twenty vertices,
// twice a frame.
struct Tail { float z, r; };

Tail engine_tail(const pse::MeshData& m) {
    int16_t lo = 0;
    for (uint16_t i = 0; i < m.vertex_count; ++i)
        if (m.vertices[i].z < lo) lo = m.vertices[i].z;
    int16_t wide = 0;
    const int16_t band = static_cast<int16_t>(m.scale / 12);
    for (uint16_t i = 0; i < m.vertex_count; ++i) {
        if (m.vertices[i].z > lo + band) continue;
        const int16_t x = m.vertices[i].x < 0
            ? static_cast<int16_t>(-m.vertices[i].x) : m.vertices[i].x;
        if (x > wide) wide = x;
    }
    const float unit = m.scale > 0 ? 1.0f / m.scale : 0.0f;
    return {lo * unit, wide * unit};
}

// The exhaust, and it is the other half of turning the engines round.
//
// They used to be rockets: pointed at the front, flaring to a wide bell at the
// back. A podracer engine is a turbine, so the blunt intake is forward now and
// the nozzle is aft. That is the right way round, and it puts the NARROW end
// permanently on screen, because a chase camera only ever sees the back of
// these. A dark taper is not what the back of a running engine looks like.
//
// A TAPER along the engine's own axis, not a square on the end of it: a flame
// is read from its shape, wide where it leaves and thin where it runs out.
// Unlit, in the racer's own binder colour, because it is light rather than
// paint and has neither a livery nor an angle. Four triangles a nozzle, at
// full detail only.
void flare(const float at[3], float yaw, float pitch, float roll,
           const Tail& tail, float scale, const uint8_t col[3]) {
    const float wide = tail.r * 0.62f * scale;
    const float thin = tail.r * 0.16f * scale;
    float mouth[3], tip[3];
    offset_from(at, yaw, pitch, roll, 0.0f, 0.0f, tail.z + tail.r * 0.25f, mouth);
    offset_from(at, yaw, pitch, roll, 0.0f, 0.0f,
                tail.z - tail.r * 2.4f * scale, tip);
    float n[3];
    if (!ribbon_axis(mouth, tip, n)) return;
    const float q[4][3] = {
        {mouth[0] - n[0] * wide, mouth[1] - n[1] * wide, mouth[2] - n[2] * wide},
        {mouth[0] + n[0] * wide, mouth[1] + n[1] * wide, mouth[2] + n[2] * wide},
        {tip[0] + n[0] * thin, tip[1] + n[1] * thin, tip[2] + n[2] * thin},
        {tip[0] - n[0] * thin, tip[1] - n[1] * thin, tip[2] - n[2] * thin},
    };
    quad(q[0], q[1], q[2], q[3], col, false);
    quad(q[3], q[2], q[1], q[0], col, false);   // a flame has no outside either
}

// How far a point in a part's own frame lies outside that part's box. Zero
// means the anchor is inside the mesh.
void note_anchor(float dx, float dy, float dz, const float half[3]) {
    const float off[3] = {dx < 0 ? -dx : dx, dy < 0 ? -dy : dy, dz < 0 ? -dz : dz};
    for (int i = 0; i < 3; ++i) {
        const float over = off[i] - half[i];
        if (over > g_stats.cable_gap) g_stats.cable_gap = over;
    }
}

void local_point(const PodPose& p, float dx, float dy, float dz, float out[3]) {
    const float cy = std::cos(p.yaw), sy = std::sin(p.yaw);
    const float cp = std::cos(p.pitch), sp = std::sin(p.pitch);
    const float cr = std::cos(p.roll), sr = std::sin(p.roll);
    const float m[9] = {
        cy * cr + sy * sp * sr, -cy * sr + sy * sp * cr, sy * cp,
        cp * sr, cp * cr, -sp,
        -sy * cr + cy * sp * sr, sy * sr + cy * sp * cr, cy * cp,
    };
    out[0] = p.x + m[0] * dx + m[1] * dy + m[2] * dz - g_origin[0];
    out[1] = p.y + m[3] * dx + m[4] * dy + m[5] * dz - g_origin[1];
    out[2] = p.z + m[6] * dx + m[7] * dy + m[8] * dz - g_origin[2];
}

// Three levels of detail, and they are not an optimisation anyone reached for
// early. A full pod is 86 triangles, six of those is 516, and with the road at
// 200 and the horizon at 64 the whole pack inside forty units of each other,
// which is exactly the moment a race is won, measured 731 against a queue that
// holds 640. The queue does not grow on overflow: it drops triangles and
// counts them, so a close finish would have shed pieces of pods, which is the
// hardest failure in this engine to attribute to its cause.
void draw_pod(const PodPose& p, int lod, const Camera& cam) {
    const Racer& rc = racer(p.racer);
    // One pod per racer, indexed by the roster rather than chosen by a bit.
    // Both tables are in the order sim.cpp lists the racers in, which is the
    // order Racer::mesh counts in; a mismatch here is six pods wearing each
    // other's engines, so it is worth the two lines that say so.
    static const pse::MeshData* const k_engines[k_racer_count] = {
        &models::twinflare::engine_scarab, &models::twinflare::engine_wisp,
        &models::twinflare::engine_anvil, &models::twinflare::engine_needle,
        &models::twinflare::engine_nightjar, &models::twinflare::engine_fang,
    };
    static const pse::MeshData* const k_cockpits[k_racer_count] = {
        &models::twinflare::cockpit_scarab, &models::twinflare::cockpit_wisp,
        &models::twinflare::cockpit_anvil, &models::twinflare::cockpit_needle,
        &models::twinflare::cockpit_nightjar, &models::twinflare::cockpit_fang,
    };
    const int shape = rc.mesh % k_racer_count;
    const pse::MeshData& engine_mesh = *k_engines[shape];
    const pse::MeshData& cockpit_mesh = *k_cockpits[shape];
    // THE ENGINES LEAD AND THE COCKPIT TRAILS, and until now neither did:
    // every part was drawn at the pod's own yaw, so the whole thing rotated
    // as one rigid object and the two mass model the sim is running was
    // invisible. The sim was swinging; the renderer was not showing it.
    //
    // Out in front on their own mountings, the engines are already pointing
    // where the pod is going to be, so they carry the yaw plus a lead taken
    // from the turn rate. The cockpit hangs behind on cables, so it carries
    // the yaw MINUS the swing and sits out to the side of the line between
    // them. Through a fast corner the three parts visibly disagree about
    // which way the pod is facing, which is the whole look of the vehicle.
    PodPose eng = p;
    eng.yaw = p.yaw + p.yaw_rate * k_engine_lead;
    eng.pitch = p.pitch * 0.7f;
    eng.roll = p.roll * 0.55f;

    PodPose cab = p;
    cab.yaw = p.yaw - p.swing * k_cockpit_trail;
    cab.roll = p.roll + p.swing * 0.45f;

    float left[3], right[3], cockpit[3];
    local_point(eng, -k_engine_x, k_engine_y, k_engine_z, left);
    local_point(eng, k_engine_x, k_engine_y, k_engine_z, right);
    // The cockpit sits where the SWING put it, not where the hull points. This
    // is the only place the two mass model shows and it is the whole reason
    // for it: through a corner the cockpit is visibly off to one side of the
    // line between the engines.
    local_point(p, p.swing * k_swing_throw, -0.25f,
                k_pod_z - std::fabs(p.swing) * 0.6f, cockpit);
    note_coordinate(left);
    note_coordinate(right);
    note_coordinate(cockpit);

    if (lod >= 2) {
        // Two flat quads where the engines are, still in the pod's own colour,
        // so the pack still reads as a pack. Four triangles.
        const float* const ends[2] = {left, right};
        for (const float* c : ends) {
            const float q[4][3] = {
                {c[0] - 0.9f, c[1] + 0.7f, c[2]}, {c[0] + 0.9f, c[1] + 0.7f, c[2]},
                {c[0] + 0.9f, c[1] - 0.7f, c[2]}, {c[0] - 0.9f, c[1] - 0.7f, c[2]},
            };
            quad(q[0], q[1], q[2], q[3], rc.colour[0]);
        }
        return;
    }

    if (lod == 0) {
        g_renderer->draw_mesh(cockpit_mesh, cockpit[0], cockpit[1],
                              cockpit[2], cab.yaw, 1.0f,
                              rc.colour[0][0], rc.colour[0][1], rc.colour[0][2],
                              cab.pitch, 0, cab.roll);
        // BOTH ENDS OF A CABLE ARE ON THE PARTS THEY JOIN, which sounds
        // obvious and was not true. The pod end was a fixed point on the pod's
        // centreline, so it did not follow the cockpit when the cockpit swung:
        // 0.9 units adrift at rest and 2.4 at full swing, which is further
        // than the cockpit is long. The engine end sat 0.59 units inboard of
        // the engine centre against a hull radius of 0.62, so it grazed the
        // surface at best. The two cables therefore met in the middle of the
        // pod and attached to nothing at either end.
        //
        // They are anchored off the drawn positions now, in each part's OWN
        // frame, so a cable stays welded to its engine and its cockpit however
        // far the two disagree about which way they are facing.
        const uint8_t cable[3] = {58, 62, 72};
        float ca[3], cb[3], ea[3], eb[3];
        // On the cockpit's roof, forward of centre, one anchor per cable.
        offset_from(cockpit, cab.yaw, cab.pitch, cab.roll,
                    -k_cable_pod_x, k_cable_pod_y, k_cable_pod_z, ca);
        offset_from(cockpit, cab.yaw, cab.pitch, cab.roll,
                    k_cable_pod_x, k_cable_pod_y, k_cable_pod_z, cb);
        // On each engine's inboard flank, aft of centre, inside the hull so it
        // reads as emerging from it rather than touching it.
        offset_from(left, eng.yaw, eng.pitch, eng.roll,
                    k_cable_eng_in, k_cable_eng_y, k_cable_eng_z, ea);
        offset_from(right, eng.yaw, eng.pitch, eng.roll,
                    -k_cable_eng_in, k_cable_eng_y, k_cable_eng_z, eb);

        // How far each anchor sits outside the BOX of the part it belongs to,
        // measured in that part's own frame rather than as a distance in the
        // world. Zero is inside. This is the number the test reads, because a
        // cable ending beside its engine looks exactly like a cable attached
        // to it until one frame is looked at closely.
        note_anchor(k_cable_pod_x, k_cable_pod_y, k_cable_pod_z, k_cockpit_half);
        note_anchor(k_cable_eng_in, k_cable_eng_y, k_cable_eng_z, k_engine_half);

        if (!(p.dead & 1)) ribbon(ca, ea, cam, 0.09f, cable);
        if (!(p.dead & 2)) ribbon(cb, eb, cam, 0.09f, cable);

        // The energy binder: the arc of plasma strung between the engines, and
        // the one piece of a podracer everybody can draw from memory. It goes
        // out the moment either engine does, which is the clearest possible
        // signal that the pod is in trouble.
        //
        // And it ARCS. It used to be a fixed parabola redrawn identically
        // every frame, which is a painted-on decal of an arc: the one thing
        // lightning does is not stay still. Every other tick it takes a new
        // path, its brightness crawls along it, and now and then a second
        // strand jumps beside the first and is gone again.
        //
        // The jitter is TAPERED to zero at both ends, and that is the same
        // rule the cables are under: a strand whose endpoints wander is a
        // strand attached to nothing, which is exactly how the cables read
        // before they were nailed down. The middle is free to move; the two
        // ends are welded to the engines.
        if (p.dead == 0) {
            // The racer's own binder colour, and a hotter core struck from
            // it. Six pods in a pack were six identical violet arcs; the arc
            // is the brightest thing on a pod and the only part of it visible
            // from directly behind, so it is also the cheapest way to know who
            // just went past.
            const uint8_t glow[3] = {rc.arc[0], rc.arc[1], rc.arc[2]};
            const uint8_t hot[3] = {
                static_cast<uint8_t>(rc.arc[0] + (255 - rc.arc[0]) * 3 / 5),
                static_cast<uint8_t>(rc.arc[1] + (255 - rc.arc[1]) * 3 / 5),
                static_cast<uint8_t>(rc.arc[2] + (255 - rc.arc[2]) * 3 / 5),
            };
            // Two strands one tick in four, so the arc crackles rather than
            // doubling. Cheap on average and the cost lands on the frames that
            // can afford it, since a second strand is never up two frames
            // running.
            const int strands = ((p.tick >> 1) & 3) == 0 ? 2 : 1;
            for (int strand = 0; strand < strands; ++strand) {
                float prev[3];
                for (int k = 0; k <= k_arc_steps; ++k) {
                    const float u = k / static_cast<float>(k_arc_steps);
                    // A parabola, not a sine: it is zero at both ends, one in
                    // the middle, and it costs a multiply instead of a
                    // software trig call on a chip with no FPU.
                    const float taper = 4.0f * u * (1.0f - u);
                    // The strand MOVES between shapes rather than jumping
                    // between them. It used to pick a fresh random offset
                    // every other tick and snap to it, which at sixty frames a
                    // second is thirty teleports: not an arc, a fault. Two
                    // hashes a quarter of a second apart, and the strand slides
                    // from one to the other.
                    const uint32_t step = p.tick / 6;
                    const float mix = (p.tick % 6) * (1.0f / 6.0f);
                    const uint32_t seed = (static_cast<uint32_t>(k) * 2654435761u)
                                        ^ (strand * 7919u);
                    const uint32_t h0 = (seed ^ step) * 2246822519u;
                    const uint32_t h1 = (seed ^ (step + 1u)) * 2246822519u;
                    const auto pick = [&](int shift, float amount) {
                        const float from = static_cast<int>((h0 >> shift) & 31) - 15;
                        const float to = static_cast<int>((h1 >> shift) & 31) - 15;
                        return (from + (to - from) * mix) * taper * amount;
                    };
                    const float jx = pick(6, 0.034f);
                    const float jy = pick(13, 0.040f);
                    const float jz = pick(21, 0.028f);
                    const uint32_t h = h0;
                    float arc[3];
                    // Reaching all the way to the engines rather than stopping
                    // short of them, for the same reason the cables now do.
                    local_point(eng, (-1.0f + 2.0f * u) * (k_engine_x - 0.3f) + jx,
                                k_engine_y + taper * 0.55f + jy,
                                k_engine_z - 1.5f + jz, arc);
                    // Brightness crawls along the strand instead of the whole
                    // thing flashing, which at 120 pixels is the difference
                    // between plasma and a blinking wire.
                    if (k) ribbon(prev, arc, cam, strand ? 0.05f : 0.075f,
                                  ((h >> 3) & 3) ? glow : hot, false);
                    for (int c = 0; c < 3; ++c) prev[c] = arc[c];
                }
            }
        }
    }

    for (int i = 0; i < 2; ++i) {
        if ((p.dead >> i) & 1) continue;
        const float wear = p.engine_max ? static_cast<float>(p.engine[i]) / p.engine_max : 1.0f;
        const uint8_t tint = static_cast<uint8_t>(140 + 115 * wear);
        const float* at = i ? right : left;
        g_renderer->draw_mesh(engine_mesh, at[0], at[1], at[2], eng.yaw, 1.0f,
                              static_cast<uint8_t>(rc.colour[0][0] * tint / 255),
                              static_cast<uint8_t>(rc.colour[0][1] * tint / 255),
                              static_cast<uint8_t>(rc.colour[0][2] * tint / 255),
                              eng.pitch, p.boosting ? 90 : 0, eng.roll);
        if (lod == 0) {
            // Dimmer as the engine wears, so a burn losing its colour is the
            // pod saying where the damage is without a second bar on the HUD.
            const Tail tail = engine_tail(engine_mesh);
            const uint8_t fire[3] = {
                static_cast<uint8_t>(rc.arc[0] * (55.0f + 45.0f * wear) / 100.0f),
                static_cast<uint8_t>(rc.arc[1] * (45.0f + 55.0f * wear) / 100.0f),
                static_cast<uint8_t>(rc.arc[2] * (35.0f + 65.0f * wear) / 100.0f),
            };
            flare(at, eng.yaw, eng.pitch, eng.roll, tail,
                  p.boosting ? 1.45f : 0.85f, fire);
        }
    }

    // Spray last, so a plume sits in front of the hull that threw it rather
    // than behind it: everything here shares one depth buffer and a billboard
    // at the same distance as the engine it came off would otherwise lose the
    // tie.
    if (p.spray && lod == 0 && g_palette) spray(p, left, right, g_palette->foam);
}

PodPose pose_of(const Pod& pod, uint32_t tick) {
    PodPose p{};
    p.x = to_world(pod.x); p.y = to_world(pod.y); p.z = to_world(pod.z);
    p.yaw = to_rad(pod.yaw); p.pitch = to_rad(pod.pitch); p.roll = to_rad(pod.roll);
    p.swing = to_rad(pod.swing);
    p.yaw_rate = to_rad(pod.yaw_rate >> k_rate_fp) * k_tick_hz;
    p.racer = pod.racer_index;
    p.dead = pod.dead;
    p.engine[0] = pod.engine[0]; p.engine[1] = pod.engine[1];
    p.engine_max = pod.engine_max;
    p.boosting = pod.boost_ticks > 0;
    p.tick = tick;
    // Only while the field has something to push off. Spray thrown by a pod
    // sailing over a gap is spray coming off nothing.
    p.spray = pod.over_water && pod.grounded;
    p.surface = to_world(pod.y - pod.clearance);
    return p;
}

PodPose pose_of(const Rival& r, const Track& t, uint32_t tick) {
    PodPose p{};
    p.x = to_world(r.x); p.y = to_world(r.y); p.z = to_world(r.z);
    p.yaw = to_rad(r.yaw); p.roll = to_rad(r.roll);
    p.yaw_rate = 0.0f;
    p.racer = r.racer_index;
    p.engine[0] = p.engine[1] = 1000;
    p.engine_max = 1000;
    p.tick = tick;
    // A rival flies the centreline at a fixed hover height, so its surface is
    // that height below it and no query is needed. It throws spray for the
    // same reason the player does: a pack running over water where only one
    // pod marks it would read as the others being on rails.
    p.surface = to_world(r.y - k_hover_height);
    p.spray = has_water(t) && r.y - k_hover_height <= water_level(t) + fp(0, 500);
    return p;
}

// ---- the HUD ------------------------------------------------------------------

void number(char* out, int32_t value, int width) {
    for (int i = width - 1; i >= 0; --i) {
        out[i] = static_cast<char>('0' + value % 10);
        value /= 10;
    }
    out[width] = '\0';
}

// Rule 9: the minimum, measured rather than placed by eye, and nothing names a
// button. Laid out where a podracer HUD has always been, so a player who has
// seen one reads it without being told: lap top left, position top right, the
// two engine bars bottom left, speed bottom right. The captions are gone
// because at 120 pixels a three letter label under every number costs a fifth
// of the screen to say what the number's position already says.
void draw_hud(const Race& race, const pse::RenderTarget& target) {
    const Pod& pod = race.pod;
    const Track& t = track(race.track_index);

    char buf[16];
    const int lap = pod.lap > t.laps ? t.laps : pod.lap;
    buf[0] = static_cast<char>('0' + lap);
    buf[1] = '/';
    buf[2] = static_cast<char>('0' + t.laps);
    buf[3] = '\0';
    pse::draw_text(target, buf, 3, 3, 236, 240, 248);

    buf[0] = static_cast<char>('0' + race.place);
    buf[1] = '/';
    buf[2] = '6';
    buf[3] = '\0';
    pse::draw_text(target, buf, k_w - 3 - pse::text_width(buf), 3, 236, 240, 248);

    // Two engine bars, side by side, because there are two engines and their
    // health is the one thing in this game that is not shared. A single
    // combined bar would hide the exact state the whole damage model is about.
    for (int i = 0; i < 2; ++i) {
        const int bx = 4 + i * 9, by = k_h - 32, bh = 28;
        pse::fill_rect(target, bx, by, 7, bh, 22, 26, 34);
        if (engine_dead(pod, i)) {
            // A hollow box with a cross, not an empty bar: an empty bar reads
            // as "nearly dead" and this one is not coming back.
            for (int k = 0; k < 7; ++k) {
                pse::plot_pixel(target, bx + k, by + bh - 8 + k % 7, 200, 70, 78);
                pse::plot_pixel(target, bx + 6 - k, by + bh - 8 + k % 7, 200, 70, 78);
            }
            continue;
        }
        const int fill = pod.engine_max
            ? (bh - 2) * pod.engine[i] / pod.engine_max : 0;
        const int tenths = pod.engine_max ? pod.engine[i] * 10 / pod.engine_max : 0;
        const uint8_t r = tenths > 6 ? 96 : (tenths > 2 ? 236 : 232);
        const uint8_t g = tenths > 6 ? 224 : (tenths > 2 ? 196 : 76);
        const uint8_t b = tenths > 6 ? 132 : (tenths > 2 ? 72 : 84);
        pse::fill_rect(target, bx + 1, by + 1 + (bh - 2 - fill), 5, fill, r, g, b);
    }

    // Heat, above the speed. A bar and not a dial: a dial at this size is nine
    // pixels of arc and a lie about how much resolution the reader has.
    const int hx = k_w - 38, hy = k_h - 30;
    pse::fill_rect(target, hx, hy, 34, 5, 22, 26, 34);
    const int heat_w = static_cast<int>(32 * pod.heat / k_heat_one);
    const int warn_at = 32 * k_heat_warn / k_heat_one;
    const bool hot = pod.heat > k_heat_warn;
    const bool blink = ((race.ticks / 8) & 1) == 0;
    if (!hot || blink) {
        const uint8_t r = pod.locked ? 255 : (hot ? 255 : 96);
        const uint8_t g = pod.locked ? 90 : (hot ? 150 : 200);
        const uint8_t b = pod.locked ? 90 : (hot ? 60 : 240);
        pse::fill_rect(target, hx + 1, hy + 1, heat_w, 3, r, g, b);
    }
    pse::fill_rect(target, hx + 1 + warn_at, hy, 1, 5, 255, 255, 255);

    // The arm light. Boost is gated on speed, so the player has to know when a
    // double tap will do anything, and this is the whole of telling them:
    // three pixels that go green. It is the one thing the speed gate adds to
    // the HUD, and it is cheaper than a player who taps twice, gets nothing,
    // and concludes the boost is broken.
    pse::fill_rect(target, hx - 7, hy, 5, 5, 18, 22, 30);
    if (boost_armed(pod)) pse::fill_rect(target, hx - 6, hy + 1, 3, 3, 90, 235, 130);

    // Speed, bottom right, drawn double. The one number worth spending pixels
    // on.
    const int32_t shown = pod_speed(pod) * k_tick_hz / k_one * k_speed_display;
    int digits = shown >= 1000 ? 4 : (shown >= 100 ? 3 : (shown >= 10 ? 2 : 1));
    number(buf, shown, digits);
    const int sw = pse::text_width(buf, 2);
    pse::draw_text(target, buf, k_w - 3 - sw, k_h - 20,
                   pod.boost_ticks > 0 ? 255 : 232,
                   pod.boost_ticks > 0 ? 214 : 238,
                   pod.boost_ticks > 0 ? 96 : 248, 2);

    // The one line of state, only when there is something to say, and never a
    // button name.
    const char* msg = nullptr;
    uint8_t mr = 255, mg = 255, mb = 255;
    if (pod.wreck_ticks > 0)      { msg = "WRECKED";    mr = 255; mg = 90;  mb = 90; }
    else if (pod.locked)          { msg = "VENTING";    mr = 255; mg = 150; mb = 60; }
    else if (pod.dead)            { msg = "ONE ENGINE"; mr = 255; mg = 190; mb = 80; }
    if (msg) {
        const int w = pse::text_width(msg);
        pse::fill_rect(target, 60 - w / 2 - 3, 46, w + 6, 11, 10, 10, 16);
        pse::draw_text_centred(target, msg, 60, 48, mr, mg, mb);
    }
}

// Speed lines. There is no blending anywhere in the engine, so these are
// sparse plots and nothing else, which is also all they need to be: what sells
// speed is the rate they stream past, not their opacity.
void draw_speed_lines(const Race& race, const pse::RenderTarget& target) {
    const int32_t v = pod_speed(race.pod) * k_tick_hz / k_one;
    if (v < 26) return;
    const int count = v / 6 > 14 ? 14 : v / 6;
    for (int i = 0; i < count; ++i) {
        const uint32_t seed = static_cast<uint32_t>(i) * 2654435761u;
        const float a = ((seed >> 8) % 360) * 0.01745f;
        const float rad = 14.0f + ((seed >> 16) % 46);
        const float f = ((race.ticks * (12 + v / 4) + i * 91) % 1000) / 1000.0f;
        const float rr = rad + f * 46.0f;
        const int x = static_cast<int>(60 + std::cos(a) * rr);
        const int y = static_cast<int>(62 + std::sin(a) * rr * 0.8f);
        const uint8_t g = race.pod.boost_ticks > 0 ? 240 : 190;
        const int len = 1 + v / 30;
        for (int k = 0; k < len; ++k) {
            pse::plot_pixel(target, x + static_cast<int>(std::cos(a) * k),
                            y + static_cast<int>(std::sin(a) * 0.8f * k), g, g, g);
        }
    }
}

// ---- screens --------------------------------------------------------------------

void draw_bar(const pse::RenderTarget& target, int x, int y, int w, int value, int of) {
    pse::fill_rect(target, x, y, w, 5, 26, 30, 42);
    pse::fill_rect(target, x, y, w * value / of, 5, 232, 138, 43);
}

// The pod on the pod select screen, turning.
//
// Six racers described by a name, a pilot and six bars, and nothing at all
// about the thing you were choosing: two of the six fly a different engine
// mesh, all six are a different colour, and none of that reached the one
// screen where it matters. It is the same draw_pod the race uses, at the same
// level of detail, so what turns here is exactly what you get.
//
// The camera sits above and behind, which is the angle the race is played
// from: recognising your pod at speed from a picture taken from somewhere else
// is a thing to have to learn.
constexpr float k_show_dist = 9.4f;     // back from the pod
constexpr float k_show_high = 3.0f;     // and above it
constexpr float k_show_pitch = -0.44f;  // tilted down past it, so it sits high
constexpr uint32_t k_show_turn_ms = 5200;   // one revolution
// A podracer is not centred on its own origin: the engines reach five units
// forward of it and the cockpit two and a half back. Spun about the origin it
// swings round the screen like something on a string. Offsetting the pose by
// the middle of that puts the turn where the eye expects it, and it also buys
// a unit of near plane margin, which at ten units back is not spare.
constexpr float k_show_centre = 1.2f;

void draw_pod_showcase(const Chrome& chrome) {
    PodPose p{};
    p.yaw = (chrome.time_ms % k_show_turn_ms) * (6.2831853f / k_show_turn_ms);
    p.x = -std::sin(p.yaw) * k_show_centre;
    p.y = 0.0f;
    p.z = -std::cos(p.yaw) * k_show_centre;
    p.racer = chrome.pod;
    p.engine[0] = p.engine[1] = 1000;
    p.engine_max = 1000;
    // The clock the binder arc crackles on. Slower than a race tick, because a
    // pod standing still wants a hum rather than a fault.
    p.tick = chrome.time_ms / 24;
    Camera cam{};
    cam.pitch = k_show_pitch;
    draw_pod(p, 0, cam);

    // Where it landed on screen, for the layout test. The corners of the pod's
    // own box, turned with it and projected: eight projections on a menu, and
    // the only way to say "it fits" as a number rather than as an opinion.
    // Half extents match what draw_pod actually places: engines out to 3.3 and
    // forward to 5.05, cockpit back to 2.6, the binder arc the highest thing on
    // it.
    g_stats.showcase_top = 32767;
    g_stats.showcase_bottom = -32768;
    const float box[3][2] = {{-3.3f, 3.3f}, {-0.7f, 1.4f}, {-2.6f, 5.05f}};
    for (int corner = 0; corner < 8; ++corner) {
        float at[3];
        local_point(p, box[0][corner & 1], box[1][(corner >> 1) & 1],
                    box[2][(corner >> 2) & 1], at);
        int sx = 0, sy = 0, sz = 0;
        if (!g_renderer->project(at[0], at[1], at[2], sx, sy, sz)) continue;
        if (sy < g_stats.showcase_top) g_stats.showcase_top = static_cast<int16_t>(sy);
        if (sy > g_stats.showcase_bottom) g_stats.showcase_bottom = static_cast<int16_t>(sy);
    }
}

void draw_pod_select(const Chrome& chrome, const pse::RenderTarget& target) {
    const Racer& rc = racer(chrome.pod);
    // A band behind the name, because the pod turns under it and four bright
    // letters over an engine are unreadable exactly when the engine is closest.
    pse::fill_rect(target, 0, 0, k_w, 25, 14, 16, 26);
    pse::draw_text_centred(target, rc.name, 60, 6, 240, 244, 250);
    pse::draw_text_centred(target, rc.pilot, 60, 16, 130, 138, 156);
    static const char* k_labels[6] = {"TOP", "ACC", "GRIP", "COOL", "FIX", "HULL"};
    const uint8_t stats[6] = {rc.top, rc.acc, rc.grip, rc.cool, rc.fix, rc.hull};
    for (int i = 0; i < 6; ++i) {
        const int y = 84 + (i % 3) * 12;
        const int x = i < 3 ? 4 : 62;
        pse::draw_text(target, k_labels[i], x, y, 130, 138, 156);
        draw_bar(target, x + 28, y + 1, 26, stats[i], 5);
    }
    pse::draw_text(target, "<", 3, 44, 200, 206, 220);
    pse::draw_text(target, ">", 112, 44, 200, 206, 220);
}

// The map is the description, and it is plotted from the same node ring the
// sim drives on rather than drawn: a diagram of a track can be wrong about the
// track, and a plot of it cannot.
void draw_track_map(const Track& t, const pse::RenderTarget& target,
                    int top, int height) {
    int32_t x0 = INT32_MAX, x1 = INT32_MIN, z0 = INT32_MAX, z1 = INT32_MIN;
    for (uint16_t i = 0; i < t.node_count; ++i) {
        const int32_t x = node_x(t.nodes[i]), z = node_z(t.nodes[i]);
        if (x < x0) x0 = x; if (x > x1) x1 = x;
        if (z < z0) z0 = z; if (z > z1) z1 = z;
    }
    const int32_t span = (x1 - x0) > (z1 - z0) ? (x1 - x0) : (z1 - z0);
    if (span <= 0) return;
    const int pad = 6;
    const int size = (height - pad * 2);
    for (uint16_t i = 0; i < t.node_count; ++i) {
        const TrackNode& n = t.nodes[i];
        const int px = pad + 60 - size / 2
            + static_cast<int>(static_cast<int64_t>(node_x(n) - x0) * size / span);
        const int pz = top + height - pad
            - static_cast<int>(static_cast<int64_t>(node_z(n) - z0) * size / span);
        uint8_t r = t.palette.road[0][0], g = t.palette.road[0][1], b = t.palette.road[0][2];
        if (n.flags & kWall)  { r = t.palette.wall[0]; g = t.palette.wall[1]; b = t.palette.wall[2]; }
        if (n.flags & kShort) { r = 140; g = 240; b = 150; }
        if (n.flags & kRamp)  { r = 255; g = 190; b = 70; }
        if (n.flags & kBoost) { r = 90;  g = 190; b = 255; }
        if (n.flags & kGap)   { r = 40;  g = 44;  b = 56; }
        // Under the waterline last, because on a track with a sea that is the
        // thing about a stretch of road that most changes how it drives, and
        // because a "gap" over water is not a gap. A third of TIDEBREAK is
        // sea, and the map is where a player finds that out before committing
        // to a three lap race on it.
        // The DEEP colour rather than the shallows, and the difference is
        // legibility rather than accuracy: a shallows blue next to a road that
        // is already teal is two cyans four steps apart, which a pixel counter
        // can separate and an eye at two dots per node cannot.
        if (has_water(t) && node_y(n) < water_level(t)) {
            r = t.palette.water[0][0];
            g = t.palette.water[0][1];
            b = t.palette.water[0][2];
        }
        pse::fill_rect(target, px - 1, pz - 1, 2, 2, r, g, b);
    }
    const TrackNode& start = t.nodes[0];
    const int sx = pad + 60 - size / 2
        + static_cast<int>(static_cast<int64_t>(node_x(start) - x0) * size / span);
    const int sz = top + height - pad
        - static_cast<int>(static_cast<int64_t>(node_z(start) - z0) * size / span);
    pse::fill_rect(target, sx - 2, sz - 2, 5, 5, 245, 245, 250);
}

void draw_pause(const Chrome& chrome, const pse::RenderTarget& target) {
    // A solid panel behind the rows, not just a dither. There is no blending
    // in this engine, so a checkerboard over a bright desert is still half a
    // bright desert and three words of 5x7 on it are genuinely hard to read.
    for (int y = 0; y < k_h; ++y)
        for (int x = (y & 1); x < k_w; x += 2)
            pse::plot_pixel(target, x, y, 8, 9, 14);
    static const char* k_rows[3] = {"RESUME", "RESTART", "QUIT"};
    int widest = 0;
    for (const char* row : k_rows) {
        const int w = pse::text_width(row);
        if (w > widest) widest = w;
    }
    pse::fill_rect(target, 60 - widest / 2 - 8, 33, widest + 16, 3 * 14 + 8, 12, 13, 20);
    for (int i = 0; i < 3; ++i) {
        const int w = pse::text_width(k_rows[i]);
        const int y = 40 + i * 14;
        if (i == chrome.menu_item)
            pse::fill_rect(target, 60 - w / 2 - 4, y - 2, w + 8, 11, 232, 138, 43);
        pse::draw_text_centred(target, k_rows[i], 60, y,
                               i == chrome.menu_item ? 20 : 210,
                               i == chrome.menu_item ? 12 : 216,
                               i == chrome.menu_item ? 4 : 228);
    }
}

void draw_results(const Race& race, const pse::RenderTarget& target) {
    char buf[16];
    static const char* k_place[6] = {"1ST", "2ND", "3RD", "4TH", "5TH", "6TH"};
    pse::draw_text_centred(target, k_place[(race.place - 1) % 6], 60, 12,
                           255, 214, 96, 2);
    pse::draw_text_centred(target, track(race.track_index).name, 60, 36,
                           180, 188, 206);
    pse::draw_text(target, "BEST", 12, 56, 150, 158, 176);
    const uint32_t best = race.best_lap ? race.best_lap : race.ticks;
    number(buf, (best / 100) % 100, 2);
    buf[2] = '.';
    number(buf + 3, (best / 10) % 10, 1);
    buf[4] = '\0';
    pse::draw_text(target, buf, 108 - pse::text_width(buf), 56, 236, 240, 248);
    pse::draw_text(target, racer(race.pod.racer_index).name, 12, 72, 150, 158, 176);
}

}  // namespace

void render_frame(const Race& race, const Chrome& chrome,
                  const pse::RenderTarget& target) {
    pse::Rasterizer& raster = pse::shared_rasterizer();
    pse::FrameQueue& queue = pse::shared_queue();
    static pse::Renderer3D renderer(raster);
    g_renderer = &renderer;

    const Track& t = track(chrome.screen == Screen::TrackSelect ? chrome.track
                                                               : race.track_index);
    const Palette& pal = t.palette;

    // Menus that are lists get a flat ground and no world at all. Drawing a
    // race behind a menu costs a full frame of geometry to be looked at
    // through a panel.
    // Pod select draws a real pod, so it takes the 3D path: a camera, a depth
    // buffer and the same draw_pod the race uses. Immediate mode rather than
    // collect and split, because one pod is eighty six triangles and handing
    // half of them to the other core costs more than it saves.
    if (chrome.screen == Screen::PodSelect) {
        g_stats = RenderStats{};
        g_palette = &pal;
        raster.begin_frame(target);
        raster.clear_gradient(20, 23, 38, 9, 10, 17);
        renderer.set_depth_range(k_near, k_far);
        renderer.set_fov(k_fov);
        // The camera is at the origin and the pod is placed relative to it, the
        // same floating origin the race runs under, so nothing in draw_pod
        // needs to know which screen it is on.
        g_origin[0] = 0.0f;
        g_origin[1] = k_show_high;
        g_origin[2] = -k_show_dist;
        g_forward[0] = 0.0f;
        g_forward[1] = std::sin(k_show_pitch);
        g_forward[2] = std::cos(k_show_pitch);
        Camera show{};
        show.pitch = k_show_pitch;
        renderer.set_camera_basis(0.0f, 0.0f, 0.0f, camera_basis(show));
        draw_pod_showcase(chrome);
        draw_pod_select(chrome, target);
        return;
    }

    if (chrome.screen == Screen::TrackSelect || chrome.screen == Screen::Results) {
        raster.begin_frame(target);
        raster.clear_gradient(18, 20, 32, 10, 11, 18);
        if (chrome.screen == Screen::TrackSelect) {
            draw_track_map(t, target, 12, 92);
            pse::fill_rect(target, 0, 0, k_w, 11, 10, 12, 18);
            pse::draw_text_centred(target, t.name, 60, 2, 240, 244, 250);
            pse::draw_text(target, "<", 3, 54, 200, 206, 220);
            pse::draw_text(target, ">", 112, 54, 200, 206, 220);
        } else {
            draw_results(race, target);
        }
        return;
    }

    g_stats = RenderStats{};
    queue.reset();
    raster.begin_frame_collect(target, queue);

    const Camera cam = follow(race.pod, 1.0f / 60.0f);
    renderer.set_depth_range(k_near, k_far);
    renderer.set_fov(k_fov);
    // At the origin, because every vertex handed to it is camera relative.
    g_origin[0] = cam.x; g_origin[1] = cam.y; g_origin[2] = cam.z;
    g_forward[0] = std::sin(cam.yaw) * std::cos(cam.pitch);
    g_forward[1] = std::sin(cam.pitch);
    g_forward[2] = std::cos(cam.yaw) * std::cos(cam.pitch);
    renderer.set_camera_basis(0.0f, 0.0f, 0.0f, camera_basis(cam));

    g_palette = &pal;
    draw_horizon(t, cam);
    draw_road(t, race.pod, race.ticks);
    draw_props(t, race.pod);

    // Sorted near to far so the rank cap below means what it says: only the
    // nearest rival is ever drawn in full, however close the others get.
    // Distance alone held the budget in every frame except the one where the
    // whole pack is alongside, which is the frame the player cares about.
    uint8_t order[k_rival_count];
    int64_t dist[k_rival_count];
    for (int i = 0; i < k_rival_count; ++i) {
        order[i] = static_cast<uint8_t>(i);
        const int64_t dx = race.rivals[i].x - race.pod.x;
        const int64_t dz = race.rivals[i].z - race.pod.z;
        dist[i] = (dx * dx + dz * dz) >> 20;
    }
    for (int i = 1; i < k_rival_count; ++i) {
        for (int j = i; j > 0 && dist[order[j]] < dist[order[j - 1]]; --j) {
            const uint8_t tmp = order[j]; order[j] = order[j - 1]; order[j - 1] = tmp;
        }
    }
    for (int rank = 0; rank < k_rival_count; ++rank) {
        const Rival& r = race.rivals[order[rank]];
        const float dx = to_world(r.x - race.pod.x), dz = to_world(r.z - race.pod.z);
        const float d2 = dx * dx + dz * dz;
        if (d2 > 160.0f * 160.0f) continue;
        int lod = d2 > 90.0f * 90.0f ? 2 : (d2 > 45.0f * 45.0f ? 1 : 0);
        const int floor_lod = rank == 0 ? 0 : (rank < 3 ? 1 : 2);
        if (lod < floor_lod) lod = floor_lod;
        draw_pod(pose_of(r, t, race.ticks), lod, cam);
    }

    PodPose mine = pose_of(race.pod, race.ticks);
    draw_pod(mine, 0, cam);

    const pse::SkyGradient sky{pal.sky_top[0], pal.sky_top[1], pal.sky_top[2],
                               pal.sky_bottom[0], pal.sky_bottom[1], pal.sky_bottom[2]};
    pse::run_split(raster, queue, sky);
    raster.end_collect();

    // Immediate mode from here: overlays and text, after the split workers have
    // finished, because drawing while a split is in flight would race them.
    draw_speed_lines(race, target);
    draw_hud(race, target);
    if (race.pod.flash_ticks > 0) {
        for (int i = 0; i < 200; ++i) {
            const uint32_t h = static_cast<uint32_t>(i * 2654435761u + race.ticks);
            pse::plot_pixel(target, (h >> 7) % k_w, (h >> 17) % k_h, 255, 90, 90);
        }
    }

    if (chrome.screen == Screen::Paused) draw_pause(chrome, target);
    if (chrome.screen == Screen::Title) {
        pse::draw_text_centred(target, "TWIN", 60, 20, 255, 220, 120, 2);
        pse::draw_text_centred(target, "FLARE", 60, 38, 255, 150, 50, 2);
    }
}

const RenderStats& render_stats() { return g_stats; }

bool drawn_ground(const Track& t, uint16_t hint, int32_t x, int32_t z, float& y) {
    // The probe runs in world space, so the floating origin is stood down for
    // the duration. Legitimate here and nowhere else: this is called from the
    // tests between frames, never while one is being drawn, and it restores
    // what it found. The alternative is a second copy of the band arithmetic,
    // which is exactly the thing that let the drawn ground and the driven
    // ground drift apart in the first place.
    float saved[3] = {g_origin[0], g_origin[1], g_origin[2]};
    g_origin[0] = g_origin[1] = g_origin[2] = 0.0f;

    const float px = to_world(x), pz = to_world(z);
    const int n = t.node_count;
    bool any = false;
    // Wide enough to catch a strip from a node the point is not beside, which
    // is the whole failure this exists to detect.
    for (int k = -40; k <= 40; ++k) {
        const int i = ((hint + k) % n + n) % n;
        const int j = (i + 1) % n;
        if (t.nodes[i].flags & kGap) continue;   // a hole draws no ground
        const float half_i = to_world(node_half_width(t.nodes[i]));
        const float half_j = to_world(node_half_width(t.nodes[j]));
        float li[3], lj[3], ri[3], rj[3];
        edge_at(t, i, -1.0f, half_i, 0.0f, li);
        edge_at(t, j, -1.0f, half_j, 0.0f, lj);
        edge_at(t, i, 1.0f, half_i, 0.0f, ri);
        edge_at(t, j, 1.0f, half_j, 0.0f, rj);
        const float sea = sea_level(t);
        float* road[4] = {li, lj, ri, rj};
        for (float* p : road) if (p[1] < sea) p[1] = sea;
        float here;
        if (quad_height(li, lj, rj, ri, px, pz, here)) {
            if (!any || here > y) { y = here; any = true; }
        }
        for (int side = 0; side < 2; ++side) {
            const float sgn = side ? 1.0f : -1.0f;
            GroundBand gi, gj;
            ground_band(t, i, sgn, gi);
            ground_band(t, j, sgn, gj);
            // The same two quads draw_road draws, whether that is a bank and a
            // plain or a canyon rim and the plateau above it. One construction,
            // so a test cannot be measuring a world the player never sees.
            if (quad_height(gi.lip, gj.lip, gj.shoulder, gi.shoulder, px, pz, here)
                || quad_height(gi.shoulder, gj.shoulder, gj.plain, gi.plain,
                               px, pz, here)) {
                if (!any || here > y) { y = here; any = true; }
            }
        }
    }

    g_origin[0] = saved[0]; g_origin[1] = saved[1]; g_origin[2] = saved[2];
    return any;
}

}  // namespace twinflare
