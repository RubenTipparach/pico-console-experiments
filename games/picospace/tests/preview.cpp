// Renders real Pico Space Program frames on the host, through the real engine
// and the real generated models, and writes them as PPM files. This is how the
// game gets looked at and tuned without a device in hand, and where the
// thumbnail comes from.
//
// It also checks the things only the renderer can answer, because the sim
// cannot ask it anything:
//
//   the horizon sits where the altitude says it should, at every scale from a
//   rocket on a pad to a planet seen from orbit, which is the one promise the
//   whole zoom scheme is built on;
//
//   the landing legs appear exactly when the booster is gone, and never
//   through it;
//
//   the prograde marker on the ring points the way the sim says the ship is
//   travelling, which nothing else can see because the marker is plotted
//   straight into the framebuffer;
//
//   and a frame never overflows the triangle queue.
//
// Usage: picospace_preview [out_dir]

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

std::vector<uint8_t>& buffer() {
    static std::vector<uint8_t> b(static_cast<size_t>(k_w) * k_h * 3);
    return b;
}

void draw(const ps::World& world, psr::View view, const std::string& path) {
    pse::RenderTarget target{buffer().data(), k_w, k_h, k_w * 3,
                             pse::PixelFormat::rgb888};
    psr::render_scene(world, target, view, g_clock);
    if (!path.empty()) write_ppm(path.c_str(), buffer().data());
}

void capture(const ps::World& world, psr::View view, const std::string& out,
             const char* name) {
    const std::string path = out + "/" + name;
    for (int f = 0; f < 3; f++) {
        g_clock += 33;
        draw(world, view, "");
    }
    draw(world, view, path);
    const psr::FrameStats s = psr::last_frame_stats();
    if (s.queued > g_worst_queued) g_worst_queued = s.queued;
    if (s.dropped > g_worst_dropped) g_worst_dropped = s.dropped;
    std::printf("wrote %s (alt %d, %u tris%s, horizon %d)\n", path.c_str(),
                ps::altitude_m(world), static_cast<unsigned>(s.queued),
                s.dropped ? ", DROPPED" : "", s.horizon_y);
}

// ---- flying the ship far enough to photograph it ---------------------------

int32_t wrap_s(int32_t a) {
    a %= ps::k_turn;
    if (a > ps::k_turn / 2) a -= ps::k_turn;
    if (a <= -ps::k_turn / 2) a += ps::k_turn;
    return a;
}

void aim(const ps::World& w, ps::Input& in, int32_t want) {
    const int32_t err = wrap_s(want - w.angle / ps::k_fp16);
    if (err > 2) in.left = true;
    if (err < -2) in.right = true;
}

// Climb until the apoapsis reaches a target, on the same gravity turn a player
// flies: nose up off the pad, tipping over with altitude, staging when the
// booster runs dry.
void ascend(ps::World& w, int32_t target_ap_m, int max_ticks) {
    for (int i = 0; i < max_ticks && ps::flying(w); i++) {
        const ps::Elements el = ps::elements(w);
        if (el.closed && el.apoapsis_m - ps::k_home_radius_m >= target_ap_m) {
            return;
        }
        ps::Input in{};
        in.up = w.throttle < 255;
        const int32_t alt = ps::altitude_m(w);
        int32_t want = ps::k_turn / 4;
        if (alt > 900) {
            const int32_t t = alt > 42000 ? 1024 : (alt - 900) * 1024 / 41100;
            want = 1024 - t;
        }
        aim(w, in, want);
        if (w.stage == 0 && w.fuel_kg <= 0) in.stage = true;
        ps::world_tick(w, in);
    }
}

void coast(ps::World& w, int ticks) {
    for (int i = 0; i < ticks && ps::flying(w); i++) {
        ps::Input in{};
        in.down = w.throttle > 0;
        ps::world_tick(w, in);
    }
}

// ---- the checks ------------------------------------------------------------

