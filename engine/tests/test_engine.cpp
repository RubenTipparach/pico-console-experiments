// Host side tests for the SDK free parts of the engine.
//
// These exist because the interesting failure modes here (winding, depth test,
// clipping, stride handling, fixed point overflow) are silent on hardware and
// expensive to chase with a device in hand. The engine deliberately has no SDK
// dependency, so all of it can be exercised with a plain host compiler.
//
// Built and run by CI on every push. No test framework: a failure prints the
// expression and the process exits non zero.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "pse/mesh.hpp"
#include "pse/pixel.hpp"
#include "pse/parallel.hpp"
#include "pse/raster.hpp"
#include "pse/renderer3d.hpp"

namespace {

int g_failures = 0;
int g_checks = 0;

void check(bool condition, const char* expression, const char* file, int line) {
    g_checks++;
    if (condition) return;
    g_failures++;
    std::printf("FAIL %s:%d: %s\n", file, line, expression);
}

#define CHECK(expr) check((expr), #expr, __FILE__, __LINE__)

// A host stand in for a 32blit Surface, in each of the formats the adapter can
// hand us.
class TestSurface {
public:
    TestSurface(int width, int height, pse::PixelFormat format)
        : width_(width), height_(height), format_(format),
          bytes_(bytes_per_pixel(format)),
          storage_(static_cast<size_t>(width) * height * bytes_per_pixel(format), 0) {}

    pse::RenderTarget target() {
        return {storage_.data(), width_, height_, width_ * bytes_, format_};
    }

    void pixel(int x, int y, int& r, int& g, int& b) const {
        const uint8_t* p = storage_.data() +
                           (static_cast<size_t>(y) * width_ + x) * bytes_;
        if (format_ == pse::PixelFormat::rgb565) {
            // Unpacked the way the SDK does it, red in the low bits. See
            // pse/pixel.hpp: this used to mirror the engine's own packing
            // instead, which made a swapped red and blue invisible here and
            // obvious on the device.
            const uint16_t v = static_cast<uint16_t>(p[0] | (p[1] << 8));
            r = (v & 0x1F) << 3;
            g = ((v >> 5) & 0x3F) << 2;
            b = ((v >> 11) & 0x1F) << 3;
        } else {
            r = p[0];
            g = p[1];
            b = p[2];
        }
    }

    bool bytes_equal(const TestSurface& other) const {
        return storage_ == other.storage_;
    }

    bool any_pixel_set() const {
        for (uint8_t byte : storage_) {
            if (byte != 0) return true;
        }
        return false;
    }

