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

// Every test below is about the RACE, not the grid, so they all start from the
// green light. race_init holds the pod on the line for a three second
// countdown now, and a test that ticks two hundred times and then asks how far
// the pod has travelled would otherwise be measuring the countdown.
void race_start(Race& race, int track_index, int racer_index) {
    race_init(race, track_index, racer_index);
    const Input idle{};
    while (race.phase == Phase::Countdown) race_tick(race, idle);
}

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
        race_start(race, ti, 0);
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
    race_start(race, 0, 0);
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
    race_start(race, 0, 0);
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
    race_start(race, 0, 0);
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

void test_the_cables_are_attached_to_something() {
    // Reported from looking at the screenshots: the cables did not come from
    // the pod, they met in the middle and joined nothing.
    //
    // They were strung between a FIXED point on the pod's centreline and a
    // point 0.59 units inboard of each engine centre. The first does not
    // follow the cockpit, which swings up to two units out to the side, so at
    // full swing the pod end was 2.4 units from the cockpit: further than the
    // cockpit is long. The second grazed a hull of radius 0.62 at best.
    //
    // Both ends are anchored off the parts' drawn positions now, in each
    // part's own frame, and render_stats reports how far the worst anchor sits
    // outside the hull it belongs to. This drives a whole lap so the check
    // covers every attitude the pod actually reaches, not just a straight.
    const Track& t = track(0);
    Race race;
    race_start(race, 0, 0);
    Input in{};
    Chrome chrome;
    chrome.screen = Screen::Race;
    float worst = 0.0f;
    int32_t peak_swing = 0;
    for (int i = 0; i < 3000; ++i) {
        drive(race, t, in);
        race_tick(race, in);
        const int32_t sw = race.pod.swing < 0 ? -race.pod.swing : race.pod.swing;
        if (sw > peak_swing) peak_swing = sw;
        if (i % 50 != 0) continue;
        render_frame(race, chrome, target());
        if (render_stats().cable_gap > worst) worst = render_stats().cable_gap;
    }
    check(worst <= 0.0f, "every cable anchor is inside the hull it attaches to");
    // And the lap really did swing the cockpit, or the check above proves
    // nothing: a pod that never swung would pass it trivially.
    check(peak_swing > 3000, "the cockpit swung hard enough for this to matter");
    std::printf("  worst cable anchor sits %.2f units outside its hull, "
                "peak swing %.0f degrees\n",
                worst, peak_swing * 360.0 / 65536.0);
}

void test_the_sea_is_drawn_where_it_is_driven() {
    // The sim's waterline and the renderer's are the same number out of the
    // same header, and this is what says so from the outside: over a submerged
    // stretch there has to BE sea in the frame, and the pod has to be sitting
    // on top of what is drawn rather than beside it.
    //
    // Worth a test of its own because the failure is silent. A shoreline that
    // never gets drawn and a planet with no water look identical in a still
    // frame of open blue, and the first version of this drew plain sea over
    // the submerged road: the racing line simply stopped existing for a third
    // of the lap and every frame still looked like a seascape.
    int tide = -1;
    for (int i = 0; i < k_track_count; ++i) if (has_water(track(i))) tide = i;
    check(tide >= 0, "a track has a sea to draw");
    if (tide < 0) return;
    const Track& t = track(tide);

    Race race;
    race_start(race, tide, 0);
    Input in{};
    Chrome chrome;
    chrome.screen = Screen::Race;
    int wet_frames = 0, sprayed = 0, sampled = 0;
    int peak_sea = 0, peak_spray = 0;
    for (int i = 0; i < 6000; ++i) {
        drive(race, t, in);
        race_tick(race, in);
        if (i % 60 || i < 200) continue;
        render_frame(race, chrome, target());
        ++sampled;
        if (render_stats().sea > 0) ++wet_frames;
        if (render_stats().spray > 0) ++sprayed;
        if (render_stats().sea > peak_sea) peak_sea = render_stats().sea;
        if (render_stats().spray > peak_spray) peak_spray = render_stats().spray;
        // Whenever the pod is over water it must be throwing some, because
        // that is the one cue that says the surface under it is not road.
        if (race.pod.over_water && race.pod.grounded) {
            check(render_stats().spray > 0, "a pod over water throws spray");
        }
    }
    check(wet_frames * 4 > sampled, "a good part of the lap has sea in frame");
    check(sprayed > 0, "the pod runs over water often enough to throw spray");
    std::printf("  %s: sea in %d of %d sampled frames (peak %d quads), "
                "spray in %d (peak %d)\n",
                t.name, wet_frames, sampled, peak_sea, sprayed, peak_spray);

    // And a dry planet draws none of it, which is what the sentinel is for.
    for (int ti = 0; ti < k_track_count; ++ti) {
        if (ti == tide) continue;
        Race dry;
        race_start(dry, ti, 0);
        Input drive_in{};
        for (int i = 0; i < 900; ++i) { drive(dry, track(ti), drive_in); race_tick(dry, drive_in); }
        render_frame(dry, chrome, target());
        check(render_stats().sea == 0 && render_stats().spray == 0,
              "a track with no water draws no water");
    }
}