// The horizon promise, and it is the load bearing one. The flight view draws
// the ship at a fixed size and scales the world so the ground under the ship
// lands a constant distance below it. If that is wrong the picture is still a
// picture, just at the wrong scale, and nothing else in the game would notice.
void check_horizon(ps::World& w, const char* what) {
    draw(w, psr::View::Flight, "");
    const psr::FrameStats s = psr::last_frame_stats();
    if (s.horizon_y < 0) {
        std::printf("FAIL: %s: no horizon in frame (alt %d)\n", what,
                    ps::altitude_m(w));
        g_failures++;
        return;
    }
    // The world is scaled so the ground under the ship is a constant distance
    // below it, and the camera then tilts by altitude, so the row it lands on
    // moves within a band rather than sitting on one number. What has to hold
    // is that the ground is BELOW the ship and in frame at every scale from a
    // rocket on a pad to a planet seen from orbit, which is the promise the
    // whole zoom scheme is built on.
    //
    // Below k_scale_floor_m the world stops zooming in, on purpose: a rocket
    // six metres up would otherwise be drawn four times life size against its
    // own pad. The band is wider down there for that reason and no other.
    const int drop = s.horizon_y - s.ship_y;
    const bool close_in = ps::altitude_m(w) < 28;
    std::printf("  %-16s alt %8d  horizon %3d rows under the ship "
                "(scale %.4f m/unit)\n",
                what, ps::altitude_m(w), drop,
                65536.0 / (s.world_scale_fp16 ? s.world_scale_fp16 : 1));
    if (drop <= 0 || drop > (close_in ? 60 : 52) ||
        (!close_in && drop < 30)) {
        std::printf("FAIL: %s: horizon %d rows under the ship\n", what, drop);
        g_failures++;
    }
}

void check_prograde(const ps::World& w, const char* what) {
    draw(w, psr::View::Flight, "");
    const psr::FrameStats s = psr::last_frame_stats();
    if (s.prograde_x < 0) {
        std::printf("FAIL: %s: no prograde marker\n", what);
        g_failures++;
        return;
    }
    // The marker is a point on a ring in the ship's own plane, seen by a
    // camera that is above that plane, so its screen offset is the local
    // direction with the vertical squashed by the tilt. That rules out
    // comparing screen angles: the squash is real and it is the point.
    //
    // What does survive the projection is the SIGN of each component, and
    // that is what this checks, because every way the marker has actually
    // gone wrong shows up in a sign. Forgetting to rotate the velocity into
    // the ship's local frame leaves the marker pointing at the world's axes,
    // which is wrong by the ship's bearing: a quarter of an orbit later it
    // points at the ground. Flipping screen y leaves it pointing at the
    // retrograde dot.
    const double dir = (ps::prograde_angle(w) - ps::bearing_from(w, w.ref_body))
                       * 2 * 3.14159265358979 / ps::k_turn;
    const double want_x = std::sin(dir);      // along the track
    const double want_y = std::cos(dir);      // away from the body
    const double got_x = s.prograde_x - s.ship_x;
    const double got_y = s.ship_y - s.prograde_y;   // screen y runs down
    const double reach = std::sqrt(got_x * got_x + got_y * got_y);
    std::printf("  %-16s prograde local (%+.2f,%+.2f), marker (%+.0f,%+.0f) "
                "at %.0f px\n", what, want_x, want_y, got_x, got_y, reach);

    // A component near zero has no sign worth checking: the marker is rounded
    // to a pixel, and half a pixel either side of centre is not an error.
    if (std::fabs(want_x) > 0.25 && want_x * got_x <= 0.0) {
        std::printf("FAIL: %s: marker is on the wrong side along the track\n",
                    what);
        g_failures++;
    }
    if (std::fabs(want_y) > 0.25 && want_y * got_y <= 0.0) {
        std::printf("FAIL: %s: marker is the wrong side of the ship\n", what);
        g_failures++;
    }
    if (reach < 6.0 || reach > 34.0) {
        std::printf("FAIL: %s: marker %.0f px from the ship, not on the ring\n",
                    what, reach);
        g_failures++;
    }
}

