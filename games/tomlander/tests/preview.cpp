// Renders real Tom Lander frames on the host, through the real engine and the
// real generated model, and writes them as PPM files. This is how the game
// gets looked at and tuned without a device in hand, and where the thumbnail
// comes from.
//
// It also checks the two things only the renderer can answer, because the sim
// cannot ask it anything: that a frame never overflows the triangle queue,
// and that the target arrow appears exactly when the deck is out of frame.
//
// Usage: tomlander_preview [out_dir]

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "pse/pixel.hpp"

#include "render.hpp"
#include "sim.hpp"

namespace {

constexpr int k_w = 120;
constexpr int k_h = 120;

uint32_t g_clock = 1000;
uint16_t g_worst_queued = 0;
uint16_t g_worst_dropped = 0;
int g_failures = 0;

void fail(const char* what) {
    std::printf("FAIL: %s\n", what);
    g_failures++;
}

void write_ppm(const char* path, const uint8_t* rgb) {
    FILE* f = std::fopen(path, "wb");
    if (!f) return;
    std::fprintf(f, "P6\n%d %d\n255\n", k_w, k_h);
    std::fwrite(rgb, 1, static_cast<size_t>(k_w) * k_h * 3, f);
    std::fclose(f);
}

void draw(const tl::World& world, float yaw, const std::string& path) {
    static std::vector<uint8_t> buffer(static_cast<size_t>(k_w) * k_h * 3);
    pse::RenderTarget target{buffer.data(), k_w, k_h, k_w * 3,
                             pse::PixelFormat::rgb888};
    tlr::render_scene(world, target, yaw, g_clock);
    if (!path.empty()) write_ppm(path.c_str(), buffer.data());
}

void capture(const tl::World& world, float yaw, const std::string& out,
             const char* name) {
    const std::string path = out + "/" + name;
    for (int f = 0; f < 3; f++) {
        g_clock += 33;
        draw(world, yaw, "");
    }
    draw(world, yaw, path);
    const tlr::FrameStats stats = tlr::last_frame_stats();
    if (stats.queued > g_worst_queued) g_worst_queued = stats.queued;
    if (stats.dropped > g_worst_dropped) g_worst_dropped = stats.dropped;
    std::printf("wrote %s (alt %d, %u tris%s)\n", path.c_str(),
                tl::altitude(world) >> 16, static_cast<unsigned>(stats.queued),
                stats.dropped ? ", DROPPED" : "");
}

void run(tl::World& world, const tl::Input& input, int ticks) {
    for (int i = 0; i < ticks; i++) tl::world_tick(world, input);
}

tl::Input fire_all() {
    tl::Input in{};
    in.level = true;
    return in;
}

// Fly toward the target deck the way a player would: hold level to arrest a
// fall, lean on one pod to translate. Crude, and enough to reach pad B.
void fly_toward_target(tl::World& world, int max_ticks) {
    for (int i = 0; i < max_ticks && world.state == tl::Flight::Flying; i++) {
        tl::Input in{};
        const int32_t alt = tl::altitude(world);
        const bool low = alt < (22 << 16);
        const bool sinking = tl::descent(world) > (tl::k_safe_descent / 2);
        if (low || sinking) {
            in.level = true;
        } else {
            // Lean toward the pad. The pod that fires is the one on the far
            // side from where we want to go, because a pod lifts its own
            // corner and the hull travels away from it.
            const tl::Pad& pad = world.pads[world.target];
            const int32_t dx = pad.x - world.x;
            const int32_t dz = pad.z - world.z;
            if ((dx < 0 ? -dx : dx) > (dz < 0 ? -dz : dz)) {
                in.pod[dx > 0 ? tl::kPodLeft : tl::kPodRight] = true;
            } else {
                in.pod[dz > 0 ? tl::kPodBack : tl::kPodFront] = true;
            }
        }
        tl::world_tick(world, in);
        if (tl::range_to_target(world) < 4) return;
    }
}

}  // namespace

