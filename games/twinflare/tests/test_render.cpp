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
    race_init(race, 0, 0);
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
    race_init(race, tide, 0);
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
        race_init(dry, ti, 0);
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
    race_init(race, 0, 0);
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
                // Out to twenty four units past the edge. The shoulder is fully
                // down at twelve, so this is the whole of the ground a pod that
                // ran wide is on and a fair margin past it. Further out than
                // this the answer stops being well defined at all: HOARFROST
                // and TIDEBREAK both run back alongside themselves inside
                // eighty units, so the ground between two carriageways belongs
                // to both, while surface_at deliberately only searches a window
                // of nodes around the pod and can only ever name one of them.
                const int32_t over = fp(2) + fp(22) * (k / 2) / 5;
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
    race_init(race, 0, 0);
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
    race_init(race, 0, 0);
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

    if (g_failures) {
        std::printf("%d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("twinflare render tests passed\n");
    return 0;
}
