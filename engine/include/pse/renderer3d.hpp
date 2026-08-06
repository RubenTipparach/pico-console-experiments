#pragma once

#include <cstdint>

#include "pse/config.hpp"
#include "pse/mesh.hpp"
#include "pse/quat.hpp"
#include "pse/raster.hpp"

namespace pse {

// Transforms world space geometry into screen triangles and hands them to a
// Rasterizer. It does the projection and nothing else: it does not own pixels,
// it does not know what an SDK is, and it holds no game state.
//
// Float maths appears only in the per frame camera setup, which happens once.
// Everything per vertex is fixed point, because the RP2040 has no FPU.
class Renderer3D {
public:
    explicit Renderer3D(Rasterizer& rasterizer) : rasterizer_(rasterizer) {}

    Renderer3D(const Renderer3D&) = delete;
    Renderer3D& operator=(const Renderer3D&) = delete;

    // Place the camera behind and above a target, looking at it.
    //
    // The aim point sits look_lift above the target rather than on it, which
    // is what drops a followed subject into the lower half of the frame and
    // leaves the scene ahead of it in shot. That is the framing a chase camera
    // wants and the reason the default is a whole unit; a camera studying one
    // object wants 0, or the object hangs low enough to leave the frame.
    void set_orbit_camera(float target_x, float target_y, float target_z,
                          float yaw, float distance, float height,
                          float look_lift = 1.0f);

    // Place the camera explicitly.
    void set_camera(float x, float y, float z, float yaw, float pitch);

    // Near and far clip. The default 0.25 to 400 covers anything, and that is
    // the problem: the depth buffer is one byte, a perspective depth curve
    // spends nearly all of its resolution near the near plane, and a scene
    // sitting 40 units out lands inside a single step of it. A whole tree
    // then ties with the ground it stands on, and ties go to whoever drew
    // first, so the tree vanishes. Bracket the range to what a scene actually
    // occupies and the same tree separates cleanly.
    void set_depth_range(float near_plane, float far_plane);

    // Vertical field of view in degrees. The default is 90, which is a very
    // wide lens: it makes the bottom of the frame look 45 degrees further down
    // than the middle, so a camera pitched for a three quarter view still
    // reads as top down near the player. A game that wants the flat, even
    // perspective of a handheld role playing game wants a long lens instead,
    // and this is the only knob that gets it. Costs one matrix rebuild.
    void set_fov(float degrees);

    // Project into a horizontal band of the target instead of the full frame,
    // for split screen scenes: y0 is the band's first row, height its row
    // count. Horizontal extent stays the full width, so a half height band
    // simply sees a wider, letterboxed view. Pass height 0 to go back to the
    // full frame.
    void set_viewport(int y0, int height);

    // Fade each vertex colour toward a fog colour by its world space y, from
    // no fade at y_start to full fade at y_end. This is what makes underwater
    // geometry read as submerged: a fish nosing down shades darker along its
    // own body. Disabled by default and reset explicitly, never per draw.
    void set_depth_fade(bool enabled, uint8_t r, uint8_t g, uint8_t b,
                        float y_start = 0.0f, float y_end = -1.0f);

    // World point to screen. Returns false when the point is behind the near
    // plane or beyond the far plane.
    bool project(float wx, float wy, float wz,
                 int& out_x, int& out_y, int& out_depth) const;

    // Axis aligned box, with a distinct top colour and simple face shading.
    void draw_box(float x, float y, float z,
                  float size_x, float size_y, float size_z,
                  uint8_t top_r, uint8_t top_g, uint8_t top_b,
                  uint8_t side_r, uint8_t side_g, uint8_t side_b);