void test_the_binder_arc_moves() {
    // Reported from playing it: the arc between the engines needed to be more
    // animated. It was a fixed parabola redrawn identically every frame, which
    // is a picture of lightning rather than lightning.
    //
    // Checked by rendering two frames one tick apart with the pod held still,
    // so the ONLY thing that can differ is the arc itself. A frame difference
    // proves it moved; the second half proves it moved and stayed attached,
    // since a strand whose ends wander is a strand joined to nothing, which is
    // the mistake the cables already made once.
    const Track& t = track(0);
    Race race;
    race_start(race, 0, 0);
    Input in{};
    Chrome chrome;
    chrome.screen = Screen::Race;
    for (int i = 0; i < 600; ++i) { drive(race, t, in); race_tick(race, in); }

    // Frozen: same pod, same camera, consecutive ticks.
    uint8_t first[120 * 120 * 3];
    int moved = 0;
    float worst_gap = 0.0f;
    for (int k = 0; k < 6; ++k) {
        race.ticks = 4000 + k;
        render_frame(race, chrome, target());
        if (render_stats().cable_gap > worst_gap) worst_gap = render_stats().cable_gap;
        if (k == 0) {
            std::memcpy(first, g_pixels, sizeof(first));
        } else {
            int diff = 0;
            for (size_t i = 0; i < sizeof(first); ++i)
                if (first[i] != g_pixels[i]) ++diff;
            if (diff > 0) ++moved;
        }
    }
    check(moved >= 4, "the arc looks different from tick to tick");
    check(worst_gap <= 0.0f, "and the pod is still welded together while it does");
    std::printf("  the arc redrew differently on %d of 5 consecutive ticks\n", moved);
}

void test_the_drawn_ground_is_the_driven_ground() {
    // Reported from playing the desert: "the pod keeps sinking into the ground
    // when I go off road." It was, and the sim was innocent. There were two
    // descriptions of the world's cross section and they did not match.
    //
    //   The shoulder fell three units over TWELVE of width in surface_at and
    //   over three and a bit in the renderer, so just off the tarmac the drawn
    //   ground was up to two units under the surface the field held the pod on.
    //
    //   A walled stretch was drawn as a four unit kerb while surface_at
    //   returned road level, so the pod hovered inside a ledge it could see.
    //
    //   Every band took its normal from its own outgoing segment, so no two
    //   strips shared an edge: a wedge of open sky between them on the outside
    //   of every corner and a wedge of overlap on the inside, the road included.
    //
    //   And the plain reached fourteen half widths, 133 units, on a circuit
    //   whose tightest corner has a radius of 46. Past the radius consecutive
    //   nodes' offsets cross and a strip folds back over the road, drawn at its
    //   OWN node's height over ground belonging to a corner the pod has not
    //   reached. Measured, that put the drawn ground 7.5 units above the pod at
    //   38 of 197 sampled positions.
    //
    // ground_offset() is the single description now, the normals are mitred,
    // the plain is clamped short of the fold, and this walks every track asking
    // both halves the same question at the same point.
    for (int ti = 0; ti < k_track_count; ++ti) {
        const Track& t = track(ti);
        float worst_high = -1e9f, worst_low = 1e9f;
        int buried = 0, bare = 0, inside_rock = 0, samples = 0;
        for (uint16_t i = 0; i < t.node_count; i += 3) {
            const TrackNode& n = t.nodes[i];
            if (n.flags & kGap) continue;          // a hole, checked below
            const TrackNode& b = t.nodes[(i + 1) % t.node_count];
            const int32_t head = fatan2(node_x(b) - node_x(n), node_z(b) - node_z(n));
            const int32_t half = node_half_width(n);
            // Half a node along the segment. A sample exactly on a node lands
            // on the line where two cells meet, and which of them answers is
            // then a rounding question rather than a geometry one.
            const int32_t mx = (node_x(n) + node_x(b)) / 2;
            const int32_t mz = (node_z(n) + node_z(b)) / 2;
            for (int k = 0; k < 12; ++k) {
                // Out to the railing and no further, because the railing is
                // where the pod stops: sixteen units past the edge, against a
                // barrier at eighteen. The shoulder is fully down at twelve, so
                // this is the whole of the ground a pod that ran wide is on.
                //
                // Further out the answer stops being well defined anyway:
                // HOARFROST and TIDEBREAK both run back alongside themselves
                // inside eighty units, so the ground between two carriageways
                // belongs to both, while surface_at deliberately only searches
                // a window of nodes around the pod and can only name one.
                const int32_t over = fp(2) + fp(14) * (k / 2) / 5;
                const int32_t off = (half + over) * ((k & 1) ? 1 : -1);
                const int32_t x = mx + ftrig(off, fcos(head));
                const int32_t z = mz - ftrig(off, fsin(head));
                const Surface s = surface_at(t, i, x, z);
                if (s.y < fp(-1000)) continue;
                // Inside a canyon wall is not somewhere the pod can be: the sim
                // pushes it out every tick it tries.
                if (s.wall) { ++inside_rock; continue; }
                float dy = 0.0f;
                if (!drawn_ground(t, i, x, z, dy)) { ++bare; continue; }
                const float gap = dy - s.y / 65536.0f;
                if (gap > worst_high) worst_high = gap;
                if (gap < worst_low) worst_low = gap;
                // The pod floats k_hover_height above the surface, so anything
                // drawn higher than that is scenery the pod is inside of.
                if (gap > k_hover_height / 65536.0f) ++buried;
                ++samples;
            }
        }
        check(buried == 0, "the pod is never drawn inside the ground off the road");
        check(bare == 0, "there is ground drawn everywhere the pod can hover");
        // And the drawn ground must not sag under the driven one either, or the
        // pod floats over a visible hole with its own shadow nowhere near it.
        check(worst_low > -1.2f, "the drawn ground does not sag under the driven one");
        check(samples > 500, "enough of the track was actually sampled");
        std::printf("  %-10s off road, drawn minus driven: %+.2f to %+.2f over %d "
                    "samples (%d inside rock), buried %d, no ground %d\n",
                    t.name, worst_low, worst_high, samples, inside_rock, buried, bare);
    }
}

