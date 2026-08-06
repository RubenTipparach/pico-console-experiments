#include "pse/renderer3d.hpp"

#include <cmath>

namespace pse {
namespace {

constexpr float k_pi = 3.14159265f;
constexpr float k_default_z_near = 0.25f;
constexpr float k_default_z_far = 400.0f;
constexpr float k_default_fov_degrees = 90.0f;

// Flat shading direction. Baked into the engine because a game that wants a
// different light should pass different vertex colours, not reconfigure the
// renderer.
constexpr float k_light_x = 0.40f;
constexpr float k_light_y = 0.82f;
constexpr float k_light_z = -0.40f;
constexpr float k_ambient = 0.45f;

inline int32_t to_fixed(float value) {
    return static_cast<int32_t>(value * k_fixed_one);
}

inline int clamp_int(int value, int low, int high) {
    if (value < low) return low;
    if (value > high) return high;
    return value;
}

inline uint8_t shade(uint8_t channel, float intensity) {
    const int lit = static_cast<int>(channel * intensity);
    return static_cast<uint8_t>(clamp_int(lit, 0, 255));
}

void multiply(const float a[4][4], const float b[4][4], float out[4][4]) {
    for (int row = 0; row < 4; row++) {
        for (int col = 0; col < 4; col++) {
            float sum = 0.0f;
            for (int k = 0; k < 4; k++) sum += a[row][k] * b[k][col];
            out[row][col] = sum;
        }
    }
}

// Unit cube, origin at the centre of its base so boxes sit on the ground.
const float k_box_vertices[8][3] = {
    {-0.5f, 0.0f, -0.5f}, {0.5f, 0.0f, -0.5f},
    {0.5f, 1.0f, -0.5f},  {-0.5f, 1.0f, -0.5f},
    {-0.5f, 0.0f, 0.5f},  {0.5f, 0.0f, 0.5f},
    {0.5f, 1.0f, 0.5f},   {-0.5f, 1.0f, 0.5f},
};

// The two end faces are wound the other way round from the four walls, and
// that is not a style choice: written the same way as the walls they face
// INWARD, the backface cull drops them, and a box viewed from above shows the
// ground through the hole where its lid should be. It shipped that way, and
// the reason nobody caught it is that the lid is only missing from the side
// you normally look from: draw_box's top_r/g/b was reachable exclusively by a
// camera underneath the box, which is to say never. test_engine.cpp now
// renders one from above and checks the top colour actually lands on screen.
const uint8_t k_box_faces[6][4] = {
    {0, 3, 2, 1}, {5, 6, 7, 4}, {4, 7, 3, 0},
    {1, 2, 6, 5}, {2, 6, 7, 3}, {5, 1, 0, 4},
};

constexpr int k_face_top = 4;
constexpr int k_face_bottom = 5;

// Per side brightness, so a box reads as solid without a real lighting pass.
const float k_face_shade[6] = {0.70f, 0.90f, 0.60f, 1.00f, 1.00f, 0.50f};

}  // namespace

void Renderer3D::set_depth_range(float near_plane, float far_plane) {
    if (near_plane < 0.05f) near_plane = 0.05f;
    if (far_plane < near_plane * 1.5f) far_plane = near_plane * 1.5f;
    z_near_ = near_plane;
    z_far_ = far_plane;
    rebuild_view_projection();
}

void Renderer3D::set_fov(float degrees) {
    if (degrees < 1.0f) degrees = 1.0f;
    if (degrees > 175.0f) degrees = 175.0f;
    fov_degrees_ = degrees;
    rebuild_view_projection();
}

void Renderer3D::set_camera(float x, float y, float z, float yaw, float pitch) {
    camera_x_ = x;
    camera_y_ = y;
    camera_z_ = z;
    camera_yaw_ = yaw;
    camera_pitch_ = pitch;
    rebuild_view_projection();
}

void Renderer3D::set_orbit_camera(float target_x, float target_y, float target_z,
                                  float yaw, float distance, float height,
                                  float look_lift) {
    const float x = target_x - sinf(yaw) * distance;
    const float y = target_y + height;
    const float z = target_z - cosf(yaw) * distance;

    const float dx = target_x - x;
    const float dy = (target_y + look_lift) - y;
    const float dz = target_z - z;

    camera_x_ = x;
    camera_y_ = y;
    camera_z_ = z;
    camera_yaw_ = atan2f(dx, dz);
    camera_pitch_ = atan2f(dy, sqrtf(dx * dx + dz * dz));
    rebuild_view_projection();
}

void Renderer3D::rebuild_view_projection() {
    // Standard perspective divide: focal = 1 / tan(fov / 2).
    const float half_fov = (fov_degrees_ * 0.5f) * k_pi / 180.0f;
    const float focal = 1.0f / tanf(half_fov);

    float projection[4][4] = {};
    projection[0][0] = focal;
    projection[1][1] = focal;
    projection[2][2] = -((z_far_ + z_near_) / (z_far_ - z_near_));
    projection[2][3] = -((2.0f * z_far_ * z_near_) / (z_far_ - z_near_));
    projection[3][2] = -1.0f;

    const float cos_pitch = cosf(camera_pitch_), sin_pitch = sinf(camera_pitch_);
    const float cos_yaw = cosf(camera_yaw_), sin_yaw = sinf(camera_yaw_);

    // Right handed basis. `forward` is where the camera looks, so a positive
    // pitch looks up and yaw 0 looks down +Z.
    const float forward[3] = {sin_yaw * cos_pitch, sin_pitch, cos_yaw * cos_pitch};
    const float right[3] = {cos_yaw, 0.0f, -sin_yaw};
    const float up[3] = {
        forward[1] * right[2] - forward[2] * right[1],
        forward[2] * right[0] - forward[0] * right[2],
        forward[0] * right[1] - forward[1] * right[0],
    };
    const float eye[3] = {camera_x_, camera_y_, camera_z_};

    // View row 2 is -forward, which is what makes w = -view_z positive for
    // anything in front of the camera. Getting this sign wrong culls the entire
    // scene silently: every vertex fails the w > 0 test and the frame comes back
    // empty with no other symptom.
    float view[4][4] = {};
    for (int axis = 0; axis < 3; axis++) {
        view[0][axis] = right[axis];
        view[1][axis] = up[axis];
        view[2][axis] = -forward[axis];
    }
    view[0][3] = -(right[0] * eye[0] + right[1] * eye[1] + right[2] * eye[2]);
    view[1][3] = -(up[0] * eye[0] + up[1] * eye[1] + up[2] * eye[2]);
    view[2][3] = forward[0] * eye[0] + forward[1] * eye[1] + forward[2] * eye[2];
    view[3][3] = 1.0f;

    float combined[4][4];
    multiply(projection, view, combined);

    for (int row = 0; row < 4; row++) {
        for (int col = 0; col < 4; col++) {
            view_projection_[row][col] = to_fixed(combined[row][col]);
        }
    }
}

bool Renderer3D::project(float wx, float wy, float wz,
                         int& out_x, int& out_y, int& out_depth) const {
    const int32_t fx = to_fixed(wx);
    const int32_t fy = to_fixed(wy);
    const int32_t fz = to_fixed(wz);
    const auto& m = view_projection_;

    const int32_t w = (m[3][0] * fx + m[3][1] * fy + m[3][2] * fz +
                       m[3][3] * k_fixed_one) / k_fixed_one;
    if (w <= 0) return false;

    const int32_t cx = (m[0][0] * fx + m[0][1] * fy + m[0][2] * fz +
                        m[0][3] * k_fixed_one) / w;
    const int32_t cy = (m[1][0] * fx + m[1][1] * fy + m[1][2] * fz +
                        m[1][3] * k_fixed_one) / w;
    const int32_t cz = (m[2][0] * fx + m[2][1] * fy + m[2][2] * fz +
                        m[2][3] * k_fixed_one) / w;

    if (cz <= 0 || cz > k_fixed_one) return false;

    const int width = rasterizer_.target().width;

    out_x = (cx + k_fixed_one) * (width - 1) / k_fixed_one / 2;
    if (viewport_h_ > 0) {
        // A viewport band keeps the same pixels per NDC unit vertically as
        // horizontally, so shapes stay undistorted and the band is a crop of
        // the view rather than a squash of it.
        out_y = viewport_y0_ + viewport_h_ / 2 -
                (cy * (width - 1)) / k_fixed_one / 2;
    } else {
        const int height = rasterizer_.target().height;
        out_y = height - ((cy + k_fixed_one) * (height - 1)) / k_fixed_one / 2;
    }
    out_depth = cz;
    return true;
}

void Renderer3D::set_viewport(int y0, int height) {
    viewport_y0_ = y0;
    viewport_h_ = height;
}

void Renderer3D::set_depth_fade(bool enabled, uint8_t r, uint8_t g, uint8_t b,
                                float y_start, float y_end) {
    fade_enabled_ = enabled && y_end != y_start;
    fade_r_ = r;
    fade_g_ = g;
    fade_b_ = b;
    fade_y_start_ = y_start;
    fade_y_scale_ = fade_enabled_ ? 1.0f / (y_end - y_start) : 0.0f;
}

void Renderer3D::emit_quad(const int sx[8], const int sy[8], const int sz[8],
                           const bool visible[8], const uint8_t face[4],
                           uint8_t r, uint8_t g, uint8_t b,
                           bool highlight_edge) {
    if (!visible[face[0]] || !visible[face[1]] ||
        !visible[face[2]] || !visible[face[3]]) {
        return;
    }

    // A slight lift on two corners fakes a gradient across the face, which
    // stops large flat walls looking like paper.
    const uint8_t lift_r = static_cast<uint8_t>(clamp_int(r + 30, 0, 255));
    const uint8_t lift_g = static_cast<uint8_t>(clamp_int(g + 30, 0, 255));
    const uint8_t lift_b = static_cast<uint8_t>(clamp_int(b + 30, 0, 255));

    ScreenTriangle tri;
    const int i0 = face[0], i1 = face[1], i2 = face[2], i3 = face[3];

    tri.x0 = static_cast<int16_t>(sx[i0]);
    tri.y0 = static_cast<int16_t>(sy[i0]);
    tri.z0 = static_cast<uint16_t>(sz[i0]);
    tri.r0 = r; tri.g0 = g; tri.b0 = b;

    tri.x1 = static_cast<int16_t>(sx[i1]);
    tri.y1 = static_cast<int16_t>(sy[i1]);
    tri.z1 = static_cast<uint16_t>(sz[i1]);
    tri.r1 = highlight_edge ? lift_r : r;
    tri.g1 = highlight_edge ? lift_g : g;
    tri.b1 = highlight_edge ? lift_b : b;

    tri.x2 = static_cast<int16_t>(sx[i2]);
    tri.y2 = static_cast<int16_t>(sy[i2]);
    tri.z2 = static_cast<uint16_t>(sz[i2]);
    tri.r2 = highlight_edge ? lift_r : r;
    tri.g2 = highlight_edge ? lift_g : g;
    tri.b2 = highlight_edge ? lift_b : b;

    rasterizer_.draw(tri);

    tri.x1 = tri.x2;
    tri.y1 = tri.y2;
    tri.z1 = tri.z2;
    tri.r1 = tri.r2; tri.g1 = tri.g2; tri.b1 = tri.b2;

    tri.x2 = static_cast<int16_t>(sx[i3]);
    tri.y2 = static_cast<int16_t>(sy[i3]);
    tri.z2 = static_cast<uint16_t>(sz[i3]);
    tri.r2 = r; tri.g2 = g; tri.b2 = b;

    rasterizer_.draw(tri);
}

void Renderer3D::draw_box(float x, float y, float z,
                          float size_x, float size_y, float size_z,
                          uint8_t top_r, uint8_t top_g, uint8_t top_b,
                          uint8_t side_r, uint8_t side_g, uint8_t side_b) {
    int sx[8], sy[8], sz[8];
    bool visible[8];

    for (int i = 0; i < 8; i++) {
        visible[i] = project(x + k_box_vertices[i][0] * size_x,
                             y + k_box_vertices[i][1] * size_y,
                             z + k_box_vertices[i][2] * size_z,
                             sx[i], sy[i], sz[i]);
    }

    for (int face = 0; face < 6; face++) {
        uint8_t r, g, b;
        if (face == k_face_top) {
            r = top_r; g = top_g; b = top_b;
        } else {
            const float intensity = k_face_shade[face];
            r = shade(side_r, intensity);
            g = shade(side_g, intensity);
            b = shade(side_b, intensity);
        }
        const bool wall = face != k_face_top && face != k_face_bottom;
        emit_quad(sx, sy, sz, visible, k_box_faces[face], r, g, b, wall);
    }
}

void Renderer3D::draw_mesh(const MeshData& mesh,
                           float x, float y, float z,
                           float yaw, float scale,
                           uint8_t tint_r, uint8_t tint_g, uint8_t tint_b,
                           float pitch, uint8_t whiten, float roll,
                           uint8_t tex) {
    // The three angle form composes Ry(yaw) Rx(pitch) Rz(roll) into the basis
    // and hands over. One implementation, so the two forms cannot disagree
    // about what an orientation means, and the angles keep the exact meaning
    // they had when this did its own per vertex trig.
    const float sy = sinf(yaw), cy = cosf(yaw);
    const float sp = sinf(pitch), cp = cosf(pitch);
    const float sr = sinf(roll), cr = cosf(roll);

    Basis basis;
    basis.m[0] = cr * cy - sr * sp * sy;
    basis.m[1] = -sr * cy - cr * sp * sy;
    basis.m[2] = cp * sy;
    basis.m[3] = sr * cp;
    basis.m[4] = cr * cp;
    basis.m[5] = sp;
    basis.m[6] = -cr * sy - sr * sp * cy;
    basis.m[7] = sr * sy - cr * sp * cy;
    basis.m[8] = cp * cy;

    draw_mesh(mesh, x, y, z, basis, scale, tint_r, tint_g, tint_b, whiten, tex);
}

void Renderer3D::draw_mesh(const MeshData& mesh,
                           float x, float y, float z,
                           const Basis& basis, float scale,
                           uint8_t tint_r, uint8_t tint_g, uint8_t tint_b,
                           uint8_t whiten, uint8_t tex) {
    if (mesh.vertices == nullptr || mesh.faces == nullptr) return;
    if (mesh.scale <= 0) return;

    const float* m = basis.m;
    const float unit = scale / static_cast<float>(mesh.scale);
    // A texture index means nothing without coordinates to sample at, so a
    // mesh that carried no `vt` draws untextured however it is called. Silent
    // rather than an error: it is the same mesh either way, and a model
    // swapped for an untextured one should lose its picture, not its picture
    // and the frame it was in.
    const uint8_t use_tex = mesh.uvs != nullptr ? tex : 0;

    for (uint16_t f = 0; f < mesh.face_count; f++) {
        const MeshFace& face = mesh.faces[f];
        if (face.i0 >= mesh.vertex_count ||
            face.i1 >= mesh.vertex_count ||
            face.i2 >= mesh.vertex_count) {
            continue;
        }

        // The baked normal turns with the model, through the same basis, so
        // lighting follows the hull. A rotation preserves lengths and angles,
        // so the vertex basis is the correct normal basis too, with no inverse
        // transpose needed.
        const float base_nx = face.nx / 127.0f;
        const float base_ny = face.ny / 127.0f;
        const float base_nz = face.nz / 127.0f;
        const float world_nx = m[0] * base_nx + m[1] * base_ny + m[2] * base_nz;
        const float ny = m[3] * base_nx + m[4] * base_ny + m[5] * base_nz;
        const float world_nz = m[6] * base_nx + m[7] * base_ny + m[8] * base_nz;

        float lambert = world_nx * k_light_x + ny * k_light_y +
                        world_nz * k_light_z;
        if (lambert < 0.0f) lambert = 0.0f;
        const float intensity = k_ambient + (1.0f - k_ambient) * lambert;

        // Tint multiplies, then whiten lerps toward white, then the lambert
        // shade. Whitening before the shade rather than after keeps a flashed
        // face lit like every other face instead of going flat.
        uint8_t r = static_cast<uint8_t>(face.r * tint_r / 255);
        uint8_t g = static_cast<uint8_t>(face.g * tint_g / 255);
        uint8_t b = static_cast<uint8_t>(face.b * tint_b / 255);
        if (whiten) {
            r = static_cast<uint8_t>(r + (255 - r) * whiten / 255);
            g = static_cast<uint8_t>(g + (255 - g) * whiten / 255);
            b = static_cast<uint8_t>(b + (255 - b) * whiten / 255);
        }
        r = shade(r, intensity);
        g = shade(g, intensity);
        b = shade(b, intensity);

        const uint16_t indices[3] = {face.i0, face.i1, face.i2};
        int px[3], py[3], pz[3];
        float wy[3];
        bool all_visible = true;

        for (int corner = 0; corner < 3; corner++) {
            const MeshVertex& v = mesh.vertices[indices[corner]];
            const float lx = v.x * unit;
            const float ly = v.y * unit;
            const float lz = v.z * unit;
            const float wx = x + m[0] * lx + m[1] * ly + m[2] * lz;
            const float wz = z + m[6] * lx + m[7] * ly + m[8] * lz;
            wy[corner] = y + m[3] * lx + m[4] * ly + m[5] * lz;
            if (!project(wx, wy[corner], wz,
                         px[corner], py[corner], pz[corner])) {
                all_visible = false;
                break;
            }
        }
        if (!all_visible) continue;

        ScreenTriangle tri;
        tri.x0 = static_cast<int16_t>(px[0]);
        tri.y0 = static_cast<int16_t>(py[0]);
        tri.z0 = static_cast<uint16_t>(pz[0]);
        tri.x1 = static_cast<int16_t>(px[1]);
        tri.y1 = static_cast<int16_t>(py[1]);
        tri.z1 = static_cast<uint16_t>(pz[1]);
        tri.x2 = static_cast<int16_t>(px[2]);
        tri.y2 = static_cast<int16_t>(py[2]);
        tri.z2 = static_cast<uint16_t>(pz[2]);
        tri.r0 = tri.r1 = tri.r2 = r;
        tri.g0 = tri.g1 = tri.g2 = g;
        tri.b0 = tri.b1 = tri.b2 = b;

        tri.tex = use_tex;
        if (use_tex != 0) {
            const MeshUv& uv = mesh.uvs[f];
            tri.u0 = uv.u0; tri.v0 = uv.v0;
            tri.u1 = uv.u1; tri.v1 = uv.v1;
            tri.u2 = uv.u2; tri.v2 = uv.v2;
        }

        if (fade_enabled_) {
            uint8_t* cr[3] = {&tri.r0, &tri.r1, &tri.r2};
            uint8_t* cg[3] = {&tri.g0, &tri.g1, &tri.g2};
            uint8_t* cb[3] = {&tri.b0, &tri.b1, &tri.b2};
            for (int corner = 0; corner < 3; corner++) {
                float t = (wy[corner] - fade_y_start_) * fade_y_scale_;
                if (t < 0.0f) t = 0.0f;
                if (t > 1.0f) t = 1.0f;
                const int ti = static_cast<int>(t * 256.0f);
                *cr[corner] = static_cast<uint8_t>(r + (fade_r_ - r) * ti / 256);
                *cg[corner] = static_cast<uint8_t>(g + (fade_g_ - g) * ti / 256);
                *cb[corner] = static_cast<uint8_t>(b + (fade_b_ - b) * ti / 256);
            }
        }

        rasterizer_.draw(tri);
    }
}

bool Renderer3D::project_billboard(float wx, float wy, float wz,
                                   float world_size, int& out_x, int& out_y,
                                   float& out_scale, uint8_t& out_depth) const {
    int depth = 0;
    if (!project(wx, wy, wz, out_x, out_y, depth)) return false;

    const int width = rasterizer_.target().width;
    const int height = rasterizer_.target().height;
    const int margin = 50;
    if (out_x < -margin || out_x >= width + margin) return false;
    if (out_y < -margin || out_y >= height + margin) return false;

    const float dx = wx - camera_x_;
    const float dy = wy - camera_y_;
    const float dz = wz - camera_z_;
    const float distance = sqrtf(dx * dx + dy * dy + dz * dz);
    if (distance < 0.5f) return false;

    // The 40 was tuned against the default 90 degree lens, where the focal
    // length is 1. Scaling it by the current focal keeps a billboard the same
    // size as the geometry around it at any field of view, and leaves every
    // game that never touches the lens byte for byte where it was.
    const float half_fov = (fov_degrees_ * 0.5f) * k_pi / 180.0f;
    out_scale = world_size * 40.0f / tanf(half_fov) / distance;
    if (out_scale < 0.5f) return false;

    const int scaled = depth * 255 / k_fixed_one;
    out_depth = static_cast<uint8_t>(clamp_int(scaled, 0, 255));
    return true;
}

}  // namespace pse
