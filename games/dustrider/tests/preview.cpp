// Renders real Dust Rider frames on the host, through the real engine and
// the real generated models, and writes them as PPM files. This is how the
// game gets looked at and tuned without a device in hand.
//
// Usage: dustrider_preview [out_dir]

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "pse/pixel.hpp"
#include "pse/raster.hpp"

#include "bot.hpp"
#include "render.hpp"
#include "sim.hpp"

namespace {

constexpr int k_w = 120;
constexpr int k_h = 120;

uint32_t g_clock = 1000;
uint16_t g_worst_dropped = 0;
uint16_t g_worst_queued = 0;
bool g_window_fail = false;

void write_ppm(const char* path, const uint8_t* rgb) {
    FILE* f = std::fopen(path, "wb");
    if (!f) return;
    std::fprintf(f, "P6\n%d %d\n255\n", k_w, k_h);
    std::fwrite(rgb, 1, static_cast<size_t>(k_w) * k_h * 3, f);
    std::fclose(f);
}

void draw(const dr::World& world, const std::string& path) {
    static std::vector<uint8_t> buffer(static_cast<size_t>(k_w) * k_h * 3);
    pse::RenderTarget target{buffer.data(), k_w, k_h, k_w * 3,
                             pse::PixelFormat::rgb888};
    drr::render_scene(world, target, g_clock);
    write_ppm(path.c_str(), buffer.data());
}

// Render a few frames so the eased camera and body pitch settle where play
// would actually show them, then keep the last one.
void capture(const dr::World& world, const std::string& out,
             const char* name) {
    const std::string scratch = out + "/tmp.ppm";
    for (int f = 0; f < 10; f++) {
        g_clock += 33;
        draw(world, scratch);
    }
    const std::string path = out + "/" + name;
    draw(world, path);
    const drr::FrameStats stats = drr::last_frame_stats();
    if (stats.dropped > g_worst_dropped) g_worst_dropped = stats.dropped;
    if (stats.queued > g_worst_queued) g_worst_queued = stats.queued;
    std::printf("wrote %s (%dm, offset %d, %u tris%s)\n", path.c_str(),
                dr::distance_m(world), dr::road_offset(world),
                static_cast<unsigned>(stats.queued),
                stats.dropped ? ", DROPPED" : "");
}

void bot_until(dr::World& world, int max_ticks, bool (*done)(const dr::World&)) {
    for (int i = 0; i < max_ticks && world.alive; i++) {
        dr::world_tick(world, dr::bot_input(world));
        if (done && done(world)) return;
    }
}

// How hard the road is turning right where the bike is, fp8 over 4 m.
int32_t bend_here(const dr::World& w) {
    const int32_t a = dr::track_center_z(w, w.x);
    const int32_t b = dr::track_center_z(w, w.x + (4 << 8));
    return b - a;
}

}  // namespace