void test_a_hole_in_the_road_is_drawn_as_a_hole() {
    // The other half of the same report: "the gaps are missing geometry for the
    // cliff walls." surface_at has always said a gap has no surface at ANY
    // distance to the side, which makes it a canyon across the world rather
    // than a pit in the tarmac. Nothing drew that. The plain was laid straight
    // across, so going round the jump meant flying over solid desert and
    // falling through it, and going at the jump meant watching the road stop at
    // nothing at all.
    int chasms = 0;
    for (int ti = 0; ti < k_track_count; ++ti) {
        const Track& t = track(ti);
        int checked = 0;
        for (uint16_t i = 0; i < t.node_count; ++i) {
            if (!(t.nodes[i].flags & kGap)) continue;
            // Inside the run, not at its lips: the lip is where the road ends,
            // and the road is allowed to reach it.
            if (!(t.nodes[(i + 1) % t.node_count].flags & kGap)) continue;
            if (!(t.nodes[(i + t.node_count - 1) % t.node_count].flags & kGap)) continue;
            const TrackNode& n = t.nodes[i];
            // A gap under water is not a gap, it is more water.
            if (has_water(t) && node_y(n) < water_level(t)) continue;
            const TrackNode& b = t.nodes[(i + 1) % t.node_count];
            const int32_t head = fatan2(node_x(b) - node_x(n), node_z(b) - node_z(n));
            const int32_t mx = (node_x(n) + node_x(b)) / 2;
            const int32_t mz = (node_z(n) + node_z(b)) / 2;
            static const int32_t k_offsets[4] = {fp(0), fp(9), fp(20), fp(38)};
            for (int32_t off : k_offsets) {
                const int32_t x = mx + ftrig(off, fcos(head));
                const int32_t z = mz - ftrig(off, fsin(head));
                float dy = 0.0f;
                check(!drawn_ground(t, i, x, z, dy),
                      "nothing solid is drawn over a hole in the road");
                ++checked;
            }
        }
        chasms += checked;
        if (checked) std::printf("  %-10s %d points inside its chasms, none drawn "
                                 "as ground\n", t.name, checked);
    }
    check(chasms > 0, "there are chasms to check");

    // And the walls are actually emitted, on every frame where one is in view.
    const Track& t = track(0);
    Race race;
    race_start(race, 0, 0);
    Input in{};
    Chrome chrome;
    chrome.screen = Screen::Race;
    int gap_node = -1;
    for (uint16_t i = 0; i < t.node_count; ++i)
        if (t.nodes[i].flags & kGap) { gap_node = i; break; }
    check(gap_node >= 0, "the desert has a jump");
    if (gap_node < 0) return;
    int with_cliffs = 0, frames = 0;
    for (int i = 0; i < 5000; ++i) {
        drive(race, t, in);
        race_tick(race, in);
        const int ahead = ((gap_node - race.pod.node) % t.node_count
                           + t.node_count) % t.node_count;
        if (ahead > 10 || race.pod.wreck_ticks) continue;
        render_frame(race, chrome, target());
        ++frames;
        if (render_stats().cliffs > 0) ++with_cliffs;
    }
    check(frames > 0, "the drive reached the jump");
    check(with_cliffs == frames, "every frame approaching the jump draws its walls");
    std::printf("  %d of %d frames approaching the desert jump drew cliff faces\n",
                with_cliffs, frames);
}