    static int bytes_per_pixel(pse::PixelFormat format) {
        switch (format) {
            case pse::PixelFormat::rgb565: case pse::PixelFormat::bgr555: return 2;
            case pse::PixelFormat::rgb888: return 3;
            default: return 4;
        }
    }

private:
    int width_;
    int height_;
    pse::PixelFormat format_;
    int bytes_;
    std::vector<uint8_t> storage_;
};

pse::ScreenTriangle make_triangle(int16_t x0, int16_t y0, int16_t x1, int16_t y1,
                                  int16_t x2, int16_t y2, uint16_t z,
                                  uint8_t r, uint8_t g, uint8_t b) {
    pse::ScreenTriangle tri{};
    tri.x0 = x0; tri.y0 = y0;
    tri.x1 = x1; tri.y1 = y1;
    tri.x2 = x2; tri.y2 = y2;
    tri.z0 = tri.z1 = tri.z2 = z;
    tri.r0 = tri.r1 = tri.r2 = r;
    tri.g0 = tri.g1 = tri.g2 = g;
    tri.b0 = tri.b1 = tri.b2 = b;
    return tri;
}

// A front facing triangle must be filled, and its mirror image must not be.
// This is the test that catches a winding flip, which otherwise shows up as a
// model rendering inside out with no other symptom.
void test_winding_and_fill() {
    TestSurface surface(64, 64, pse::PixelFormat::rgb888);
    pse::Rasterizer raster;
    raster.begin_frame(surface.target());

    raster.draw(make_triangle(10, 10, 10, 50, 50, 10, 500, 255, 0, 0));
    CHECK(raster.triangles_drawn() == 1);

    int r = 0, g = 0, b = 0;
    surface.pixel(15, 15, r, g, b);
    CHECK(r == 255 && g == 0 && b == 0);

    // Reversed winding is a backface and must be culled.
    TestSurface back(64, 64, pse::PixelFormat::rgb888);
    pse::Rasterizer back_raster;
    back_raster.begin_frame(back.target());
    back_raster.draw(make_triangle(10, 10, 50, 10, 10, 50, 500, 255, 0, 0));
    CHECK(back_raster.triangles_drawn() == 0);
    CHECK(!back.any_pixel_set());
}

// A nearer triangle must overwrite a farther one, and never the reverse,
// regardless of submission order.
void test_depth_ordering() {
    for (int order = 0; order < 2; order++) {
        TestSurface surface(64, 64, pse::PixelFormat::rgb888);
        pse::Rasterizer raster;
        raster.begin_frame(surface.target());

        const auto far_tri = make_triangle(0, 0, 0, 60, 60, 0, 900, 0, 0, 255);
        const auto near_tri = make_triangle(0, 0, 0, 60, 60, 0, 100, 0, 255, 0);

        if (order == 0) {
            raster.draw(far_tri);
            raster.draw(near_tri);
        } else {
            raster.draw(near_tri);
            raster.draw(far_tri);
        }

        int r = 0, g = 0, b = 0;
        surface.pixel(5, 5, r, g, b);
        CHECK(g == 255 && b == 0);
    }
}

// Geometry far outside the surface must not write outside it. The guard values
// on either side of the buffer catch an overrun that clipping missed.
void test_offscreen_clipping() {
    const int width = 32, height = 32;
    std::vector<uint8_t> canvas(3 * width * height + 32, 0xAA);
    pse::RenderTarget target{canvas.data() + 16, width, height, width * 3,
                             pse::PixelFormat::rgb888};

    pse::Rasterizer raster;
    raster.begin_frame(target);
    raster.draw(make_triangle(-500, -500, -500, 500, 500, -500, 400, 1, 2, 3));
    raster.draw(make_triangle(1000, 1000, 1000, 2000, 2000, 1000, 400, 1, 2, 3));
    raster.plot(-5, -5, 9, 9, 9);
    raster.plot(9999, 9999, 9, 9, 9);

    for (int i = 0; i < 16; i++) {
        CHECK(canvas[i] == 0xAA);
        CHECK(canvas[canvas.size() - 1 - i] == 0xAA);
    }
}

// Rows are addressed through row_stride, not width. A surface with padded rows
// must not smear diagonally.
void test_row_stride_is_respected() {
    const int width = 16, height = 16;
    const int stride = width * 3 + 11;   // deliberately not a packed row
    std::vector<uint8_t> canvas(static_cast<size_t>(stride) * height, 0);
    pse::RenderTarget target{canvas.data(), width, height, stride,
                             pse::PixelFormat::rgb888};

    pse::Rasterizer raster;
    raster.begin_frame(target);
    raster.plot(0, 2, 11, 22, 33);

    const uint8_t* row = canvas.data() + static_cast<size_t>(2) * stride;
    CHECK(row[0] == 11 && row[1] == 22 && row[2] == 33);
    // The padding at the end of row 1 must be untouched.
    const uint8_t* pad = canvas.data() + stride + width * 3;
    CHECK(pad[0] == 0);
}

void test_gradient_covers_every_pixel() {
    for (auto format : {pse::PixelFormat::rgb565, pse::PixelFormat::rgb888,
                        pse::PixelFormat::rgba8888}) {
        TestSurface surface(40, 40, format);
        pse::Rasterizer raster;
        raster.begin_frame(surface.target());
        raster.clear_gradient(20, 40, 80, 200, 220, 240);

        int top_r = 0, top_g = 0, top_b = 0;
        int bottom_r = 0, bottom_g = 0, bottom_b = 0;
        surface.pixel(0, 0, top_r, top_g, top_b);
        surface.pixel(39, 39, bottom_r, bottom_g, bottom_b);

        // 565 quantises, so compare with tolerance rather than exactly.
        CHECK(top_b < bottom_b);
        CHECK(top_r < bottom_r);
        surface.pixel(39, 0, top_r, top_g, top_b);
        CHECK(top_b < bottom_b);
    }
}

// The depth helper used by sprites must claim a pixel once and refuse a farther
// claim afterwards.
void test_billboard_depth_claim() {
    TestSurface surface(32, 32, pse::PixelFormat::rgb565);
    pse::Rasterizer raster;
    raster.begin_frame(surface.target());

    CHECK(raster.test_and_set_depth(4, 4, 100));
    CHECK(!raster.test_and_set_depth(4, 4, 150));
    CHECK(raster.test_and_set_depth(4, 4, 50));
    CHECK(!raster.test_and_set_depth(-1, 0, 10));
    CHECK(!raster.test_and_set_depth(0, 999, 10));
}

// A box in front of the camera should produce visible geometry, and the same
// box behind the camera should produce none.
void test_renderer_projects_and_culls() {
    TestSurface surface(pse::k_render_width, pse::k_render_height,
                        pse::PixelFormat::rgb565);
    pse::Rasterizer raster;
    pse::Renderer3D renderer(raster);

    raster.begin_frame(surface.target());
    renderer.set_orbit_camera(0.0f, 0.0f, 0.0f, 0.0f, 8.0f, 4.0f);
    renderer.draw_box(0.0f, 0.0f, 0.0f, 2.0f, 2.0f, 2.0f,
                      200, 100, 100, 100, 100, 200);
    CHECK(raster.triangles_drawn() > 0);
    CHECK(surface.any_pixel_set());

    TestSurface empty(pse::k_render_width, pse::k_render_height,
                      pse::PixelFormat::rgb565);
    pse::Rasterizer behind_raster;
    pse::Renderer3D behind(behind_raster);
    behind_raster.begin_frame(empty.target());
    behind.set_camera(0.0f, 0.0f, 0.0f, 0.0f, 0.0f);
    // The camera looks down +Z, so a box far along -Z is behind it.
    behind.draw_box(0.0f, 0.0f, -400.0f, 1.0f, 1.0f, 1.0f,
                    255, 255, 255, 255, 255, 255);
    CHECK(behind_raster.triangles_drawn() == 0);
}

// A box's lid has to be visible from above, which is the only side anyone
// looks at one from.
//
// It was not. The two end faces were wound like the four walls, so they faced
// inward, the backface cull dropped them, and a box seen from above showed
// the ground through the hole where its top should be. draw_box's top colour
// was reachable only by a camera underneath the box. The old test above
// checked that SOME pixel was set and that a box behind the camera drew
// nothing, both of which stayed true the whole time.
void test_box_has_a_lid() {
    // A distinctive top colour that cannot be confused with the sides, and a
    // camera directly above looking down.
    const uint8_t top_r = 250, top_g = 40, top_b = 10;

    TestSurface surface(pse::k_render_width, pse::k_render_height,
                        pse::PixelFormat::rgb888);
    pse::Rasterizer raster;
    pse::Renderer3D renderer(raster);
    raster.begin_frame(surface.target());
    renderer.set_orbit_camera(0.0f, 0.0f, 0.0f, 0.0f, 10.0f, 10.0f, 0.0f);
    renderer.draw_box(0.0f, 0.0f, 0.0f, 6.0f, 2.0f, 6.0f,
                      top_r, top_g, top_b, 30, 30, 30);

    int lid_pixels = 0;
    const pse::RenderTarget& t = surface.target();
    for (int y = 0; y < t.height; y++) {
        for (int x = 0; x < t.width; x++) {
            const uint8_t* p = t.pixels + y * t.row_stride + x * 3;
            // The lid is emitted unshaded, so it lands on screen exactly.
            if (p[0] == top_r && p[1] == top_g && p[2] == top_b) lid_pixels++;
        }
    }
    CHECK(lid_pixels > 100);

    // And the underside still faces the other way, so the box is solid rather
    // than merely inside out.
    TestSurface below(pse::k_render_width, pse::k_render_height,
                      pse::PixelFormat::rgb888);
    pse::Rasterizer under_raster;
    pse::Renderer3D under(under_raster);
    under_raster.begin_frame(below.target());
    under.set_orbit_camera(0.0f, 0.0f, 0.0f, 0.0f, 10.0f, -10.0f, 0.0f);
    under.draw_box(0.0f, 0.0f, 0.0f, 6.0f, 2.0f, 6.0f,
                   top_r, top_g, top_b, 30, 30, 30);
    int lid_from_below = 0;
    const pse::RenderTarget& b = below.target();
    for (int y = 0; y < b.height; y++) {
        for (int x = 0; x < b.width; x++) {
            const uint8_t* p = b.pixels + y * b.row_stride + x * 3;
            if (p[0] == top_r && p[1] == top_g && p[2] == top_b) lid_from_below++;
        }
    }
    CHECK(lid_from_below == 0);
}

// A mesh from obj2cpp must render, and must survive a corrupt index without
// reading out of bounds.
void test_mesh_rendering_and_bounds() {
    static const pse::MeshVertex vertices[] = {
        {-256, 0, -256}, {256, 0, -256}, {0, 256, 0}, {0, 0, 256},
    };
    static const pse::MeshFace faces[] = {
        {0, 1, 2, 200, 60, 60, 0, 0, -127},
        {1, 3, 2, 60, 200, 60, 127, 0, 0},
        {3, 0, 2, 60, 60, 200, -127, 0, 0},
        {0, 1, 3, 120, 120, 120, 0, -127, 0},
    };
    static const pse::MeshData model{vertices, 4, faces, 4, 256};

    TestSurface surface(pse::k_render_width, pse::k_render_height,
                        pse::PixelFormat::rgb888);
    pse::Rasterizer raster;
    pse::Renderer3D renderer(raster);
    raster.begin_frame(surface.target());
    renderer.set_orbit_camera(0.0f, 0.0f, 0.0f, 0.0f, 6.0f, 2.0f);
    renderer.draw_mesh(model, 0.0f, 0.0f, 0.0f, 0.3f, 1.0f);
    CHECK(raster.triangles_drawn() > 0);
    CHECK(surface.any_pixel_set());

    // An index past the end of the vertex table must be skipped, not followed.
    static const pse::MeshFace bad_faces[] = {{0, 1, 9999, 255, 0, 0, 0, 127, 0}};
    static const pse::MeshData bad{vertices, 4, bad_faces, 1, 256};
    pse::Rasterizer bad_raster;
    pse::Renderer3D bad_renderer(bad_raster);
    TestSurface bad_surface(pse::k_render_width, pse::k_render_height,
                            pse::PixelFormat::rgb888);
    bad_raster.begin_frame(bad_surface.target());
    bad_renderer.set_orbit_camera(0.0f, 0.0f, 0.0f, 0.0f, 6.0f, 2.0f);
    bad_renderer.draw_mesh(bad, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f);
    CHECK(bad_raster.triangles_drawn() == 0);

    // A zero scale mesh is a build error upstream, and must not divide by zero.
    static const pse::MeshData zero_scale{vertices, 4, faces, 4, 0};
    bad_renderer.draw_mesh(zero_scale, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f);
}

// Pitch tilts a mesh about its own lateral axis: positive pitch must lift the
// +Z nose of the model on screen, negative must drop it, and omitting the
// argument must reproduce the unpitched image exactly. A sign error here shows
// up on the device as a motorcycle wheelie tipping the bike onto its face.
void test_mesh_pitch() {
    // A single marker face out on the +Z nose, drawn with both windings so the
    // test cannot silently pass or fail off backface culling.
    static const pse::MeshVertex nose_vertices[] = {
        {-80, 0, 256}, {80, 0, 256}, {0, 80, 256},
    };
    static const pse::MeshFace nose_faces[] = {
        {0, 1, 2, 255, 255, 255, 0, 0, -127},
        {2, 1, 0, 255, 255, 255, 0, 0, 127},
    };
    static const pse::MeshData nose{nose_vertices, 3, nose_faces, 2, 256};

    // Centroid row of every lit pixel, or -1 for an empty frame.
    auto centroid_row = [](const TestSurface& surface) {
        long sum = 0, count = 0;
        for (int y = 0; y < pse::k_render_height; y++) {
            for (int x = 0; x < pse::k_render_width; x++) {
                int r, g, b;
                surface.pixel(x, y, r, g, b);
                if (r || g || b) { sum += y; count++; }
            }
        }
        return count > 0 ? static_cast<int>(sum / count) : -1;
    };

    auto render_at = [&](TestSurface& surface, float pitch) {
        pse::Rasterizer raster;
        pse::Renderer3D renderer(raster);
        raster.begin_frame(surface.target());
        renderer.set_camera(0.0f, 0.0f, -3.0f, 0.0f, 0.0f);
        renderer.draw_mesh(nose, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f,
                           255, 255, 255, pitch);
    };

    TestSurface level(pse::k_render_width, pse::k_render_height,
                      pse::PixelFormat::rgb888);
    TestSurface up(pse::k_render_width, pse::k_render_height,
                   pse::PixelFormat::rgb888);
    TestSurface down(pse::k_render_width, pse::k_render_height,
                     pse::PixelFormat::rgb888);
    render_at(level, 0.0f);
    render_at(up, 0.8f);
    render_at(down, -0.8f);

    const int row_level = centroid_row(level);
    const int row_up = centroid_row(up);
    const int row_down = centroid_row(down);
    CHECK(row_level >= 0 && row_up >= 0 && row_down >= 0);
    CHECK(row_up < row_level);       // screen y grows downward
    CHECK(row_down > row_level);

    // The default argument must be the identity: a call that never mentions
    // pitch renders byte for byte what it rendered before pitch existed.
    TestSurface defaulted(pse::k_render_width, pse::k_render_height,
                          pse::PixelFormat::rgb888);
    pse::Rasterizer raster;
    pse::Renderer3D renderer(raster);
    raster.begin_frame(defaulted.target());
    renderer.set_camera(0.0f, 0.0f, -3.0f, 0.0f, 0.0f);
    renderer.draw_mesh(nose, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f);
    CHECK(defaulted.bytes_equal(level));
}


// Split rasterization is only allowed to exist if it is invisible: collecting
// triangles and rendering them as two disjoint row bands must reproduce the
// immediate mode image byte for byte. Anything less and the two cores on the
// device would produce a seam down the middle of every frame.
void test_split_matches_immediate() {
    const int w = pse::k_render_width, h = pse::k_render_height;
    const pse::SkyGradient sky{30, 40, 90, 90, 120, 170};

    // Deterministic pseudo random triangle soup, including degenerate,
    // offscreen, and overlapping cases.
    uint32_t rng = 0xC0FFEE01u;
    auto next = [&rng]() {
        rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
        return rng;
    };

    pse::ScreenTriangle tris[120];
    for (auto& t : tris) {
        t.x0 = static_cast<int16_t>(static_cast<int>(next() % 200) - 40);
        t.y0 = static_cast<int16_t>(static_cast<int>(next() % 200) - 40);
        t.x1 = static_cast<int16_t>(static_cast<int>(next() % 200) - 40);
        t.y1 = static_cast<int16_t>(static_cast<int>(next() % 200) - 40);
        t.x2 = static_cast<int16_t>(static_cast<int>(next() % 200) - 40);
        t.y2 = static_cast<int16_t>(static_cast<int>(next() % 200) - 40);
        t.z0 = static_cast<uint16_t>(next() % 1024);
        t.z1 = static_cast<uint16_t>(next() % 1024);
        t.z2 = static_cast<uint16_t>(next() % 1024);
        t.r0 = static_cast<uint8_t>(next()); t.g0 = static_cast<uint8_t>(next());
        t.b0 = static_cast<uint8_t>(next());
        t.r1 = static_cast<uint8_t>(next()); t.g1 = static_cast<uint8_t>(next());
        t.b1 = static_cast<uint8_t>(next());
        t.r2 = static_cast<uint8_t>(next()); t.g2 = static_cast<uint8_t>(next());
        t.b2 = static_cast<uint8_t>(next());
    }

    // Immediate reference.
    TestSurface reference(w, h, pse::PixelFormat::rgb565);
    static pse::Rasterizer immediate;
    immediate.begin_frame(reference.target());
    immediate.clear_gradient(sky.top_r, sky.top_g, sky.top_b,
                             sky.bottom_r, sky.bottom_g, sky.bottom_b);
    for (const auto& t : tris) immediate.draw(t);

    // Collected and rendered as two bands.
    TestSurface split_out(w, h, pse::PixelFormat::rgb565);
    static pse::Rasterizer collector;
    static pse::FrameQueue queue;
    collector.begin_frame_collect(split_out.target(), queue);
    for (const auto& t : tris) collector.draw(t);
    pse::run_split(collector, queue, sky);
    collector.end_collect();

    CHECK(queue.dropped == 0);
    CHECK(collector.triangles_drawn() == queue.count);
    CHECK(reference.bytes_equal(split_out));

    // Post-split immediate drawing must still work (billboards and UI go on
    // top after the workers finish).
    collector.draw(make_triangle(2, 2, 2, 20, 20, 2, 50, 255, 255, 255));
    int r = 0, g = 0, b = 0;
    split_out.pixel(4, 4, r, g, b);
    CHECK(r > 200 && g > 200);
}

// A marked queue is two scenes: the first group must render only into the top
// band under its own gradient, the second only into the bottom band under
// its. Verified against a reference built from the band primitives directly.
void test_two_scene_split() {
    const int w = pse::k_render_width, h = pse::k_render_height;
    const int mid = h / 2;
    const pse::SkyGradient sky_top{200, 120, 60, 240, 200, 160};
    const pse::SkyGradient sky_bottom{40, 90, 120, 5, 10, 30};

    // One triangle per scene, both crossing the split row so the clipping is
    // actually exercised, horizontally separated so a leak of either into the
    // other band is detectable as a lone colour in pure gradient.
    pse::ScreenTriangle top_tri =
        make_triangle(10, 20, 30, 100, 50, 20, 90, 250, 20, 20);
    pse::ScreenTriangle bottom_tri =
        make_triangle(70, 100, 110, 100, 90, 20, 90, 20, 250, 20);

    // Reference: the exact band calls the split should reduce to.
    TestSurface reference(w, h, pse::PixelFormat::rgb565);
    static pse::Rasterizer ref_raster;
    ref_raster.begin_frame(reference.target());
    ref_raster.clear_gradient_span(sky_top.top_r, sky_top.top_g, sky_top.top_b,
                                   sky_top.bottom_r, sky_top.bottom_g,
                                   sky_top.bottom_b, 0, mid, 0, mid);
    ref_raster.clear_gradient_span(sky_bottom.top_r, sky_bottom.top_g,
                                   sky_bottom.top_b, sky_bottom.bottom_r,
                                   sky_bottom.bottom_g, sky_bottom.bottom_b,
                                   mid, h, mid, h);
    ref_raster.draw_rows(top_tri, 0, mid);
    ref_raster.draw_rows(bottom_tri, mid, h);

    TestSurface split_out(w, h, pse::PixelFormat::rgb565);
    static pse::Rasterizer collector;
    static pse::FrameQueue queue;
    collector.begin_frame_collect(split_out.target(), queue);
    collector.draw(top_tri);
    queue.mark_split();
    collector.draw(bottom_tri);
    pse::run_split(collector, queue, sky_top, sky_bottom);
    collector.end_collect();

    CHECK(queue.split == 1);
    CHECK(queue.count == 2);
    CHECK(reference.bytes_equal(split_out));

    // The top scene's red triangle continues below the split row
    // geometrically, but those rows belong to the bottom scene: they must
    // show only the bottom gradient. Same for the bottom scene's green
    // triangle above the split.
    int r = 0, g = 0, b = 0;
    split_out.pixel(30, 80, r, g, b);
    CHECK(r < 100);
    split_out.pixel(90, 40, r, g, b);
    CHECK(b > 60 && g < 220);
}

// Queue overflow must drop and count, never write out of bounds.
void test_queue_overflow() {
    static pse::FrameQueue queue;
    queue.reset();
    pse::ScreenTriangle t = make_triangle(0, 0, 0, 10, 10, 0, 100, 1, 2, 3);
    for (int i = 0; i < pse::FrameQueue::k_capacity + 25; i++) queue.push(t);
    CHECK(queue.count == pse::FrameQueue::k_capacity);
    CHECK(queue.dropped == 25);
}

// Guard the documented memory budget. If someone raises the render size this
// test is the thing that tells them what it just cost.
// The screen format on the device, pinned against the SDK's own packing.
//
// This is deliberately an independent copy of blend.cpp's pack_rgb565 rather
// than a readback through our own code: the previous test decoded 565 with the
// same convention the engine encoded it with, so red and blue were swapped in
// perfect agreement and every test passed while the hardware showed the wrong
// colours in every game.
void test_rgb565_matches_the_sdk() {
    // 32blit/graphics/blend.cpp:
    //     pack_rgb565(r, g, b) = (r >> 3) | ((g >> 2) << 5) | ((b >> 3) << 11)
    auto sdk_pack = [](int r, int g, int b) {
        return static_cast<uint16_t>((r >> 3) | ((g >> 2) << 5) |
                                     ((b >> 3) << 11));
    };

    for (int r = 0; r < 256; r += 5) {
        for (int g = 0; g < 256; g += 7) {
            for (int b = 0; b < 256; b += 11) {
                uint8_t bytes[2] = {0, 0};
                pse::Rgb565::store(bytes, static_cast<uint8_t>(r),
                                   static_cast<uint8_t>(g),
                                   static_cast<uint8_t>(b));
                const uint16_t got =
                    static_cast<uint16_t>(bytes[0] | (bytes[1] << 8));
                CHECK(got == sdk_pack(r, g, b));
            }
        }
    }

    // Spelled out, because "red is 0x001F" is the whole point and a reader
    // should not have to run the loop above to learn it.
    uint8_t red[2] = {0, 0};
    uint8_t blue[2] = {0, 0};
    pse::Rgb565::store(red, 255, 0, 0);
    pse::Rgb565::store(blue, 0, 0, 255);
    CHECK(red[0] == 0x1F && red[1] == 0x00);
    CHECK(blue[0] == 0x00 && blue[1] == 0xF8);
}

void test_memory_budget() {
    CHECK(sizeof(pse::MeshFace) == 12);
    CHECK(sizeof(pse::MeshVertex) == 6);
    CHECK(pse::k_render_width * pse::k_render_height <= 120 * 120);
    CHECK(sizeof(pse::Rasterizer) < 20 * 1024);
}

}  // namespace

int main() {
    test_winding_and_fill();
    test_depth_ordering();
    test_offscreen_clipping();
    test_row_stride_is_respected();
    test_gradient_covers_every_pixel();
    test_billboard_depth_claim();
    test_renderer_projects_and_culls();
    test_box_has_a_lid();
    test_mesh_rendering_and_bounds();
    test_mesh_pitch();
    test_rgb565_matches_the_sdk();
    test_memory_budget();
    test_split_matches_immediate();
    test_two_scene_split();
    test_queue_overflow();

    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
