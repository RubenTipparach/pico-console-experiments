// Host tests for Twin Flare's sim.
//
// Every one of these exists because something was wrong. The fixed point port
// of a flight model that already worked in floating point shipped four bugs,
// and every one of them presented as "the pod behaves oddly" rather than as a
// crash, so none would have been caught by anything that only checked the
// build:
//
//   the hover spring was seven hundred times too weak (two factors of the tick
//   rate), so the pod sank through the middle of its own road at walking pace;
//
//   the wall impulse used ftrig on an fp16 vector and then multiplied by four
//   to compensate, which did not cancel, so HOARFROST accelerated itself to
//   19,000 units a second off its own scenery;
//
//   the lift term was about two hundred times gravity, and stayed hidden until
//   something finally pitched the nose up over a gap;
//
//   the drag coefficient overflowed int32 whenever the air brake was out, so
//   braking ACCELERATED the pod, on three tracks out of four.
//
// So the tests below are mostly about bounds and units rather than gameplay.

#include <cstdio>
#include <cstdlib>

#include "fixed.hpp"
#include "sim.hpp"

using namespace twinflare;

namespace {

int g_failures = 0;

void check(bool ok, const char* what) {
    if (!ok) {
        std::printf("FAIL: %s\n", what);
        ++g_failures;
    }
}

// The same damped, braking driver the design was measured with. It is here
// rather than in the test bodies because every test wants a pod that is
// actually racing, and "hold throttle and never steer" is a wall test.
void drive(const Race& race, const Track& t, Input& in) {
    const Pod& p = race.pod;
    const int n = t.node_count;
    const TrackNode& here = t.nodes[p.node];
    const TrackNode& target = t.nodes[(p.node + 7) % n];
    int32_t want = fatan2(node_x(target) - p.x, node_z(target) - p.z);
    want -= clamp32(p.lateral / 96, -3000, 3000);
    const int32_t err = angle_diff(want, p.yaw) - (p.yaw_rate >> k_rate_fp) * 42 / 100;
    in.left = err < -400;
    in.right = err > 400;

    const TrackNode& next = t.nodes[(p.node + 1) % n];
    const int32_t h0 = fatan2(node_x(next) - node_x(here), node_z(next) - node_z(here));
    const TrackNode& a = t.nodes[(p.node + 14) % n];
    const TrackNode& b = t.nodes[(p.node + 15) % n];
    const int32_t bend = angle_diff(fatan2(node_x(b) - node_x(a), node_z(b) - node_z(a)), h0);
    const int32_t sharp = bend < 0 ? -bend : bend;
    in.brake = sharp > 2600 && pod_speed(p) > fscale(pod_top_speed(p), 500);
    in.throttle = !in.brake;
    in.up = (t.nodes[(p.node + 3) % n].flags & kGap) != 0;
    in.repair = p.engine[0] < p.engine_max / 2 || p.engine[1] < p.engine_max / 2;
}

// ---------------------------------------------------------------------------

void test_trig_is_accurate() {
    // The sine table is built at compile time and interpolated, and the whole
    // sim's geometry rests on it.
    check(fsin(0) == 0, "sin 0");
    check(fsin(k_turn / 4) >= k_trig_one - 2, "sin quarter turn is 1");
    check(fsin(k_turn / 2) <= 2 && fsin(k_turn / 2) >= -2, "sin half turn is 0");
    check(fsin(-k_turn / 4) <= -(k_trig_one - 2), "sin minus quarter is -1");
    check(fcos(0) >= k_trig_one - 2, "cos 0 is 1");
    // Pythagoras, everywhere round the circle.
    for (int32_t a = 0; a < k_turn; a += 97) {
        const int32_t s = fsin(a), c = fcos(a);
        const int32_t sum = (s * s + c * c) >> k_trig_fp;
        check(sum > k_trig_one - 40 && sum < k_trig_one + 40, "sin^2 + cos^2 is 1");
    }
    // atan2 round trips a heading.
    for (int32_t a = 0; a < k_turn; a += 311) {
        const int32_t x = ftrig(fp(300), fsin(a));
        const int32_t z = ftrig(fp(300), fcos(a));
        const int32_t back = fatan2(x, z);
        const int32_t err = angle_diff(back, a);
        check(err > -60 && err < 60, "atan2 round trips a heading");
    }
}

void test_lengths_never_go_negative() {
    // flength shifts a rooted square back up, and an overflow there reads as a
    // negative speed, which is how the drag bug first showed itself.
    check(flength(0, 0) == 0, "length of nothing");
    check(flength(fp(3), fp(4)) > fp(4) && flength(fp(3), fp(4)) < fp(6),
          "3,4,5 triangle");
    check(flength(fp(2000), fp(2000)) > 0, "length of a big vector is positive");
}

void test_every_track_is_a_closed_ring() {
    for (int i = 0; i < k_track_count; ++i) {
        const Track& t = track(i);
        check(t.node_count > 100, "track has nodes");
        check(t.laps >= 2, "track has laps");
        // Consecutive nodes are one spacing apart, including the wrap, which
        // is what "closed" means here. An unclosed ring would put a step in
        // the road at the start line that nothing else would report.
        for (uint16_t n = 0; n < t.node_count; ++n) {
            const TrackNode& a = t.nodes[n];
            const TrackNode& b = t.nodes[(n + 1) % t.node_count];
            const int32_t d = flength(node_x(b) - node_x(a), node_z(b) - node_z(a));
            check(d > k_node_spacing / 2 && d < k_node_spacing * 2,
                  "nodes are one spacing apart the whole way round");
        }
    }
}

void test_every_corner_is_takeable() {
    // The generator relaxes each shape until nothing is under its target. This
    // is the check that the relaxed output is what actually shipped: an
    // authored shape that quietly stopped being relaxed would produce a track
    // with a corner no pod can turn, and the only symptom would be that
    // everybody crashes there.
    for (int i = 0; i < k_track_count; ++i) {
        const Track& t = track(i);
        int32_t tightest = INT32_MAX;
        for (uint16_t n = 0; n < t.node_count; ++n) {
            const TrackNode& a = t.nodes[n];
            const TrackNode& b = t.nodes[(n + 1) % t.node_count];
            const TrackNode& c = t.nodes[(n + 2) % t.node_count];
            const int32_t h0 = fatan2(node_x(b) - node_x(a), node_z(b) - node_z(a));
            const int32_t h1 = fatan2(node_x(c) - node_x(b), node_z(c) - node_z(b));
            const int32_t turn = angle_diff(h1, h0);
            const int32_t mag = turn < 0 ? -turn : turn;
            if (mag < 8) continue;
            const int32_t radius = (k_node_spacing / k_one) * 10430 / mag;
            if (radius < tightest) tightest = radius;
        }
        check(tightest >= 30, "no corner is tighter than the pod can turn");
    }
}

void test_a_pod_settles_on_the_road() {
    // The hover field is a spring that only ever pushes up. Dropped on the
    // road with no throttle, a pod should settle near the rest height and
    // stay there. It sank straight through, once.
    Race race;
    race_init(race, 0, 0);
    Input in{};
    for (int i = 0; i < 400; ++i) race_tick(race, in);
    const int32_t clear = race.pod.clearance;
    check(clear > fp(1) && clear < fp(4), "a parked pod hovers");
    check(race.pod.wreck_ticks == 0, "a parked pod does not wreck");
}

void test_speed_never_runs_away() {
    // The single most valuable check here. Three separate unit errors all
    // presented as a velocity that grew without bound, and every one of them
    // was invisible until something happened to exercise it: braking, or a
    // wall, or a nose lifted over a gap. This drives every track with every
    // pod, using all of those, and asserts the pod stays inside physics.
    const int32_t ceiling = fp(4);   // 400 units a second, absurd
    for (int ti = 0; ti < k_track_count; ++ti) {
        const Track& t = track(ti);
        for (int ri = 0; ri < k_racer_count; ++ri) {
            Race race;
            race_init(race, ti, ri);
            Input in{};
            for (int i = 0; i < 3000; ++i) {
                drive(race, t, in);
                race_tick(race, in);
                const Pod& p = race.pod;
                const bool sane =
                    p.vx < ceiling && p.vx > -ceiling &&
                    p.vy < ceiling && p.vy > -ceiling &&
                    p.vz < ceiling && p.vz > -ceiling;
                if (!sane) {
                    std::printf("  runaway on %s with %s at tick %d: v=(%d,%d,%d)\n",
                                t.name, racer(ri).name, i, p.vx, p.vy, p.vz);
                    check(false, "velocity stays inside physics");
                    return;
                }
            }
        }
    }
}

void test_the_pod_gets_round() {
    // A track nobody can finish is not a hard track. The autopilot is crude
    // and brakes bluntly, so this is a floor and not a target.
    for (int ti = 0; ti < k_track_count; ++ti) {
        const Track& t = track(ti);
        Race race;
        race_init(race, ti, 0);
        Input in{};
        for (int i = 0; i < 12000; ++i) {
            drive(race, t, in);
            race_tick(race, in);
        }
        check(race.pod.lap >= 2, "a lap can be completed inside two minutes");
        check(race.best_lap > 0, "a lap time is recorded");
    }
}

void test_one_engine_pulls_the_pod_off_line() {
    // "Flying with one engine will inevitably cause the ship to crash." That
    // is not enforced by a timer anywhere: it falls out of the surviving
    // engine's thrust acting off the pod's centreline. If it ever stops
    // falling out, this is what says so.
    Race race;
    race_init(race, 0, 0);
    Input in{};
    in.throttle = true;
    for (int i = 0; i < 300; ++i) race_tick(race, in);
    const int32_t straight_yaw = race.pod.yaw;

    Race hurt_race;
    race_init(hurt_race, 0, 0);
    hurt_race.pod.engine[0] = 0;
    hurt_race.pod.dead = 1;
    Input hurt_in{};
    hurt_in.throttle = true;
    for (int i = 0; i < 300; ++i) race_tick(hurt_race, hurt_in);

    const int32_t drift = angle_diff(hurt_race.pod.yaw, straight_yaw);
    check(drift < -200, "one engine yaws the pod toward its dead side");
}

void test_damage_pulls_before_it_kills() {
    // The pull starts before the engine dies, in proportion to the damage,
    // which is what turns a health bar into something arriving through the
    // stick.
    Race race;
    race_init(race, 0, 0);
    race.pod.engine[0] = static_cast<int16_t>(race.pod.engine_max / 5);
    Input in{};
    in.throttle = true;
    const int32_t start = race.pod.yaw;
    for (int i = 0; i < 200; ++i) race_tick(race, in);
    check(angle_diff(race.pod.yaw, start) < -50,
          "a weakened engine pulls the pod toward itself");
}

void test_boost_is_gated_on_speed() {
    // The Thrust Meter reads current speed, not a charge. A double tap from a
    // standstill has to do nothing, or the mechanic is just a button.
    Race race;
    race_init(race, 0, 0);
    check(!boost_armed(race.pod), "boost is not armed from a standstill");

    // Driven until the gate opens, rather than sampled at one arbitrary tick.
    // The driver brakes for a fifth of the lap, so "after N ticks" lands
    // mid-braking often enough that a fixed count tests the corner it happened
    // to stop on rather than the mechanic.
    const Track& t = track(0);
    Input in{};
    int armed_at = -1;
    for (int i = 0; i < 3000 && armed_at < 0; ++i) {
        drive(race, t, in);
        race_tick(race, in);
        if (boost_armed(race.pod)) armed_at = i;
    }
    check(armed_at >= 0, "boost arms once the pod is up to speed");
    if (armed_at < 0) return;

    // And a double tap while it is armed lights it.
    in.boost_press = true;
    race_tick(race, in);
    race_tick(race, in);
    check(race.pod.boost_ticks > 0, "a double tap at speed lights the boost");

    // A single press, far apart, does not: that is what makes it a double tap
    // rather than a boost button with extra steps.
    Race single;
    race_init(single, 0, 0);
    Input sin_{};
    for (int i = 0; i < 3000 && !boost_armed(single.pod); ++i) {
        drive(single, t, sin_);
        race_tick(single, sin_);
    }
    sin_.boost_press = true;
    race_tick(single, sin_);
    sin_.boost_press = false;
    for (int i = 0; i < k_double_tap_ticks + 5; ++i) race_tick(single, sin_);
    sin_.boost_press = true;
    const int16_t before = single.pod.boost_ticks;
    race_tick(single, sin_);
    check(single.pod.boost_ticks <= before,
          "two presses further apart than the window are not a double tap");
}

void test_overheating_costs_both_engines() {
    Race race;
    race_init(race, 0, 0);
    race.pod.heat = k_heat_one - 4;
    // Boosting, or the heat simply sheds and the redline never arrives: the
    // gauge only climbs while the boost is lit.
    race.pod.boost_ticks = 200;
    const int16_t before[2] = {race.pod.engine[0], race.pod.engine[1]};
    Input in{};
    for (int i = 0; i < 100; ++i) race_tick(race, in);
    check(race.pod.engine[0] < before[0], "overheating burns the left engine");
    check(race.pod.engine[1] < before[1], "overheating burns the right engine");
    check(race.pod.locked, "a redline locks the boost out");
}

void test_repair_cannot_resurrect() {
    // A repair beam is not a spare part.
    Race race;
    race_init(race, 0, 0);
    race.pod.engine[0] = 0;
    race.pod.dead = 1;
    race.pod.engine[1] = 100;
    Input in{};
    in.repair = true;
    for (int i = 0; i < 200; ++i) race_tick(race, in);
    check(race.pod.engine[0] == 0, "a dead engine stays dead");
    check(race.pod.engine[1] > 100, "a live engine is repaired");
}

void test_a_gap_is_a_hole_and_not_a_wall() {
    // A gap carries no surface, so a pod that drives into one falls. If a gap
    // ever started reporting ground, the jump would silently become a straight.
    const Track& t = track(0);
    int gap_node = -1;
    for (uint16_t i = 0; i < t.node_count; ++i)
        if (t.nodes[i].flags & kGap) { gap_node = i; break; }
    check(gap_node >= 0, "the desert has a gap in it");
    if (gap_node < 0) return;
    const Surface s = surface_at(t, static_cast<uint16_t>(gap_node),
                                 node_x(t.nodes[gap_node]), node_z(t.nodes[gap_node]));
    check(!s.road, "a gap is not road");
    check(s.y < fp(-1000), "a gap has no surface to push against");
}

void test_every_gap_is_passable() {
    // The brief asks that the obstacles all be passable, so this measures it
    // rather than claiming it: the SLOWEST pod, launched off the end of the
    // road with no nose up at all, no glide and no skill, has to clear the
    // widest hole on every track.
    for (int ti = 0; ti < k_track_count; ++ti) {
        const Track& t = track(ti);
        int widest = 0, run = 0;
        for (uint16_t i = 0; i < t.node_count * 2; ++i) {
            if (t.nodes[i % t.node_count].flags & kGap) {
                if (++run > widest) widest = run;
            } else {
                run = 0;
            }
        }
        if (widest == 0) continue;

        // Get the slowest pod up to speed on the track it has to jump on.
        Race race;
        race_init(race, ti, 1);   // WISP, the lowest top speed on the roster
        Input in{};
        int32_t best = 0;
        for (int i = 0; i < 2000; ++i) {
            drive(race, t, in);
            race_tick(race, in);
            if (race.pod.wreck_ticks == 0 && pod_speed(race.pod) > best)
                best = pod_speed(race.pod);
        }
        // A ballistic arc off a ramp at that speed, at the track's gravity.
        const int32_t g = fscale(k_gravity, t.world.gravity);
        int32_t vy = fscale(best, 300), y = 0, reach = 0;
        for (int i = 0; i < 4000 && (i < 4 || y > 0); ++i) {
            vy -= g;
            y += vy;
            reach += best;
        }
        const int32_t gap_units = widest * (k_node_spacing / k_one);
        const int32_t reach_units = reach / k_one;
        check(reach_units > gap_units * 2,
              "the slowest pod clears the widest gap with margin");
        std::printf("  %-10s widest gap %3d u, worst case jump %4d u\n",
                    t.name, gap_units, reach_units);
    }
}

void test_state_is_small() {
    // Rule 8: budget everything. Star Dancer's whole world is 3,372 bytes, and
    // this is the number the PR body has to state.
    std::printf("  sizeof(Race) = %u bytes (pod %u, %d rivals at %u)\n",
                static_cast<unsigned>(sizeof(Race)),
                static_cast<unsigned>(sizeof(Pod)), k_rival_count,
                static_cast<unsigned>(sizeof(Rival)));
    check(sizeof(Race) < 1024, "the whole race fits in a kilobyte of RAM");
}

void test_tracks_cost_what_they_claim() {
    unsigned nodes = 0;
    for (int i = 0; i < k_track_count; ++i) nodes += track(i).node_count;
    const unsigned bytes = nodes * static_cast<unsigned>(sizeof(TrackNode));
    std::printf("  %u nodes across %d tracks = %u bytes of flash (%u per node)\n",
                nodes, k_track_count, bytes,
                static_cast<unsigned>(sizeof(TrackNode)));
    check(sizeof(TrackNode) == 8, "a node is eight bytes");
    check(bytes < 16u * 1024u, "the track tables stay under 16 KB");
}

}  // namespace

int main() {
    test_trig_is_accurate();
    test_lengths_never_go_negative();
    test_every_track_is_a_closed_ring();
    test_every_corner_is_takeable();
    test_a_pod_settles_on_the_road();
    test_speed_never_runs_away();
    test_the_pod_gets_round();
    test_one_engine_pulls_the_pod_off_line();
    test_damage_pulls_before_it_kills();
    test_boost_is_gated_on_speed();
    test_overheating_costs_both_engines();
    test_repair_cannot_resurrect();
    test_a_gap_is_a_hole_and_not_a_wall();
    test_every_gap_is_passable();
    test_state_is_small();
    test_tracks_cost_what_they_claim();

    if (g_failures) {
        std::printf("%d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("twinflare sim tests passed\n");
    return 0;
}