void test_pod_select_shows_the_pod() {
    // Six racers described by a name, a pilot and six bars, and nothing about
    // the thing being chosen. Two of the six fly a different engine mesh and
    // all six are a different colour, and none of that reached the one screen
    // where the choice is made.
    //
    // Three claims, and all three are things a screenshot of one frame cannot
    // settle: that a pod is drawn at all, that it TURNS, and that picking a
    // different racer shows a different pod.
    Race race;
    race_start(race, 0, 0);
    Chrome chrome;
    chrome.screen = Screen::PodSelect;

    // The band the pod has to live in: under the name plate, over the stats.
    // Both of those are laid out in draw_pod_select and this is the number that
    // says the third thing on the screen fits between them.
    constexpr int k_name_plate = 25;
    constexpr int k_stats_top = 84;

    int highest = 120, lowest = 0;
    uint32_t previous_hash = 0;
    int moved = 0;
    for (int step = 0; step < 12; ++step) {
        chrome.time_ms = static_cast<uint32_t>(step) * 5200u / 12u;
        render_frame(race, chrome, target());
        check(render_stats().triangles > 20,
              "the pod select screen draws a pod and not a picture of one");
        if (render_stats().showcase_top < highest) highest = render_stats().showcase_top;
        if (render_stats().showcase_bottom > lowest) lowest = render_stats().showcase_bottom;
        uint32_t hash = 2166136261u;
        for (size_t i = 0; i < sizeof(g_pixels); ++i)
            hash = (hash ^ g_pixels[i]) * 16777619u;
        if (step && hash != previous_hash) ++moved;
        previous_hash = hash;
    }
    check(moved == 11, "it turns: every step of the revolution is a new frame");
    check(highest >= k_name_plate, "the pod stays clear of the name plate");
    check(lowest < k_stats_top, "and clear of the stat bars");
    std::printf("  pod select: the turn sweeps rows %d to %d, inside %d..%d\n",
                highest, lowest, k_name_plate, k_stats_top);

    // And the six racers do not all look the same. Compared at a fixed angle,
    // so what differs is the pod and not the moment.
    uint32_t seen[k_racer_count];
    for (int i = 0; i < k_racer_count; ++i) {
        chrome.pod = static_cast<uint8_t>(i);
        chrome.time_ms = 900;
        render_frame(race, chrome, target());
        uint32_t hash = 2166136261u;
        for (size_t k = 0; k < sizeof(g_pixels); ++k)
            hash = (hash ^ g_pixels[k]) * 16777619u;
        seen[i] = hash;
        for (int j = 0; j < i; ++j)
            check(seen[j] != hash, "each racer's pod select screen is its own");
    }
    std::printf("  all %d racers render a distinct pod\n", k_racer_count);
}

void test_the_rocks_belong_to_the_track() {
    // Reported from playing it: "spikes on the side of the track don't seem to
    // be stable in position, that thing pops around the screen every frame."
    //
    // They were picked at fixed offsets AHEAD OF THE POD, `pod.node + 0, 7,
    // 14`, so the three rocks in view were pinned to the pod rather than to the
    // ground. Every time the pod crossed a node boundary, five times a second,
    // all three jumped to different nodes and were rebuilt from a different
    // hash: a different side, a different distance out, a different size. They
    // did not move, they teleported.
    //
    // A rock belongs to a node now, and the test is exactly that: whatever the
    // pod is doing, every rock drawn stands on a node the TRACK chose.
    for (int ti = 0; ti < k_track_count; ++ti) {
        const Track& t = track(ti);
        Race race;
        race_start(race, ti, 0);
        Input in{};
        Chrome chrome;
        chrome.screen = Screen::Race;
        int frames = 0, drawn = 0;
        for (int i = 0; i < 1400; ++i) {
            drive(race, t, in);
            race_tick(race, in);
            if (i % 17 || i < 100) continue;
            render_frame(race, chrome, target());
            ++frames;
            drawn += render_stats().props;
            for (int k = 0; k < render_stats().props; ++k) {
                check(render_stats().prop_node[k] % 7 == 0,
                      "every rock stands on a node the track chose, not one the "
                      "pod chose");
            }
        }
        check(drawn > frames, "there are rocks out there to check");
        std::printf("  %-10s %d rocks over %d frames, all on track nodes\n",
                    t.name, drawn, frames);
    }
}