int main(int argc, char** argv) {
    const std::string out = argc > 1 ? argv[1] : ".";

    // 1: the start line, exactly what the title screen sits over.
    dr::World world;
    dr::world_init(world, 20260804);
    capture(world, out, "preview_1_start.ppm");

    // 2: the launch wheelie, nose up under full throttle.
    for (int i = 0; i < 55; i++) {
        dr::Input in{};
        in.throttle = true;
        dr::world_tick(world, in);
    }
    capture(world, out, "preview_2_wheelie.ppm");

    // 3: cruising a straight stretch.
    bot_until(world, 30000, [](const dr::World& w) {
        const int32_t bend = bend_here(w);
        return w.tick > 900 && bend < 24 && bend > -24;
    });
    capture(world, out, "preview_3_cruise.ppm");

    // 4: hard into a bend, the road snaking away north.
    bot_until(world, 30000, [](const dr::World& w) {
        return bend_here(w) > 300;
    });
    capture(world, out, "preview_4_bend.ppm");

    // 5: the other way, the road falling south.
    bot_until(world, 30000, [](const dr::World& w) {
        return bend_here(w) < -300;
    });
    capture(world, out, "preview_5_bend_south.ppm");

    // 6: the middle of a railed stretch, the north edge walled off. Waiting
    // for rail-just-ahead framed the very start of a run, which is mostly
    // off the right of the screen and looks like the rails are missing.
    bot_until(world, 60000, [](const dr::World& w) {
        return dr::track_rail_at(w, w.x - (4 << 8)) &&
               dr::track_rail_at(w, w.x + (6 << 8));
    });
    capture(world, out, "preview_6_rail.ppm");

    // 7: a cactus on the shoulder, close alongside.
    bot_until(world, 60000, [](const dr::World& w) {
        int32_t cx, cz;
        return dr::track_next_cactus(w, w.x, 3 << 8, cx, cz);
    });
    capture(world, out, "preview_7_cactus.ppm");

    // 8: drifting off into the sand toward one, which is how riders die.
    {
        dr::World doomed;
        dr::world_init(doomed, 99);
        dr::world_test_straight(doomed, true);
        dr::world_test_clear_hazards(doomed);
        dr::Input in{};
        in.throttle = true;
        for (int i = 0; i < 90; i++) dr::world_tick(doomed, in);
        dr::world_test_place_cactus(doomed, doomed.x + (14 << 8),
                                    dr::k_cactus_off_min);
        for (int i = 0; i < 900 && doomed.alive; i++) {
            dr::Input ride{};
            ride.throttle = true;
            ride.north = dr::road_offset(doomed) < dr::k_cactus_off_min;
            dr::world_tick(doomed, ride);
            doomed.screen_x = doomed.x;
        }
        for (int i = 0; i < 25; i++) dr::world_tick(doomed, dr::Input{});
        capture(doomed, out, "preview_8_wreck.ppm");
    }

    // The window promise, checked against the projection that actually
    // draws the bike: at the death threshold nothing of it may be on
    // screen, and a nudge back inside must put something back on screen.
    {
        dr::World probe;
        dr::world_init(probe, 7);
        dr::world_test_straight(probe, true);
        dr::world_test_clear_hazards(probe);
        const std::string scratch = out + "/tmp.ppm";

        // Measured with the bike as FAR from the lens as it can get, which
        // is hard north. The camera is road locked, so the bike's depth
        // varies with how far off the centerline it has strayed, and the
        // farther it is the smaller it draws and the later it clears the
        // frame. Deriving the window against the worst case makes it safe
        // for every nearer one.
        auto span_at = [&](int32_t rel) {
            probe.x = (60 << 8) + rel;
            probe.screen_x = 60 << 8;
            probe.z = dr::k_offroad_max;
            for (int f = 0; f < 30; f++) draw(probe, scratch);
            return drr::last_frame_stats();
        };

        for (int sign = -1; sign <= 1; sign += 2) {
            const drr::FrameStats edge = span_at(sign * dr::k_window_half);
            const bool off = edge.bike_x1 < 0 || edge.bike_x0 > k_w - 1;
            std::printf("window %s edge: bike span %d..%d -> %s\n",
                        sign < 0 ? "left" : "right",
                        edge.bike_x0, edge.bike_x1,
                        off ? "fully off screen" : "STILL VISIBLE");
            if (!off) g_window_fail = true;

            // 0.4 m back inside the window, some of the bike must show.
            const drr::FrameStats in =
                span_at(sign * (dr::k_window_half - 102));
            const bool visible = in.bike_x1 >= 0 && in.bike_x0 <= k_w - 1;
            std::printf("window %s inside: bike span %d..%d -> %s\n",
                        sign < 0 ? "left" : "right",
                        in.bike_x0, in.bike_x1,
                        visible ? "visible" : "ALREADY GONE");
            if (!visible) g_window_fail = true;
        }

        // The rider moves up and down the road by design, so both extremes
        // of that travel have to stay in frame vertically. Losing the bike
        // off the top or the bottom is a framing bug, not a death.
        for (int side = -1; side <= 1; side += 2) {
            probe.x = 60 << 8;
            probe.screen_x = 60 << 8;
            probe.z = side * dr::k_offroad_max;
            for (int f = 0; f < 30; f++) draw(probe, scratch);
            const drr::FrameStats s = drr::last_frame_stats();
            const bool framed = s.bike_y0 >= 0 && s.bike_y1 <= k_h - 1 &&
                                s.bike_x0 >= 0 && s.bike_x1 <= k_w - 1;
            std::printf("framing %s shoulder: bike x %d..%d y %d..%d -> %s\n",
                        side < 0 ? "south" : "north",
                        s.bike_x0, s.bike_x1, s.bike_y0, s.bike_y1,
                        framed ? "fully in frame" : "CLIPPED");
            if (!framed) g_window_fail = true;
        }
    }

    std::remove((out + "/tmp.ppm").c_str());

    // The triangle queue silently drops its overflow, which on screen is a
    // hole in the desert. Fail loudly here rather than let it be found on
    // hardware.
    std::printf("peak %u triangles of %d\n",
                static_cast<unsigned>(g_worst_queued),
                pse::FrameQueue::k_capacity);
    if (g_worst_dropped > 0) {
        std::printf("FAIL: dropped %u triangles, the queue is too small\n",
                    static_cast<unsigned>(g_worst_dropped));
        return 1;
    }
    if (g_window_fail) {
        std::printf("FAIL: k_window_half does not match the camera\n");
        return 1;
    }
    return 0;
}
