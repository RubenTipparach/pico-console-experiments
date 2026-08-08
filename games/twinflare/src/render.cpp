#include "render.hpp"

#include <cmath>

#include "pse/parallel.hpp"
#include "pse/raster.hpp"
#include "pse/renderer3d.hpp"
#include "pse/shared_render.hpp"
#include "pse/text.hpp"
#include "fixed.hpp"
#include "twinflare/cockpit.hpp"
#include "twinflare/engine_heavy.hpp"
#include "twinflare/engine_slim.hpp"
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

// A quad in world space, flat shaded off its own normal. The engine has no
// call for this: draw_box is axis aligned and draw_mesh wants a MeshData, so
// the road builds its own ScreenTriangles exactly as Dust Rider's push_quad
// does.
void quad(const float p0[3], const float p1[3], const float p2[3],
          const float p3[3], const uint8_t colour[3]) {
    int sx[4], sy[4], sz[4];
    const float* pts[4] = {p0, p1, p2, p3};
    for (int i = 0; i < 4; ++i) {
        if (!g_renderer->project(pts[i][0], pts[i][1], pts[i][2], sx[i], sy[i], sz[i])) {
            // Dropped whole, because the engine drops it whole anyway. This is
            // why the road is short strips: a long quad running from under the
            // nose to the horizon vanishes the instant its near corner crosses
            // the plane, and Dust Rider's comment already says the desert did
            // exactly that.
            return;
        }
    }
    float n[3];
    normal_of(p0, p1, p2, n);
    const Rgb c = shade(colour, n[0], n[1], n[2]);
    pse::ScreenTriangle tri{};
    tri.r0 = tri.r1 = tri.r2 = c.r;
    tri.g0 = tri.g1 = tri.g2 = c.g;
    tri.b0 = tri.b1 = tri.b2 = c.b;
    // Wound to face the camera, decided per triangle from the screen space
    // signed area rather than assumed from the source order.
    //
    // Rasterizer::draw culls backfaces, and the first version of this handed
    // it whichever order the caller happened to write the corners in: every
    // road quad and every horizon column was culled, so the game rendered a
    // sky, a pod and two rocks, with no ground under any of it. The pod
    // survived because draw_mesh sorts its own winding out.
    //
    // Flipping rather than fixing the callers is also correct here and not
    // just convenient: the road is a surface a pod can end up underneath
    // after falling through a gap, and a one sided road would vanish from
    // below at exactly the moment the player wants to see where it went.
    const auto put = [&](int a, int b, int d) {
        // Rasterizer::rejected's own expression, character for character, and
        // that matters: written the other way round it is the NEGATIVE of the
        // engine's, so "flip when this is backfacing" flipped exactly the
        // triangles that were already facing the right way and the road went
        // from partly missing to entirely missing.
        const long area = static_cast<long>(sx[d] - sx[a]) * (sy[b] - sy[a])
                        - static_cast<long>(sy[d] - sy[a]) * (sx[b] - sx[a]);
        if (area == 0) return;
        if (area < 0) { const int t = b; b = d; d = t; }
        tri.x0 = static_cast<int16_t>(sx[a]); tri.y0 = static_cast<int16_t>(sy[a]);
        tri.x1 = static_cast<int16_t>(sx[b]); tri.y1 = static_cast<int16_t>(sy[b]);
        tri.x2 = static_cast<int16_t>(sx[d]); tri.y2 = static_cast<int16_t>(sy[d]);
        tri.z0 = static_cast<uint16_t>(sz[a]);
        tri.z1 = static_cast<uint16_t>(sz[b]);
        tri.z2 = static_cast<uint16_t>(sz[d]);
        g_renderer->rasterizer().draw(tri);
    };
    put(0, 1, 2);
    put(0, 2, 3);
}

// ---- the road ---------------------------------------------------------------

// Eighteen segments ahead and three behind. Each is ten to fourteen triangles,
// so this is where the frame's ground budget goes, and it is spent on the one
// thing that tells the player where the track goes.
constexpr int k_view_segments = 18;
constexpr int k_view_behind = 3;

void edge_point(const Track& t, int index, float side, float out[3], float widen = 1.0f) {
    const TrackNode& a = t.nodes[index];
    const TrackNode& b = t.nodes[(index + 1) % t.node_count];
    const float ax = to_world(node_x(a)), az = to_world(node_z(a));
    const float bx = to_world(node_x(b)), bz = to_world(node_z(b));
    float dx = bx - ax, dz = bz - az;
    const float m = std::sqrt(dx * dx + dz * dz);
    if (m > 0.0001f) { dx /= m; dz /= m; }
    const float half = to_world(node_half_width(a)) * widen;
    out[0] = ax + dz * half * side;
    out[1] = to_world(node_y(a));
    out[2] = az - dx * half * side;
}