void test_going_under_the_sea_looks_like_going_under_the_sea() {
    // The sim taking the pod down is only half of it. The geometry was right
    // as soon as it did: poly re-winds every triangle toward the camera, so the
    // sea is drawn from below as happily as from above, and a probe colour
    // showed it correctly overhead receding to a horizon. The frame still read
    // as open air, because the underside of the sea was painted in the same
    // blues as the sky and the gradient behind everything was still a sky.
    //
    // So this measures the LOOK, at the top of the frame, which is where the
    // difference lives: above water that band is sky, under it that band is
    // the sea's underside and has to be darker.
    int tide = -1;
    for (int i = 0; i < k_track_count; ++i) if (has_water(track(i))) tide = i;
    check(tide >= 0, "a track has a sea");
    if (tide < 0) return;
    const Track& t = track(tide);
    const int32_t sea = water_level(t);

    // How much of the frame has had the red taken out of it, which is what
    // being under water DOES to a picture. Brightness was the obvious measure
    // and it is useless here: TIDEBREAK's sky and its sunk water come out
    // within one luma step of each other, so a frame correctly tinted from top
    // to bottom scored 97 against the dry frame's 96 and the check failed on a
    // working build.
    //
    // Water absorbs red first, which is why the sink halves it and lifts the
    // other two. Nothing on a dry frame of this track is blue by that margin.
    // How much of the frame is still wearing the SKY's own colour. Under the
    // sea the answer has to be none of it: the gradient behind everything is
    // deep water, and the ceiling over the pod is the sea's underside.
    //
    // Sampling one pixel and asking how far it is from the sky was tried and
    // is too weak to matter: the pixel it lands on is the sea ceiling, which
    // differs from the sky whether or not the tint ran, so the check passed
    // with the underwater gradient switched off entirely.
    const auto sky_pixels = [&t]() {
        int n = 0;
        for (int i = 0; i < 120 * 120; ++i) {
            const uint8_t* p = g_pixels + i * 3;
            if (std::abs(p[0] - t.palette.sky_top[0])
              + std::abs(p[1] - t.palette.sky_top[1])
              + std::abs(p[2] - t.palette.sky_top[2]) < 12) ++n;
        }
        return n;
    };
    // Pixels too RED to have come through the sink. It keeps three tenths of
    // the red, so a tinted polygon cannot exceed 76 whatever it was painted;
    // anything above that underwater is either the HUD, which is drawn
    // immediate mode afterwards and deliberately not tinted, or the pod's
    // mesh, which goes through draw_mesh rather than poly.
    //
    // A threshold derived from the constant beats comparing mean red between
    // the two frames, which was tried: they are different places on the track
    // with different scenery, so 58 against 36 was mostly the view and only
    // partly the water.
    const auto too_red = []() {
        int n = 0;
        for (int i = 0; i < 120 * 120; ++i) if (g_pixels[i * 3] > 76) ++n;
        return n;
    };

    uint16_t deepest = 0, high = 0;
    for (uint16_t i = 0; i < t.node_count; ++i) {
        if (node_y(t.nodes[i]) < node_y(t.nodes[deepest])) deepest = i;
        if (node_y(t.nodes[i]) > node_y(t.nodes[high])) high = i;
    }

    Chrome chrome;
    chrome.screen = Screen::Race;
    chrome.track = static_cast<uint8_t>(tide);
    int luma[2] = {0, 0}, red[2] = {0, 0};
    for (int k = 0; k < 2; ++k) {
        const uint16_t at = k ? deepest : high;
        Race race;
        race_start(race, tide, 0);
        const TrackNode& n = t.nodes[at];
        race.pod.node = at;
        race.pod.x = node_x(n);
        race.pod.z = node_z(n);
        race.pod.y = node_y(n) + k_hover_floor;
        Input coast{};
        for (int i = 0; i < 12; ++i) race_tick(race, coast);
        check(race.pod.submerged == (k == 1),
              "the pod is under the sea at the deep end and not at the high one");
        render_frame(race, chrome, target());
        luma[k] = sky_pixels();
        red[k] = too_red();
        if (k == 1) check(render_stats().sea > 0, "the sea is drawn from under it");
    }
    check(luma[0] > 200, "above the water there is sky in the picture");
    check(luma[1] == 0, "and under it there is none: the sky is gone entirely");
    check(red[1] * 4 < red[0],
          "and the whole picture has had the red taken out of it");
    std::printf("  under: %d sky pixels above water and %d below, "
                "%d pixels too red to be underwater against %d\n",
                luma[0], luma[1], red[0], red[1]);
}

