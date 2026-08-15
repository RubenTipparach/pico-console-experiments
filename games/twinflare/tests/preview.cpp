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

// The median luminance of a band of the frame.
//
// The median rather than the mean or the maximum, and that choice is the whole
// usefulness of it: the results board's rows are mostly background with text
// over them, so the median IS the background and the text cannot drag it. A
// maximum would report the brightest text pixel and a mean would move with how
// many characters a name happens to have.
int median_luma(int y0, int y1) {
    int hist[256] = {0};
    int n = 0;
    for (int y = y0; y < y1; ++y) {
        for (int x = 0; x < k_w; ++x) {
            const uint8_t* p = g_pixels + (y * k_w + x) * 3;
            const int luma = (p[0] * 77 + p[1] * 151 + p[2] * 28) >> 8;
            ++hist[luma < 0 ? 0 : (luma > 255 ? 255 : luma)];
            ++n;
        }
    }
    int seen = 0;
    for (int i = 0; i < 256; ++i) {
        seen += hist[i];
        if (seen * 2 >= n) return i;
    }
    return 255;
}

// Pixels wearing a given racer's paint. The hull, the trim and the binder are
// per racer and nothing on any of these four tracks is coloured out of the same
// table, so this is a reliable "is that pod in the picture" for a still frame.
//
// A tolerance rather than an exact match, because a lit face is its base colour
// scaled by the lambert term and only a face square on to the light is the
// colour in the roster.
int livery_pixels(const Racer& rc) {
    const uint8_t* wanted[3] = {rc.colour[0], rc.colour[1], rc.arc};
    int hits = 0;
    for (int i = 0; i < k_w * k_h; ++i) {
        for (int c = 0; c < 3; ++c) {
            const int dr = g_pixels[i * 3] - wanted[c][0];
            const int dg = g_pixels[i * 3 + 1] - wanted[c][1];
            const int db = g_pixels[i * 3 + 2] - wanted[c][2];
            if (dr > -26 && dr < 26 && dg > -26 && dg < 26 && db > -26 && db < 26) {
                ++hits;
                break;
            }
        }
    }
    return hits;
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

    // The grid, at each of the three numbers and at GO, with the charge partway
    // up. Four frames rather than one because the whole screen is a number that
    // changes and a single shot cannot show it changing.
    {
        Race race;
        race_init(race, 0, 0);
        Chrome chrome;
        chrome.screen = Screen::Race;
        Input flat{};
        flat.throttle = true;
        const Input idle{};
        static const char* k_shots[4] = {"grid_3", "grid_2", "grid_1", "grid_go"};
        int shot = 0;
        int last = countdown_number(race);
        render_frame(race, chrome, target());
        write_ppm(dir, k_shots[shot++]);
        while (race.phase == Phase::Countdown && shot < 4) {
            // Wound up from the second number on, which is the shot worth
            // having: a charge bar with something in it and the mark it must
            // not reach.
            race_tick(race, countdown_number(race) <= 2 ? flat : idle);
            const int n = countdown_number(race);
            if (n == last && race.phase == Phase::Countdown) continue;
            last = n;
            render_frame(race, chrome, target());
            if (distinct_colours() < 4) {
                std::printf("FAIL: %s rendered flat\n", k_shots[shot]);
                ++failures;
            }
            write_ppm(dir, k_shots[shot++]);
        }
        if (shot < 4) {
            std::printf("FAIL: the countdown only produced %d of 4 frames\n", shot);
            ++failures;
        }
    }

    // The run in: the board over a live race, at two of the four camera angles,
    // and a pod with a hurt engine so the smoke and the sparks are in a picture
    // somebody can look at.
    {
        Race race;
        race_start(race, 0, 0);
        Input in{};
        const Track& t = track(0);
        int guard = 0;
        while (!race.finished && guard++ < 40000) { drive(race, t, in); race_tick(race, in); }
        // Hurt, so the frames show what damage looks like from the chase
        // camera. Set rather than driven into, because arranging a specific
        // amount of damage by crashing is a test of the crash.
        race.pod.engine[0] = static_cast<int16_t>(race.pod.engine_max / 5);
        race.pod.hit[1] = k_hit_ticks;
        Chrome chrome;
        chrome.screen = Screen::Race;
        const Input hands_off{};
        for (int mode = 0; mode < k_cam_modes; ++mode) {
            while (race.cam_mode != mode && guard++ < 60000) {
                race.pod.engine[0] = static_cast<int16_t>(race.pod.engine_max / 5);
                race.pod.hit[1] = k_hit_ticks;
                race_tick(race, hands_off);
            }
            render_frame(race, chrome, target());
            char name[16] = {'f','i','n','i','s','h','_','0','\0'};
            name[7] = static_cast<char>('0' + mode);
            if (distinct_colours() < 8) {
                std::printf("FAIL: %s rendered flat\n", name);
                ++failures;
            }
            // AND THE POD IS IN THE SHOT. A cinematic angle that has framed
            // scenery is a frame that is not flat, has the right number of
            // colours, and is of nothing: the plain chase angle drew ZERO
            // pixels of pod, because it puts the subject dead centre and the
            // results board is dead centre. Counted in the racer's own livery,
            // which nothing else on a desert wears.
            const int pod_px = livery_pixels(racer(race.pod.racer_index));
            std::printf("  %-9s pod pixels %3d\n", name, pod_px);
            if (pod_px < 20) {
                std::printf("FAIL: %s frames the pod out of the shot\n", name);
                ++failures;
            }
            write_ppm(dir, name);
        }
        // And the panel it lands on, which now carries the whole field.
        chrome.screen = Screen::Results;
        render_frame(race, chrome, target());
        write_ppm(dir, "screen_board");
    }

    // The results board is BLENDED over the live race now, and the thing that
    // buys is also the thing that can break it: the race showing through is
    // scenery behind text.
    //
    // HOARFROST is the case that matters and the reason this runs on all four
    // circuits rather than on the desert the other finish frames use. It is
    // white ice under white cloud, the brightest ground in the game, and a
    // board mixed too far toward see-through is white rows on a white hill.
    // Measured as the median luminance of the board's own rows, which is its
    // background, against the 236 the text is drawn at.
    {
        static const char* k_names[k_track_count] = {
            "board_dune", "board_tide", "board_ash", "board_frost"};
        for (int ti = 0; ti < k_track_count; ++ti) {
            Race race;
            race_start(race, ti, 0);
            const Track& t = track(ti);
            Input in{};
            int guard = 0;
            while (!race.finished && guard++ < 40000) {
                drive(race, t, in);
                race_tick(race, in);
            }
            Chrome chrome;
            chrome.screen = Screen::Race;
            const Input hands_off{};
            for (int i = 0; i < 30; ++i) race_tick(race, hands_off);
            render_frame(race, chrome, target());
            // The board sits at the bottom of the screen. Its rows are the band
            // under the header rule.
            const int luma = median_luma(k_h - 50, k_h - 4);
            std::printf("  %-11s board rows median luma %3d (text is 236)\n",
                        k_names[ti], luma);
            // Dark enough that light text on it is still light text. A ratio of
            // two is the usual floor for legibility and 236/2 is 118.
            if (luma > 118) {
                std::printf("FAIL: %s board is too pale to read text on\n",
                            k_names[ti]);
                ++failures;
            }
            // AND NOT OPAQUE, which is the other half and the easy one to lose:
            // a board that passed the check above by being solid black would be
            // the wall this stopped being. The race behind it has to still be
            // getting through, so the board cannot be the flat 14 its own fill
            // colour would give.
            if (luma < 20) {
                std::printf("FAIL: %s board is solid, nothing shows through\n",
                            k_names[ti]);
                ++failures;
            }
            write_ppm(dir, k_names[ti]);
        }
    }

    // The fuse, on the frame a player actually sees it: one engine gone and the
    // pod counting down to coming apart.
    {
        for (int at = 3; at >= 1; --at) {
            Race race;
            race_start(race, 0, 0);
            Input in{};
            const Track& t = track(0);
            for (int i = 0; i < 900; ++i) { drive(race, t, in); race_tick(race, in); }
            race.pod.engine[0] = 0;
            race.pod.dead = 1;
            Input coast{};
            int guard = 0;
            while (fuse_seconds(race.pod) != at && guard++ < 400)
                race_tick(race, coast);
            Chrome chrome;
            chrome.screen = Screen::Race;
            render_frame(race, chrome, target());
            char name[16] = {'f','u','s','e','_','0','\0'};
            name[5] = static_cast<char>('0' + at);
            if (fuse_seconds(race.pod) != at) {
                std::printf("FAIL: never reached %d on the fuse\n", at);
                ++failures;
            }
            write_ppm(dir, name);
        }
    }

    // The pod coming apart, sampled across the wreck. The fuse blows, both
    // engines go, and these are the frames between the bang and the respawn.
    // Taken at ticks rather than at fractions so the names say when: boom_02 is
    // two ticks after it went, which is the flash, and boom_90 is the smoke.
    {
        static const int k_when[6] = {1, 6, 16, 34, 60, 100};
        Race race;
        race_start(race, 0, 0);
        Input in{};
        const Track& t = track(0);
        // 300 rather than the 900 the other shots use, on purpose: the fuse
        // takes three more seconds to burn, and from 900 that lands the wreck
        // in a shadowed dune where the whole frame is nearly black. Same
        // explosion, a stretch of road you can see it on.
        for (int i = 0; i < 300; ++i) { drive(race, t, in); race_tick(race, in); }
        // One engine out, then let the fuse run: this is the wreck the
        // requirement is about, not a pod dropped down a hole.
        race.pod.engine[0] = 0;
        race.pod.dead = 1;
        Input coast{};
        int guard = 0;
        while (race.pod.wreck_ticks == 0 && guard++ < 600) {
            race_tick(race, coast);
            // The last intact frame, kept as the scale reference: an explosion
            // wants to be bigger than the thing that exploded, and the only
            // way to know is to have the pod at the same distance to hold it
            // against. Overwritten every tick, so what lands on disk is the
            // frame immediately before the bang.
            if (race.pod.wreck_ticks == 0 && fuse_seconds(race.pod) == 1) {
                Chrome pre;
                pre.screen = Screen::Race;
                render_frame(race, pre, target());
                write_ppm(dir, "boom_000_before");
            }
        }
        if (race.pod.wreck_ticks == 0) {
            std::printf("FAIL: the fuse never blew, so there is no wreck to draw\n");
            ++failures;
        }
        int at = 0;
        for (int shot = 0; shot < 6; ++shot) {
            while (at < k_when[shot]) { race_tick(race, coast); ++at; }
            Chrome chrome;
            chrome.screen = Screen::Race;
            render_frame(race, chrome, target());
            // The hull must be GONE and the explosion must be THERE. Checked
            // here rather than only in the render tests because this is the
            // harness that produces the picture: a silent failure here is a
            // frame of empty scenery that still gets written to disk and still
            // looks like a plausible screenshot of a track.
            if (render_stats().boom == 0) {
                std::printf("FAIL: nothing drawn %d ticks into the wreck\n",
                            k_when[shot]);
                ++failures;
            }
            // Zero padded to three, because two digits turned tick 100 into
            // "boom_:0": '0' + 10 is ':' and the file still wrote perfectly
            // happily under a name nothing would ever look for.
            char name[16];
            std::snprintf(name, sizeof name, "boom_%03d", k_when[shot]);
            write_ppm(dir, name);
        }
        std::printf("  boom: %d pieces on the last frame, wreck_ticks %d\n",
                    render_stats().boom, race.pod.wreck_ticks);
    }

    // Damage, from the chase camera, which is the angle the game is played at
    // and the one the smoke exists for. Three frames: healthy, one engine
    // smoking, and one engine smoking while the other is being struck.
    {
        static const char* k_names[4] = {"hurt_none", "hurt_smoke", "hurt_both",
                                         "hurt_dead"};
        for (int shot = 0; shot < 4; ++shot) {
            Race race;
            race_start(race, 0, 0);
            Input in{};
            const Track& t = track(0);
            for (int i = 0; i < 900; ++i) { drive(race, t, in); race_tick(race, in); }
            // Held across the frames the effects run over, because both are
            // animated off the race clock and one still frame of a plume is a
            // plume at one instant of its life.
            for (int i = 0; i < 40; ++i) {
                if (shot >= 1) race.pod.engine[0] =
                    static_cast<int16_t>(race.pod.engine_max / 6);
                if (shot == 2) race.pod.hit[1] = k_hit_ticks;
                if (shot == 3) { race.pod.engine[0] = 0; race.pod.dead = 1; }
                drive(race, t, in);
                race_tick(race, in);
            }
            Chrome chrome;
            chrome.screen = Screen::Race;
            render_frame(race, chrome, target());
            const RenderStats& s = render_stats();
            // The counters, because a plume that is submitted and never lands a
            // pixel looks exactly like a plume that was never submitted, and
            // that has happened here before with the sparks.
            std::printf("  %-10s smoke %u sparks %u\n", k_names[shot], s.smoke,
                        s.sparks);
            // A healthy pod trails nothing; a hurt one trails smoke; a hurt one
            // being struck trails both. A DEAD engine trails nothing either,
            // and that last case is the one worth a frame of its own: the
            // renderer draws no mesh and no cable for a dead engine, so smoke
            // off one is a plume pouring out of a hole in the air, and it read
            // as working because the counter went up.
            const bool want_smoke = (shot == 1 || shot == 2);
            const bool want_sparks = (shot == 2);
            if (want_smoke != (s.smoke > 0)) {
                std::printf("FAIL: %s smoke count is wrong\n", k_names[shot]);
                ++failures;
            }
            if (want_sparks != (s.sparks > 0)) {
                std::printf("FAIL: %s spark count is wrong\n", k_names[shot]);
                ++failures;
            }
            write_ppm(dir, k_names[shot]);
        }
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
