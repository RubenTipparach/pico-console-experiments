// Render real frames through the real engine, with no SDK and no window, and
// write them as PPM. This is where the committed thumbnail comes from, and it
// is the only way to look at a change to the HUD or a menu without a device.
//
// It also self checks, which is why it is registered with add_test: a frame
// that comes out entirely one colour is a frame where the geometry never
// reached the rasterizer, and that has happened for reasons ranging from a
// near plane to a camera that had not been primed.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "pse/pixel.hpp"
#include "pse/raster.hpp"
#include "pse/shared_render.hpp"
#include "fixed.hpp"
#include "render.hpp"
#include "sim.hpp"

using namespace twinflare;

namespace {

// Every test below is about the RACE, not the grid, so they all start from the
// green light. race_init holds the pod on the line for a three second
// countdown now, and a test that ticks two hundred times and then asks how far
// the pod has travelled would otherwise be measuring the countdown.
void race_start(Race& race, int track_index, int racer_index) {
    race_init(race, track_index, racer_index);
    const Input idle{};
    while (race.phase == Phase::Countdown) race_tick(race, idle);
}

constexpr int k_w = 120;
constexpr int k_h = 120;
uint8_t g_pixels[k_w * k_h * 3];

pse::RenderTarget target() {
    return pse::RenderTarget{g_pixels, k_w, k_h, k_w * 3, pse::PixelFormat::rgb888};
}

void write_ppm(const std::string& dir, const char* name) {
    const std::string path = dir + "/" + name + ".ppm";
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) { std::printf("cannot write %s\n", path.c_str()); return; }
    std::fprintf(f, "P6\n%d %d\n255\n", k_w, k_h);
    std::fwrite(g_pixels, 1, sizeof(g_pixels), f);
    std::fclose(f);
}

// Distinct colours in the frame. One is a frame that never drew anything.
int distinct_colours() {
    int seen = 0;
    uint32_t table[64];
    for (int i = 0; i < k_w * k_h; ++i) {
        const uint32_t c = (g_pixels[i * 3] << 16) | (g_pixels[i * 3 + 1] << 8)
                         | g_pixels[i * 3 + 2];
        bool found = false;
        for (int k = 0; k < seen; ++k) if (table[k] == c) { found = true; break; }
        if (!found && seen < 64) table[seen++] = c;
    }
    return seen;
}

void drive(const Race& race, const Track& t, Input& in) {
    const Pod& p = race.pod;
    const int n = t.node_count;
    const TrackNode& here = t.nodes[p.node];
    const TrackNode& tgt = t.nodes[(p.node + 7) % n];
    int32_t want = fatan2(node_x(tgt) - p.x, node_z(tgt) - p.z);
    want -= clamp32(p.lateral / 96, -3000, 3000);
    const int32_t e = angle_diff(want, p.yaw) - (p.yaw_rate >> k_rate_fp) * 42 / 100;
    in.left = e < -400; in.right = e > 400;
    const TrackNode& next = t.nodes[(p.node + 1) % n];
    const int32_t h0 = fatan2(node_x(next) - node_x(here), node_z(next) - node_z(here));
    const TrackNode& a = t.nodes[(p.node + 14) % n];
    const TrackNode& b = t.nodes[(p.node + 15) % n];
    const int32_t bend = angle_diff(fatan2(node_x(b) - node_x(a), node_z(b) - node_z(a)), h0);
    const int32_t sharp = bend < 0 ? -bend : bend;
    in.brake = sharp > 2600 && pod_speed(p) > fscale(pod_top_speed(p), 500);
    in.throttle = !in.brake;
    in.up = (t.nodes[(p.node + 3) % n].flags & kGap) != 0;
}

}  // namespace

int main(int argc, char** argv) {
    const std::string dir = argc > 1 ? argv[1] : ".";
    int failures = 0;

    static const char* k_names[k_track_count] = {"dune", "tide", "ash", "frost"};
    for (int ti = 0; ti < k_track_count; ++ti) {
        Race race;
        race_start(race, ti, ti % k_racer_count);
        Input in{};
        const Track& t = track(ti);
        for (int i = 0; i < 1400; ++i) { drive(race, t, in); race_tick(race, in); }

        Chrome chrome;
        chrome.screen = Screen::Race;
        chrome.track = static_cast<uint8_t>(ti);
        render_frame(race, chrome, target());
        const int colours = distinct_colours();
        if (colours < 8) {
            std::printf("FAIL: %s rendered %d distinct colours\n", t.name, colours);
            ++failures;
        }
        write_ppm(dir, k_names[ti]);

        // The thumbnail is the desert, mid race, which is the picture that says
        // what the game is in one frame.
        if (ti == 0) write_ppm(dir, "thumbnail");
    }

    // One frame of each screen, so a menu change can be looked at.
    {
        Race race;
        race_start(race, 0, 0);
        Chrome chrome;
        struct { Screen s; const char* name; } shots[] = {
            {Screen::PodSelect, "screen_pod"},
            {Screen::TrackSelect, "screen_track"},
            {Screen::Results, "screen_results"},
        };
        for (const auto& shot : shots) {
            chrome.screen = shot.s;
            chrome.pod = 2;
            chrome.track = 2;
            race.place = 2;
            race.best_lap = 4231;
            render_frame(race, chrome, target());
            if (distinct_colours() < 4) {
                std::printf("FAIL: %s rendered flat\n", shot.name);
                ++failures;
            }
            write_ppm(dir, shot.name);
        }
        Input in{};
        in.throttle = true;
        for (int i = 0; i < 400; ++i) race_tick(race, in);
        chrome.screen = Screen::Paused;
        render_frame(race, chrome, target());
        write_ppm(dir, "screen_paused");
    }

    std::printf("twinflare preview wrote frames to %s (queue peak %u of %d)\n",
                dir.c_str(), pse::shared_queue().count, pse::FrameQueue::k_capacity);
    if (pse::shared_queue().dropped) {
        std::printf("FAIL: the queue dropped %u triangles\n",
                    pse::shared_queue().dropped);
        ++failures;
    }
    return failures ? 1 : 0;
}