void test_the_pod_explodes_when_the_fuse_runs_out() {
    // The requirement: when the break up countdown reaches zero the pod should
    // explode. It reached zero and printed WRECKED, with the intact pod still
    // being drawn, tumbling gently through its own non-existent fireball.
    //
    // Pixels, not draw calls, for exactly the reason the spark test above
    // spells out: a billboard that projects to nothing submits happily and
    // paints nothing. And one extra thing that only matters here, because the
    // hull is deliberately not drawn during a wreck: an explosion that fails
    // to rasterize is not a missing effect on a visible pod, it is an empty
    // road with a camera tracking a hole in the air, and a test that only
    // checked "the pod is gone" would pass on that.
    // NIGHTJAR, on purpose, and not the SCARAB the rest of these tests fly.
    // The hull has to be told apart from the desert AND from the fireball, and
    // Scarab's livery is {214, 124, 40}: amber, which is both. Nightjar is
    // purple, which is neither, so "is the pod on screen" is answerable.
    const int k_racer = 4;
    const Track& t = track(0);
    Race race;
    race_start(race, 0, k_racer);
    Input in{};
    Chrome chrome;
    chrome.screen = Screen::Race;
    for (int i = 0; i < 300; ++i) { drive(race, t, in); race_tick(race, in); }

    // The hull, by the RELATIONSHIP between its channels rather than by
    // matching its colour. Matching the colour was the obvious way and it does
    // not work: the mesh is lit, so what lands on screen is the livery scaled
    // by a shading factor, and measured, an exact match with a generous
    // tolerance found zero hull pixels on a frame with the pod plainly in the
    // middle of it. That detector would have made "the hull is gone during the
    // wreck" pass on a frame where the pod never left.
    //
    // Flat shading multiplies all three channels together, so the ORDER
    // survives it. Nightjar is blue dominant with red over green; the desert
    // is red over green over blue, and the fire is red dominant with almost no
    // blue. Nothing else on this track is blue dominant.
    //
    // Counting triangles instead was tried and is worse than useless here: the
    // explosion submits its own quads, so the wrecked frame drew THREE MORE
    // triangles than the intact one and the difference said nothing about the
    // hull at all.
    const auto hull_pixels = []() {
        int n = 0;
        for (int i = 0; i < 120 * 120; ++i) {
            const uint8_t* px = g_pixels + i * 3;
            if (px[2] > px[0] + 18 && px[0] > px[1] + 12) ++n;
        }
        return n;
    };
    // Fire: hot white through orange. Nothing on DUNE SEA is this bright and
    // this saturated, which is the whole reason the blast reads at 120 pixels.
    const auto fire_pixels = []() {
        int n = 0;
        for (int i = 0; i < 120 * 120; ++i) {
            const uint8_t* px = g_pixels + i * 3;
            if (px[0] > 230 && px[1] > 130 && px[1] < 255 && px[2] < 140) ++n;
        }
        return n;
    };

    // Two renders of one sim state: the pod intact, then the same pod with the
    // wreck clock running. Everything else in the frame is identical, so the
    // whole difference between them is the pod.
    render_frame(race, chrome, target());
    const int hull_before = hull_pixels();
    check(hull_before >= 20, "the pod is on screen before it goes");
    check(render_stats().boom == 0, "and nothing is exploding yet");
    const int16_t saved = race.pod.wreck_ticks;
    race.pod.wreck_ticks = k_respawn_ticks / 2;
    render_frame(race, chrome, target());
    check(hull_pixels() == 0,
          "the hull stops being drawn the moment the pod is wrecked");
    check(render_stats().boom > 0, "and the explosion is drawn instead");
    race.pod.wreck_ticks = saved;

    // Lose an engine and let the fuse burn all the way down.
    race.pod.engine[0] = 0;
    race.pod.dead = 1;
    Input coast{};
    int guard = 0;
    while (race.pod.wreck_ticks == 0 && guard++ < 600) race_tick(race, coast);
    check(race.pod.wreck_ticks > 0, "the fuse ran out and the pod wrecked");

    // The flash's own white, which the debris never wears. Both are hot, but
    // the flash core is {255, 250, 226} and the hottest debris is
    // {255, 232, 168}: the blue channel is the whole difference, and without
    // it a test cannot tell a fireball from a shower of embers. Switching the
    // flash off entirely left every other check in this test green.
    const auto flash_pixels = []() {
        int n = 0;
        for (int i = 0; i < 120 * 120; ++i) {
            const uint8_t* px = g_pixels + i * 3;
            if (px[0] > 240 && px[1] > 235 && px[2] > 200) ++n;
        }
        return n;
    };

    int fire_peak = 0, drawn = 0, frames = 0, hull_seen = 0;
    int flash_peak = 0, thin = 0;
    for (int i = 0; race.pod.wreck_ticks > 0 && i < 400; ++i) {
        render_frame(race, chrome, target());
        ++frames;
        // Every frame, not a sample: the wreck runs a second and a half and a
        // check at the flash alone would pass for an effect that is one frame
        // long and then leaves the camera on an empty road.
        if (render_stats().boom > 0) ++drawn;
        // And SUBSTANCE, not just presence. Counting "more than nothing" let a
        // mutant through that cut the debris off after one frame: the trailing
        // smoke alone kept the counter above zero for the whole wreck, so an
        // explosion reduced to seven drifting puffs read as fully working.
        if (render_stats().boom < 10) ++thin;
        if (hull_pixels() > 0) ++hull_seen;
        const int fire = fire_pixels();
        if (fire > fire_peak) fire_peak = fire;
        const int white = flash_pixels();
        if (white > flash_peak) flash_peak = white;
        race_tick(race, coast);
    }
    check(frames > 100, "the wreck lasts long enough to see");
    check(drawn == frames, "something is drawn on every frame of it");
    check(hull_seen == 0, "and the hull is gone for all of it, not just the flash");
    // Measured at zero on a working build, so the margin is slack rather than
    // a tuned number. An explosion cut down to its trailing smoke runs about
    // 150 thin frames.
    check(thin <= 2, "the wreck stays an explosion, not a few drifting puffs");
    // Eight pixels was enough for a spark. A pod coming apart is the biggest
    // event in the game and has to be bigger than one spark.
    check(fire_peak >= 60, "the fireball is a fireball, not a speck");
    // Measured at 162 px. Zero when the flash is switched off, which every
    // other check in this test survived.
    check(flash_peak >= 40, "and it starts with a flash, not just embers");
    std::printf("  boom: %d frames of wreck, all drawn (%d thin), fire peaks at "
                "%d px, flash core %d px, hull %d px before and %d after\n",
                frames, thin, fire_peak, flash_peak, hull_before, hull_seen);
}

