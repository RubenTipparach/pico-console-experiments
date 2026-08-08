// Host tests for the parts of the renderer that a screenshot cannot show.
//
// All four of these exist because the game was played and four things were
// wrong with it, and none of the four is visible in a still frame:
//
//   geometry near the camera was HIDDEN rather than clipped, because the
//   engine drops a triangle whole when any corner crosses the near plane, and
//   the strip of road under the nose is exactly the one that does;
//
//   the position JITTERED, because every vertex was handed to the projector in
//   absolute world coordinates and Renderer3D projects in 1024 scale fixed
//   point, whose error grows with the magnitude going in, so a 2,400 unit lap
//   shimmered at one end and was steady at the other;
//
//   going off course found MISSING GEOMETRY, because the drawn ground ran out
//   at four half widths;
//
//   and the pod did not sway, because every part of it was drawn at the pod's
//   own yaw, so the two mass model the sim runs was invisible.

#include <cmath>
#include <cstdio>
#include <cstring>

#include "fixed.hpp"
#include "pse/pixel.hpp"
#include "render.hpp"
#include "sim.hpp"

using namespace twinflare;

namespace {

int g_failures = 0;
uint8_t g_pixels[120 * 120 * 3];

void check(bool ok, const char* what) {
    if (!ok) {
        std::printf("FAIL: %s\n", what);
        ++g_failures;
    }
}

pse::RenderTarget target() {
    return pse::RenderTarget{g_pixels, 120, 120, 120 * 3, pse::PixelFormat::rgb888};
}

void drive(const Race& race, const Track& t, Input& in) {
    const Pod& p = race.pod;
    const int n = t.node_count;
    const TrackNode& here = t.nodes[p.node];
    const TrackNode& tgt = t.nodes[(p.node + 7) % n];
    int32_t want = fatan2(node_x(tgt) - p.x, node_z(tgt) - p.z);
    want -= clamp32(p.lateral / 96, -3000, 3000);
    const int32_t e = angle_diff(want, p.yaw) - (p.yaw_rate >> k_rate_fp) * 42 / 100;
    in.left = e < -400;
    in.right = e > 400;
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

int fraction_painted(const uint8_t sky[3]) {
    int painted = 0;
    for (int i = 0; i < 120 * 120; ++i) {
        const uint8_t* px = g_pixels + i * 3;
        const int d = std::abs(px[0] - sky[0]) + std::abs(px[1] - sky[1])
                    + std::abs(px[2] - sky[2]);
        if (d > 24) ++painted;
    }
    return painted * 100 / (120 * 120);
}

// ---------------------------------------------------------------------------

void test_the_projector_only_ever_sees_small_numbers() {
    // The floating origin, stated as a number. Whatever corner of whatever
    // track the race is on, no coordinate handed to the projector may exceed
    // the far plane by much, because everything is camera relative. Absolute
    // coordinates on these tracks reach about 600.
    for (int ti = 0; ti < k_track_count; ++ti) {
        const Track& t = track(ti);
        Race race;
        race_init(race, ti, 0);
        Input in{};
        Chrome chrome;
        chrome.screen = Screen::Race;
        float worst = 0.0f;
        int32_t furthest = 0;
        for (int lap = 0; lap < 2400; ++lap) {
            drive(race, t, in);
            race_tick(race, in);
            if (lap % 200 != 0) continue;
            render_frame(race, chrome, target());
            if (render_stats().max_coordinate > worst)
                worst = render_stats().max_coordinate;
            const int32_t away = flength(race.pod.x, race.pod.z) / k_one;
            if (away > furthest) furthest = away;
        }
        check(worst < 260.0f,
              "no coordinate reaching the projector is far from the origin");
        std::printf("  %-10s pod reaches %4d u from world origin, "
                    "projector never sees past %5.0f\n", t.name, furthest, worst);
        // And the point of the test: the pod really did get a long way out, so
        // the bound above is not just an untravelled track.
        check(furthest > 150, "the pod travelled far enough for this to mean something");
    }
}

void test_near_geometry_is_cut_and_not_dropped() {
    // The road under the nose straddles the near plane constantly. Before the
    // clipper it was dropped whole, and the ground vanished from under the pod
    // whenever the camera got low or the pod pitched down. Now it is cut, so
    // polygons should be CLIPPED regularly and the frame should stay covered.
    const Track& t = track(0);
    Race race;
    race_init(race, 0, 0);
    Input in{};
    Chrome chrome;
    chrome.screen = Screen::Race;
    int clipped_frames = 0, worst_cover = 100;
    for (int i = 0; i < 1600; ++i) {
        drive(race, t, in);
        race_tick(race, in);
        if (i % 100 != 0 || i < 200) continue;
        render_frame(race, chrome, target());
        if (render_stats().clipped > 0) ++clipped_frames;
        const int cover = fraction_painted(t.palette.sky_top);
        if (cover < worst_cover) worst_cover = cover;
    }
    check(clipped_frames > 0,
          "the near plane actually cuts polygons rather than never firing");
    // Ground, pod and HUD together: a frame that is nearly all sky is a frame
    // where the world fell out of it.
    check(worst_cover > 35, "the world covers the frame while racing");
    std::printf("  clipping fired on %d sampled frames, worst coverage %d%%\n",
                clipped_frames, worst_cover);
}

void test_there_is_ground_when_the_pod_runs_wide() {
    // Off the road is a shoulder now, not a cliff, so there has to be
    // something drawn out there. The drawn plain used to stop at four half
    // widths, about thirty eight units, and a pod that ran wide flew over
    // nothing.
    const Track& t = track(0);
    Race race;
    race_init(race, 0, 0);
    Input in{};
    Chrome chrome;
    chrome.screen = Screen::Race;
    for (int i = 0; i < 400; ++i) { drive(race, t, in); race_tick(race, in); }

    // Put it well outside the road, beside the centreline rather than on it.
    const TrackNode& n = t.nodes[race.pod.node];
    const TrackNode& next = t.nodes[(race.pod.node + 1) % t.node_count];
    const int32_t head = fatan2(node_x(next) - node_x(n), node_z(next) - node_z(n));
    race.pod.x = node_x(n) + ftrig(fp(60), fcos(head));
    race.pod.z = node_z(n) - ftrig(fp(60), fsin(head));
    race.pod.y = node_y(n) + fp(3);
    for (int i = 0; i < 60; ++i) race_tick(race, in);

    render_frame(race, chrome, target());
    const int cover = fraction_painted(t.palette.sky_top);
    check(cover > 30, "there is ground drawn sixty units off the racing line");
    check(race.pod.wreck_ticks == 0, "running wide does not wreck the pod");
    std::printf("  sixty units wide: %d%% of the frame is not sky\n", cover);
}

void test_the_parts_of_the_pod_disagree_in_a_corner() {
    // The engines lead, the cockpit trails and swings out. Every part used to
    // be drawn at the pod's own yaw, so the whole vehicle rotated as one rigid
    // object and the sim's two mass model was invisible.
    //
    // The sim half is checkable directly: through a hard corner the swing must
    // be a real angle rather than a rounding error, and it must lag the yaw
    // rate rather than track it exactly.
    const Track& t = track(0);
    Race race;
    race_init(race, 0, 0);
    Input in{};
    for (int i = 0; i < 500; ++i) { drive(race, t, in); race_tick(race, in); }

    in.throttle = true;
    in.left = false;
    in.right = true;
    int32_t peak_swing = 0;
    for (int i = 0; i < 120; ++i) {
        race_tick(race, in);
        const int32_t s = race.pod.swing < 0 ? -race.pod.swing : race.pod.swing;
        if (s > peak_swing) peak_swing = s;
    }
    // A tenth of a radian is about six degrees of cockpit, which is visible on
    // a 120 pixel screen at this camera distance.
    check(peak_swing > 1000, "the cockpit swings a real angle through a corner");

    // And it keeps swinging after the stick centres, which is the feedback the
    // whole two mass model exists for: a rigid body would stop at once.
    in.right = false;
    const int32_t at_release = race.pod.swing;
    bool still_moving = false;
    for (int i = 0; i < 12; ++i) {
        race_tick(race, in);
        if (race.pod.swing_rate != 0 && race.pod.swing != at_release) still_moving = true;
    }
    check(still_moving, "the cockpit keeps moving after the stick centres");
    std::printf("  peak swing %d brads (%.1f degrees)\n",
                peak_swing, peak_swing * 360.0 / 65536.0);
}

}  // namespace

int main() {
    test_the_projector_only_ever_sees_small_numbers();
    test_near_geometry_is_cut_and_not_dropped();
    test_there_is_ground_when_the_pod_runs_wide();
    test_the_parts_of_the_pod_disagree_in_a_corner();

    if (g_failures) {
        std::printf("%d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("twinflare render tests passed\n");
    return 0;
}