void check_stages(const ps::World& w, bool want_booster, const char* what) {
    draw(w, psr::View::Flight, "");
    const psr::FrameStats s = psr::last_frame_stats();
    if (s.booster_drawn != want_booster) {
        std::printf("FAIL: %s: booster %s\n", what,
                    s.booster_drawn ? "still drawn" : "missing");
        g_failures++;
    }
    // The legs reach through where the first stage is, so they are the exact
    // complement of it: never both, never neither.
    if (s.legs_drawn == s.booster_drawn) {
        std::printf("FAIL: %s: legs and booster both %s\n", what,
                    s.legs_drawn ? "drawn" : "hidden");
        g_failures++;
    }
}

}  // namespace

int main(int argc, char** argv) {
    const std::string out = argc > 1 ? argv[1] : ".";

    ps::World w;
    ps::world_init(w, ps::Mission::Pip);

    capture(w, psr::View::Flight, out, "preview_0_pad.ppm");
    check_horizon(w, "on the pad");
    check_stages(w, true, "on the pad");

    // Light it and hold the nose up: the frame every launch opens with.
    for (int i = 0; i < 260 && ps::flying(w); i++) {
        ps::Input in{};
        in.up = w.throttle < 255;
        ps::world_tick(w, in);
    }
    capture(w, psr::View::Flight, out, "preview_1_liftoff.ppm");
    check_horizon(w, "lifting off");

    ascend(w, 12000, 20000);
    capture(w, psr::View::Flight, out, "preview_2_climb.ppm");
    check_horizon(w, "in the climb");
    check_prograde(w, "in the climb");

    ascend(w, 80000, 40000);
    capture(w, psr::View::Flight, out, "preview_3_cutoff.ppm");
    check_horizon(w, "at cutoff");
    check_stages(w, false, "after staging");

    coast(w, 6000);
    capture(w, psr::View::Flight, out, "preview_4_coast.ppm");
    check_horizon(w, "coasting");
    check_prograde(w, "coasting");
    capture(w, psr::View::Map, out, "preview_5_map.ppm");

    // The moon, and a ship on it. Placed rather than flown: the sim tests fly
    // the whole mission, and repeating it here would cost the preview several
    // hundred thousand ticks to reach a frame it can photograph directly.
    ps::World moon;
    ps::world_init(moon, ps::Mission::Pip);
    moon.stage = 1;
    moon.fuel_kg = 180 * ps::k_fp8;
    moon.grounded = false;
    moon.landed_on = ps::kBodyCount;
    {
        int32_t mx, my;
        ps::body_position(moon, ps::kPip, mx, my);
        const int32_t r = ps::k_bodies[ps::kPip].radius_m + 400;
        // Multiplied, not shifted: mx is negative half the time and shifting
        // a negative left is undefined behaviour, which the sanitizer says so
        // about and a release build quietly does something else with.
        moon.x = (static_cast<int64_t>(mx) + r) * ps::k_fp16;
        moon.y = static_cast<int64_t>(my) * ps::k_fp16;
        int32_t bvx, bvy;
        ps::body_velocity(moon, ps::kPip, bvx, bvy);
        moon.vx = bvx;
        moon.vy = bvy - 14 * ps::k_fp16;      // dropping toward the surface
        moon.angle = 0;                        // nose out along +x, which is up
        (void)0;
        moon.throttle = 90;
        moon.ref_body = ps::reference_body(moon);
    }
    capture(moon, psr::View::Flight, out, "preview_6_descent.ppm");
    check_horizon(moon, "over Pip");
    check_stages(moon, false, "over Pip");
    capture(moon, psr::View::Map, out, "preview_7_moon_map.ppm");

    std::printf("worst frame: %u triangles, %u dropped\n",
                static_cast<unsigned>(g_worst_queued),
                static_cast<unsigned>(g_worst_dropped));
    if (g_worst_dropped > 0) fail("a frame overflowed the triangle queue");

    if (g_failures) {
        std::printf("%d preview check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("preview checks passed\n");
    return 0;
}