void test_scraping_a_wall_throws_sparks() {
    // Reported from playing it: crashing into a wall popped the pod up over
    // the wall, and nothing on screen said the pod was grinding along it. The
    // sim has always charged for the scrape, so a player on a walled circuit
    // could shed a hundred health a second with no cue but a bar in the corner.
    //
    // Three things to prove, and none of them is "the code ran". The first
    // version of this test counted the quads the renderer submitted, which was
    // eight every scraping frame and told us nothing: at a tenth of a unit
    // across they projected to under a pixel from the chase camera, so the
    // rasterizer painted NONE of them and the test was green over an effect
    // that was invisible. So: count pixels the sparks actually landed, check
    // they come off the side against the rock, and check the pod stayed down
    // at the foot of the wall instead of riding up over it.
    const Track& t = track(3);   // HOARFROST, 624 units of canyon
    Race race;
    race_start(race, 3, 0);
    Input in{};
    Chrome chrome;
    chrome.screen = Screen::Race;

    // The two spark colours, which are nobody's livery and nothing else on a
    // frozen circuit is anywhere near.
    const auto spark_pixels = []() {
        const int hot[3] = {255, 236, 176}, cool[3] = {255, 150, 70};
        int n = 0;
        for (int i = 0; i < 120 * 120; ++i) {
            const uint8_t* px = g_pixels + i * 3;
            int dh = 0, dc = 0;
            for (int k = 0; k < 3; ++k) {
                dh += std::abs(px[k] - hot[k]);
                dc += std::abs(px[k] - cool[k]);
            }
            if (dh < 70 || dc < 70) ++n;
        }
        return n;
    };

    int scraped = 0, sparked = 0, wrong_side = 0, high = 0, peak = 0;
    for (int i = 0; i < 3000; ++i) {
        drive(race, t, in);
        // Steer into whichever wall is nearer once there is a wall to hit.
        const Surface s = surface_at(t, race.pod.node, race.pod.x, race.pod.z);
        if (node_wall(t.nodes[s.node]) > 0) {
            in.left = s.lateral < 0;
            in.right = !in.left;
        }
        race_tick(race, in);
        if (i % 3) continue;
        render_frame(race, chrome, target());
        if (!race.pod.scraping) continue;
        ++scraped;
        check(render_stats().sparks > 0, "a scrape submits sparks to draw");
        const int lit = spark_pixels();
        if (lit > 0) ++sparked;
        if (lit > peak) peak = lit;
        // The side the sim says is grinding has to be the side the pod ran out
        // of road on, which is the sign of its offset from the centreline.
        if ((race.pod.scrape > 0) != (s.lateral > 0)) ++wrong_side;
        // And the pod stays down at the foot of the wall rather than riding up
        // over it. The wall is eleven units; half of it is the report.
        //
        // MEASURED AGAINST THE ROAD, and after the tick. It used to be the
        // pod's height above `s`, which is the surface sampled BEFORE the tick
        // at the position the pod had before it moved: on a slope the two
        // disagree by a couple of units for reasons that have nothing to do
        // with walls, and one frame in eighty five duly reported a pod riding
        // up a wall while the sim had it 1.97 units off the ground. The road at
        // the node is a fixed reference, and being up on the plateau is
        // precisely being high above THAT.
        const Surface after = surface_at(t, race.pod.node, race.pod.x, race.pod.z);
        const float over =
            (race.pod.y - node_y(t.nodes[after.node])) / 65536.0f;
        if (over > k_wall_height / 65536.0f * 0.5f) ++high;
    }
    check(scraped > 20, "the pod really did grind along a wall");
    check(sparked * 10 >= scraped * 9, "a scrape puts sparks on the screen");
    check(peak >= 8, "the sparks are big enough to see, not sub pixel");
    check(wrong_side == 0, "the sparks come off the side against the rock");
    check(high == 0, "scraping a wall does not lift the pod over it");
    std::printf("  HOARFROST  %d scraping frames, %d lit sparks (peak %d px), "
                "%d on the wrong side, %d rode up the wall\n",
                scraped, sparked, peak, wrong_side, high);
}