    // A model from tools/obj2cpp.py, rotated about Y and uniformly scaled.
    // Faces are lit flat from a fixed direction using the baked normal.
    //
    // The tint multiplies the model's own face colours, so one mesh in flash
    // can serve every colour variant a game needs. 255 leaves a channel alone.
    //
    // Pitch rotates about the model's local X axis, applied before yaw, so it
    // tilts the model about its own lateral axis whichever way yaw points it.
    // Positive pitch lifts the +Z nose of the model. It trails the tints so
    // every existing call keeps its meaning; a caller that wants pitch with
    // default tints spells the tints out.
    // `tint` multiplies the mesh's own face colours, so it can darken and
    // recolour but never brighten. `whiten` lerps the result toward white
    // afterwards, 0 for none and 255 for fully white, which is what a hit
    // flash needs and what draw_sprite has always had.
    //
    // Roll rotates about the model's local Z axis and is applied FIRST, inside
    // pitch and yaw, so the composed rotation is Ry(yaw) Rx(pitch) Rz(roll):
    // the model banks about its own nose line whichever way the other two have
    // pointed it. A craft that tilts on two axes at once needs all three, and
    // an aircraft that pitches and rolls in the wrong order visibly yaws when
    // it should not. It trails every existing parameter so no current call
    // changes meaning, and it costs one more sin and cos per draw_mesh, once,
    // not per vertex.
    void draw_mesh(const MeshData& mesh,
                   float x, float y, float z,
                   float yaw, float scale,
                   uint8_t tint_r = 255, uint8_t tint_g = 255,
                   uint8_t tint_b = 255, float pitch = 0.0f,
                   uint8_t whiten = 0, float roll = 0.0f);

    // The same draw, given the orientation as a basis instead of three angles.
    //
    // Three angles cannot describe an orientation that was reached by turning
    // about the body's own axes, which is what anything driven by thrusters
    // does. Composing them always names a frame that one of the other angles
    // has already moved, so the axes drift apart from the body's real ones,
    // and at a quarter turn a whole degree of freedom disappears. Pass the
    // basis and none of that arises: it is the orientation itself rather than
    // a recipe for reaching it. pse::quat_basis turns a quaternion into one.
    //
    // Cheaper per draw as well, not just more correct: the three angle form
    // spends six trig calls building this same basis before it can start.
    void draw_mesh(const MeshData& mesh,
                   float x, float y, float z,
                   const Basis& basis, float scale,
                   uint8_t tint_r = 255, uint8_t tint_g = 255,
                   uint8_t tint_b = 255, uint8_t whiten = 0);

    // Screen position and pixel size for a camera facing sprite. Returns false
    // when the point is off screen or behind the camera. The caller draws the
    // sprite itself, which keeps sprite styles out of the engine.
    bool project_billboard(float wx, float wy, float wz, float world_size,
                           int& out_x, int& out_y, float& out_scale,
                           uint8_t& out_depth) const;

    Rasterizer& rasterizer() { return rasterizer_; }

    void camera_position(float& x, float& y, float& z) const {
        x = camera_x_; y = camera_y_; z = camera_z_;
    }

private:
    void rebuild_view_projection();

    void emit_quad(const int sx[8], const int sy[8], const int sz[8],
                   const bool visible[8], const uint8_t face[4],
                   uint8_t r, uint8_t g, uint8_t b, bool highlight_edge);

    Rasterizer& rasterizer_;

    float camera_x_ = 0.0f, camera_y_ = 0.0f, camera_z_ = 0.0f;
    float camera_yaw_ = 0.0f, camera_pitch_ = 0.0f;
    float fov_degrees_ = 90.0f;
    float z_near_ = 0.25f;     // k_default_z_near in the .cpp
    float z_far_ = 400.0f;     // k_default_z_far

    int viewport_y0_ = 0;
    int viewport_h_ = 0;   // 0 means the full target height

    bool fade_enabled_ = false;
    uint8_t fade_r_ = 0, fade_g_ = 0, fade_b_ = 0;
    float fade_y_start_ = 0.0f;
    float fade_y_scale_ = 0.0f;   // 1 / (y_end - y_start)

    // View projection in fixed point, rebuilt once per camera change.
    int32_t view_projection_[4][4] = {};
};

}  // namespace pse