void draw_road(const Track& t, const Pod& pod) {
    const Palette& pal = t.palette;
    for (int s = -k_view_behind; s < k_view_segments; ++s) {
        const int i = ((pod.node + s) % t.node_count + t.node_count) % t.node_count;
        const int j = (i + 1) % t.node_count;
        const TrackNode& a = t.nodes[i];
        const int band = (i >> 1) & 1;
        const bool near = s <= 11;

        float li[3], lj[3], ri[3], rj[3];
        edge_point(t, i, -1.0f, li);
        edge_point(t, j, -1.0f, lj);
        edge_point(t, i, 1.0f, ri);
        edge_point(t, j, 1.0f, rj);

        // Ground and shoulders only where they can still be seen. Past about
        // ninety units out the shoulder is under a pixel tall and the ground
        // beyond it is the same few pixels as the horizon wall behind it, so
        // six of a far segment's fourteen triangles were drawing the horizon a
        // second time.
        if (near) {
            float wi[3], wj[3], fi[3], fj[3];
            for (int side = 0; side < 2; ++side) {
                const float sgn = side ? 1.0f : -1.0f;
                edge_point(t, i, sgn, wi, 1.35f);
                edge_point(t, j, sgn, wj, 1.35f);
                edge_point(t, i, sgn, fi, 4.0f);
                edge_point(t, j, sgn, fj, 4.0f);
                const float drop = (a.flags & kWall) ? 4.0f : -2.2f;
                fi[1] += drop; fj[1] += drop;
                wi[1] += (a.flags & kWall) ? 4.0f : -0.74f;
                wj[1] += (a.flags & kWall) ? 4.0f : -0.74f;
                if (side) quad(wi, wj, fj, fi, pal.ground[band]);
                else      quad(fi, fj, wj, wi, pal.ground[band]);
                // Walls get a colour of their own rather than borrowing the
                // scenery's: a near vertical face takes the ambient floor and
                // nothing else, so a wall painted from the same palette entry
                // as a distant rock comes out three times darker than it, and
                // the track that is mostly wall came out a black corridor.
                const uint8_t* side_col = (a.flags & kWall) ? pal.wall : pal.rock[band];
                if (side) quad(ri, rj, wj, wi, side_col);
                else      quad(wi, wj, lj, li, side_col);
            }
        }

        if (a.flags & kGap) continue;   // the jump: no road at all

        const uint8_t* col = pal.road[band];
        uint8_t boost_col[3] = {90, 190, 255};
        if (a.flags & kBoost) col = boost_col;

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
            edge_point(t, i, -1.0f + k_lip, ai);
            edge_point(t, j, -1.0f + k_lip, aj);
            edge_point(t, i, 1.0f - k_lip, bi);
            edge_point(t, j, 1.0f - k_lip, bj);
            quad(li, lj, aj, ai, pal.edge);
            quad(ai, aj, bj, bi, col);
            quad(bi, bj, rj, ri, pal.edge);
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
        const float base = cam.y - 26.0f;
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
            const float p0[3] = {cam.x + dx * w.depth + rx * t0, base, cam.z + dz * w.depth + rz * t0};
            const float p1[3] = {p0[0], base + h0, p0[2]};
            const float p3[3] = {cam.x + dx * w.depth + rx * t1, base, cam.z + dz * w.depth + rz * t1};
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
        g_renderer->draw_mesh(models::twinflare::rock, p[0], p[1] + 1.0f, p[2],
                              static_cast<float>(h & 255) * 0.024f, scale,
                              col[0], col[1], col[2]);
    }
}

// ---- pods ---------------------------------------------------------------------

struct PodPose {
    float x, y, z;
    float yaw, pitch, roll, swing;
    uint8_t racer;
    uint8_t dead;
    int32_t engine[2];
    int32_t engine_max;
    bool boosting;
    float throttle;
};