void test_the_world_is_closed_when_you_look_off_the_side() {
    // Reported from playing it: off the track there was "all this area" with
    // nothing in it. There was, and it is geometry rather than taste.
    //
    // The drawn plain stops forty six units past the road edge and nothing is
    // drawn beyond it: what covers the view out there is two ridges pinned to
    // the CAMERA, whose feet sat twenty six units below it. So a ray leaving
    // the pod downward hit the plain only if it landed inside the plain, and
    // hit a ridge only if it was shallower than that foot, and between those
    // two angles was a wedge that hit nothing. The wedge OPENS AS THE POD
    // CLIMBS: at pod height it is a sliver that reads as haze, and measured
    // from twenty six units up, which is one jump, it was 41 to 54 percent of
    // the lower half of the frame on all four circuits.
    //
    // Dropping those feet to sixty units costs nothing, because it is the same
    // two quads per column drawn taller, and every height above them gained
    // the same amount so the skyline is unchanged to the pixel.
    //
    // The check is the two numbers meeting: how high a pilot actually gets
    // above the plain, driven rather than assumed, against the height the
    // geometry stays closed to. It is done this way because the obvious test
    // cannot be written here: counting sky pixels needs sky and ground to be
    // different colours, and on three of these four circuits they are not.
    for (int ti = 0; ti < k_track_count; ++ti) {
        const Track& t = track(ti);
        Race race;
        race_start(race, ti, 0);
        Input in{};
        float highest = 0.0f;
        for (int i = 0; i < 8000; ++i) {
            drive(race, t, in);
            race_tick(race, in);
            if (race.pod.wreck_ticks) continue;
            const float over = (race.pod.y - node_y(t.nodes[race.pod.node])
                                + k_shoulder_drop) / 65536.0f;
            // A pod in freefall down a chasm is not a pod looking at scenery.
            if (over > highest && over < 60.0f) highest = over;
        }
        const float closes = sky_closes_below(t);
        check(highest > 4.0f, "the pod got airborne enough for this to mean something");
        check(closes > highest,
              "the world has no hole in it from any height a pilot reaches");
        std::printf("  %-10s a pilot reaches %.1f u over the plain, the world is "
                    "closed to %.1f\n", t.name, highest, closes);
    }
}

void test_no_band_of_ground_runs_backwards() {
    // A strip of ground is a quad between one node's boundary point and the
    // next node's. If the outer point advances the OTHER way to the road, the
    // two ends have crossed and the quad is a bowtie: it folds through itself,
    // draws over ground it does not own at the wrong height, and leaves a
    // wedge of nothing beside it. From a pod that has wandered off the road
    // that reads as a hole in the world, which is exactly how it was reported.
    //
    // The clamp meant to prevent this measured the reach PAST THE ROAD EDGE
    // against a radius measured from the CENTRELINE, so the road's own half
    // width was spent twice, and it only ever looked at one node when a strip
    // has two ends. 33 folds across the four circuits, and on HOARFROST one
    // band ran back along itself almost exactly.
    //
    // The check is the definition: every boundary point on both sides of every
    // node has to advance with the road, not against it.
    for (int ti = 0; ti < k_track_count; ++ti) {
        const Track& t = track(ti);
        float worst = 1.0f;
        int worst_node = 0;
        for (int i = 0; i < t.node_count; ++i) {
            const int j = (i + 1) % t.node_count;
            const float cx =
                (node_x(t.nodes[j]) - node_x(t.nodes[i])) / 65536.0f;
            const float cz =
                (node_z(t.nodes[j]) - node_z(t.nodes[i])) / 65536.0f;
            const float cl = std::sqrt(cx * cx + cz * cz);
            if (cl < 0.0001f) continue;
            for (int s = 0; s < 2; ++s) {
                const float side = s ? 1.0f : -1.0f;
                GroundSlice a, b;
                ground_slice(t, i, side, a);
                ground_slice(t, j, side, b);
                const float* pa[6] = {a.base, a.lip, a.shoulder, a.verge,
                                      a.rail, a.plain};
                const float* pb[6] = {b.base, b.lip, b.shoulder, b.verge,
                                      b.rail, b.plain};
                for (int k = 0; k < 6; ++k) {
                    const float dx = pb[k][0] - pa[k][0];
                    const float dz = pb[k][2] - pa[k][2];
                    const float dl = std::sqrt(dx * dx + dz * dz);
                    if (dl < 0.0001f) continue;
                    const float dot = (cx * dx + cz * dz) / (cl * dl);
                    if (dot < worst) { worst = dot; worst_node = i; }
                }
            }
        }
        check(worst > 0.0f,
              "no band of ground runs backwards against the road it borders");
        std::printf("  %-10s worst band heads %+.2f with the road, at node %d\n",
                    t.name, worst, worst_node);
    }
}

}  // namespace

int main() {
    test_the_projector_only_ever_sees_small_numbers();
    test_near_geometry_is_cut_and_not_dropped();
    test_there_is_ground_when_the_pod_runs_wide();
    test_the_parts_of_the_pod_disagree_in_a_corner();
    test_the_cables_are_attached_to_something();
    test_the_sea_is_drawn_where_it_is_driven();
    test_the_binder_arc_moves();
    test_the_drawn_ground_is_the_driven_ground();
    test_a_hole_in_the_road_is_drawn_as_a_hole();
    test_pod_select_shows_the_pod();
    test_the_rocks_belong_to_the_track();
    test_scraping_a_wall_throws_sparks();
    test_going_under_the_sea_looks_like_going_under_the_sea();
    test_the_pod_explodes_when_the_fuse_runs_out();
    test_the_world_is_closed_when_you_look_off_the_side();
    test_no_band_of_ground_runs_backwards();

    if (g_failures) {
        std::printf("%d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("twinflare render tests passed\n");
    return 0;
}