int main(int argc, char** argv) {
    const std::string out = argc > 1 ? argv[1] : ".";

    // 1: sitting on pad A, which is exactly what the title screen sits over.
    tl::World world;
    tl::world_init(world);
    capture(world, 0.0f, out, "preview_1_pad.ppm");

    // 2: lifting off, all four lit.
    run(world, fire_all(), 90);
    capture(world, 0.0f, out, "preview_2_liftoff.ppm");

    // 3: leaning across the valley on one pod.
    {
        tl::Input lean{};
        lean.pod[tl::kPodRight] = true;
        run(world, lean, 60);
        capture(world, 0.0f, out, "preview_3_lean.ppm");
    }

    // 4: the camera swung round, which is what left and right do.
    capture(world, 1.1f, out, "preview_4_orbit.ppm");

    // 5: on the approach to pad B.
    {
        tl::World cruise;
        tl::world_init(cruise);
        run(cruise, fire_all(), 80);
        fly_toward_target(cruise, 4000);
        capture(cruise, 0.0f, out, "preview_5_approach.ppm");
        std::printf("approach ended at range %d, state %d\n",
                    tl::range_to_target(cruise),
                    static_cast<int>(cruise.state));
    }

    // 6: a wreck, which is most of what a player will see at first.
    {
        tl::World doomed;
        tl::world_init(doomed);
        doomed.y = tl::ground_at(doomed, doomed.x, doomed.z) + (70 << 16);
        doomed.grounded = false;
        tl::Input none{};
        for (int i = 0; i < 900 && doomed.state == tl::Flight::Flying; i++) {
            tl::world_tick(doomed, none);
        }
        if (doomed.state != tl::Flight::Crashed) fail("a long fall must crash");
        capture(doomed, 0.0f, out, "preview_6_wreck.ppm");
    }

    // The arrow promise, checked against the projection that actually draws
    // the deck: it must appear when the deck is out of frame and stay away
    // when it is in frame. The arrow's direction comes from the world bearing
    // rather than from that projection, so nothing else can check the two
    // agree.
    {
        tl::World probe;
        tl::world_init(probe);
        const tl::Pad& pad = probe.pads[probe.target];

        struct Case { int32_t dx, dz; float yaw; const char* name; };
        const Case cases[] = {
            {0, -95 * 65536, 0.0f, "far short of it"},
            {0,  95 * 65536, 0.0f, "past it, behind the camera"},
            {-95 * 65536, 0, 0.0f, "well to its left"},
            {95 * 65536, 0, 0.0f, "well to its right"},
            {0, -95 * 65536, 1.57f, "short of it, camera turned"},
        };
        for (const Case& c : cases) {
            probe.x = pad.x + c.dx;
            probe.z = pad.z + c.dz;
            probe.y = tl::ground_at(probe, probe.x, probe.z) + (55 << 16);
            probe.state = tl::Flight::Flying;
            draw(probe, c.yaw, "");
            const tlr::FrameStats s = tlr::last_frame_stats();
            const bool in_frame = s.pad_visible && s.pad_x >= 13 &&
                                  s.pad_x <= k_w - 13 && s.pad_y >= 13 &&
                                  s.pad_y <= k_h - 13;
            const bool arrow = s.arrow_x >= 0;
            std::printf("arrow, %s: deck %s, arrow %s\n", c.name,
                        in_frame ? "in frame" : "out of frame",
                        arrow ? "drawn" : "hidden");
            if (in_frame == arrow) {
                fail("the arrow must appear exactly when the deck does not");
            }
            if (arrow && (s.arrow_x < 0 || s.arrow_x >= k_w ||
                          s.arrow_y < 0 || s.arrow_y >= k_h)) {
                fail("the arrow must land on the screen");
            }
        }
    }

    // The flame promise. A plume leaves along the hull's own down axis, so
    // rolling the ship has to lean every plume the same way across the
    // screen. This is worth pinning because the flames never reach the
    // triangle queue and never reach a test either: they are plotted straight
    // into the framebuffer, and a plume falling vertically out of a tilted
    // nozzle looks close enough to right in a still frame to survive.
    {
        tl::World tilted;
        tl::world_init(tilted);
        tilted.grounded = false;
        tilted.y += 40 << 16;
        for (int i = 0; i < tl::kPodCount; i++) tilted.throttle[i] = 255;

        // The lean that separates a rolled hull from perspective convergence,
        // in the same 1/64ths FrameStats reports. A level ship comes in under
        // it, a 30 degree roll well over.
        const int k_lean = 16;

        struct Case { int32_t roll_deg; const char* name; };
        const Case cases[] = {
            {0, "level"},
            {30, "rolled right, +X side up"},
            {-30, "rolled left, -X side up"},
        };
        for (const Case& c : cases) {
            // Roll is a turn about the hull's own z. Positive raises the +x
            // side, which is what the engine's test_mesh_roll pins.
            tilted.q = pse::quat_from_axis_angle(
                0.0f, 0.0f, 1.0f,
                static_cast<float>(c.roll_deg) * 3.14159265f / 180.0f);
            draw(tilted, 0.0f, "");
            const tlr::FrameStats s = tlr::last_frame_stats();
            std::printf("flame, %s: aim", c.name);
            for (int i = 0; i < tl::kPodCount; i++) {
                std::printf(" (%d,%d)", s.flame_ax[i], s.flame_ay[i]);
            }
            std::printf("\n");
            for (int i = 0; i < tl::kPodCount; i++) {
                if (s.flame_ay[i] <= 0) fail("a plume must run down the hull");
                if (c.roll_deg > 0 && s.flame_ax[i] <= k_lean) {
                    fail("rolling right must lean the plumes right");
                }
                if (c.roll_deg < 0 && s.flame_ax[i] >= -k_lean) {
                    fail("rolling left must lean the plumes left");
                }
            }
            // A level ship's plumes are near vertical but not exactly: a
            // world vertical line off the centre of the screen converges
            // toward the vanishing point under perspective, and that is the
            // projection being right rather than the aim being wrong. What
            // has to hold is that the lean is small and that the two side
            // pods converge by the same amount in opposite directions.
            if (c.roll_deg == 0) {
                for (int i = 0; i < tl::kPodCount; i++) {
                    if (s.flame_ax[i] > k_lean || s.flame_ax[i] < -k_lean) {
                        fail("a level ship's plumes must run near vertical");
                    }
                }
                if (s.flame_ax[tl::kPodRight] != -s.flame_ax[tl::kPodLeft]) {
                    fail("a level ship's side plumes must converge evenly");
                }
                if (s.flame_ax[tl::kPodFront] != 0 ||
                    s.flame_ax[tl::kPodBack] != 0) {
                    fail("a level ship's centred plumes must not lean");
                }
            }
        }
    }

    std::printf("worst frame: %u triangles, %u dropped\n",
                static_cast<unsigned>(g_worst_queued),
                static_cast<unsigned>(g_worst_dropped));
    if (g_worst_dropped > 0) {
        fail("a frame overflowed the triangle queue");
    }
    if (g_failures) {
        std::printf("%d preview check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("preview checks passed\n");
    return 0;
}