// A ribbon that always faces the camera, for the cables and the binder arc. A
// tube this thin is one pixel wide, so a tube's worth of triangles buys
// nothing.
void ribbon(const float a[3], const float b[3], const Camera& cam, float width,
            const uint8_t col[3]) {
    float d[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]};
    const float vx = a[0] - cam.x, vy = a[1] - cam.y, vz = a[2] - cam.z;
    float n[3] = {d[1] * vz - d[2] * vy, d[2] * vx - d[0] * vz, d[0] * vy - d[1] * vx};
    const float m = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
    if (m < 0.0001f) return;
    for (float& v : n) v = v / m * width;
    const float p0[3] = {a[0] - n[0], a[1] - n[1], a[2] - n[2]};
    const float p1[3] = {a[0] + n[0], a[1] + n[1], a[2] + n[2]};
    const float p2[3] = {b[0] + n[0], b[1] + n[1], b[2] + n[2]};
    const float p3[3] = {b[0] - n[0], b[1] - n[1], b[2] - n[2]};
    quad(p0, p1, p2, p3, col);
    quad(p3, p2, p1, p0, col);   // both faces: a ribbon has no outside
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
    out[0] = p.x + m[0] * dx + m[1] * dy + m[2] * dz;
    out[1] = p.y + m[3] * dx + m[4] * dy + m[5] * dz;
    out[2] = p.z + m[6] * dx + m[7] * dy + m[8] * dz;
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
    const pse::MeshData& engine_mesh = rc.heavy ? models::twinflare::engine_heavy
                                                : models::twinflare::engine_slim;
    float left[3], right[3], cockpit[3], top[3];
    local_point(p, -k_engine_x, k_engine_y, k_engine_z, left);
    local_point(p, k_engine_x, k_engine_y, k_engine_z, right);
    // The cockpit sits where the SWING put it, not where the hull points. This
    // is the only place the two mass model shows and it is the whole reason
    // for it: through a corner the cockpit is visibly off to one side of the
    // line between the engines.
    local_point(p, p.swing * 2.6f, -0.25f, k_pod_z - std::fabs(p.swing) * 0.5f, cockpit);
    local_point(p, 0.0f, 0.30f, k_pod_z + 0.9f, top);

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
        g_renderer->draw_mesh(models::twinflare::cockpit, cockpit[0], cockpit[1],
                              cockpit[2], p.yaw, 1.0f,
                              rc.colour[0][0], rc.colour[0][1], rc.colour[0][2],
                              p.pitch, 0, p.roll);
        const uint8_t cable[3] = {58, 62, 72};
        float ea[3], eb[3];
        local_point(p, -k_engine_x * 0.75f, k_engine_y - 0.3f, k_engine_z - 1.2f, ea);
        local_point(p, k_engine_x * 0.75f, k_engine_y - 0.3f, k_engine_z - 1.2f, eb);
        if (!(p.dead & 1)) ribbon(top, ea, cam, 0.09f, cable);
        if (!(p.dead & 2)) ribbon(top, eb, cam, 0.09f, cable);

        // The energy binder: the arc of plasma strung between the engines, and
        // the one piece of a podracer everybody can draw from memory. It goes
        // out the moment either engine does, which is the clearest possible
        // signal that the pod is in trouble.
        if (p.dead == 0) {
            const uint8_t glow[3] = {236, 150, 255};
            float prev[3];
            for (int k = 0; k <= 4; ++k) {
                const float u = k / 4.0f;
                float arc[3];
                local_point(p, (-1.0f + 2.0f * u) * k_engine_x * 0.92f,
                            k_engine_y + std::sin(u * 3.14159f) * 0.55f,
                            k_engine_z - 1.5f, arc);
                if (k) ribbon(prev, arc, cam, 0.07f, glow);
                for (int c = 0; c < 3; ++c) prev[c] = arc[c];
            }
        }
    }

    for (int i = 0; i < 2; ++i) {
        if ((p.dead >> i) & 1) continue;
        const float wear = p.engine_max ? static_cast<float>(p.engine[i]) / p.engine_max : 1.0f;
        const uint8_t tint = static_cast<uint8_t>(140 + 115 * wear);
        const float* at = i ? right : left;
        g_renderer->draw_mesh(engine_mesh, at[0], at[1], at[2], p.yaw, 1.0f,
                              static_cast<uint8_t>(rc.colour[0][0] * tint / 255),
                              static_cast<uint8_t>(rc.colour[0][1] * tint / 255),
                              static_cast<uint8_t>(rc.colour[0][2] * tint / 255),
                              p.pitch, p.boosting ? 90 : 0, p.roll);
    }
}

PodPose pose_of(const Pod& pod) {
    PodPose p{};
    p.x = to_world(pod.x); p.y = to_world(pod.y); p.z = to_world(pod.z);
    p.yaw = to_rad(pod.yaw); p.pitch = to_rad(pod.pitch); p.roll = to_rad(pod.roll);
    p.swing = to_rad(pod.swing);
    p.racer = pod.racer_index;
    p.dead = pod.dead;
    p.engine[0] = pod.engine[0]; p.engine[1] = pod.engine[1];
    p.engine_max = pod.engine_max;
    p.boosting = pod.boost_ticks > 0;
    return p;
}

PodPose pose_of(const Rival& r) {
    PodPose p{};
    p.x = to_world(r.x); p.y = to_world(r.y); p.z = to_world(r.z);
    p.yaw = to_rad(r.yaw); p.roll = to_rad(r.roll);
    p.racer = r.racer_index;
    p.engine[0] = p.engine[1] = 1000;
    p.engine_max = 1000;
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

void draw_pod_select(const Chrome& chrome, const pse::RenderTarget& target) {
    const Racer& rc = racer(chrome.pod);
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
    if (chrome.screen == Screen::PodSelect || chrome.screen == Screen::TrackSelect
        || chrome.screen == Screen::Results) {
        raster.begin_frame(target);
        raster.clear_gradient(18, 20, 32, 10, 11, 18);
        if (chrome.screen == Screen::PodSelect) draw_pod_select(chrome, target);
        else if (chrome.screen == Screen::TrackSelect) {
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

    queue.reset();
    raster.begin_frame_collect(target, queue);

    const Camera cam = follow(race.pod, 1.0f / 60.0f);
    renderer.set_depth_range(k_near, k_far);
    renderer.set_fov(k_fov);
    renderer.set_camera_basis(cam.x, cam.y, cam.z, camera_basis(cam));

    draw_horizon(t, cam);
    draw_road(t, race.pod);
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
        draw_pod(pose_of(r), lod, cam);
    }

    PodPose mine = pose_of(race.pod);
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

}  // namespace twinflare
