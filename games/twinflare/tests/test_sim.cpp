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

void test_running_wide_costs_time_and_not_the_run() {
    // Reported from playing it: going off course dropped the pod and killed
    // it. The ground beside the road fell away by up to thirty units and the
    // crash floor is twenty six, so drifting wide was fatal rather than slow.
    // A hover field that holds you over the road and drops you beside it is a
    // trapdoor, not a field.
    const Track& t = track(0);
    Race race;
    race_init(race, 0, 0);
    Input in{};
    for (int i = 0; i < 400; ++i) { drive(race, t, in); race_tick(race, in); }

    // Sixty units off the centreline, six times the road's half width.
    const TrackNode& n = t.nodes[race.pod.node];
    const TrackNode& next = t.nodes[(race.pod.node + 1) % t.node_count];
    const int32_t head = fatan2(node_x(next) - node_x(n), node_z(next) - node_z(n));
    race.pod.x = node_x(n) + ftrig(fp(60), fcos(head));
    race.pod.z = node_z(n) - ftrig(fp(60), fsin(head));
    race.pod.y = node_y(n) + fp(3);
    race.pod.yaw = head;

    in.throttle = true;
    in.left = in.right = in.brake = in.up = false;
    int32_t offroad_speed = 0;
    for (int i = 0; i < 300; ++i) {
        race_tick(race, in);
        check(race.pod.wreck_ticks == 0, "running wide does not wreck the pod");
        if (g_failures) return;
        offroad_speed = pod_speed(race.pod);
    }
    check(!race.pod.on_road, "the pod really is off the road for this test");
    check(race.pod.clearance > 0, "the pod still hovers over the rough");

    // And it is slower out there, which is the whole penalty. Measured on the
    // DRAG rather than by racing two pods: both start at the same speed with
    // the throttle shut and coast for a third of a second, so neither travels
    // far enough to reach a corner and the only difference between them is the
    // surface.
    //
    // Two earlier versions of this measured something else. One raced a
    // braking autopilot round corners against a straight line off road run and
    // concluded the rough was faster; the next held both straight for three
    // seconds, by which time the on road pod had driven off the road.
    Race rough, smooth;
    race_init(rough, 0, 0);
    race_init(smooth, 0, 0);
    Race* const pair[2] = {&rough, &smooth};
    for (Race* r : pair) {
        r->pod.node = race.pod.node;
        r->pod.x = node_x(n);
        r->pod.z = node_z(n);
        r->pod.yaw = head;
        r->pod.vx = ftrig(per_s(fp(70)), fsin(head));
        r->pod.vz = ftrig(per_s(fp(70)), fcos(head));
    }
    // Only the rough one is moved off the racing line.
    rough.pod.x += ftrig(fp(60), fcos(head));
    rough.pod.z -= ftrig(fp(60), fsin(head));

    // Each starts at ITS OWN rest height. The shoulder sits three units below
    // the road, so dropping the rough pod in at the road's height leaves it
    // six units up, which is above the hover field's reach: it was airborne,
    // not on the rough, and the surface it was being tested on never touched
    // it.
    for (Race* r : pair) {
        const Surface s = surface_at(t, r->pod.node, r->pod.x, r->pod.z);
        r->pod.y = s.y + k_hover_height;
    }

    Input coast{};
    for (int i = 0; i < 40; ++i) { race_tick(rough, coast); race_tick(smooth, coast); }
    check(smooth.pod.on_road, "the reference pod stayed on the road");
    check(!rough.pod.on_road, "the rough pod stayed off it");
    check(pod_speed(rough.pod) < pod_speed(smooth.pod),
          "the rough is slower than the road");
    std::printf("  coasting: %d u/s on the rough against %d u/s on the road\n",
                pod_speed(rough.pod) * k_tick_hz / k_one,
                pod_speed(smooth.pod) * k_tick_hz / k_one);
}

void test_only_a_gap_is_fatal() {
    // Falling is reserved for a hole in the road. If this ever stops being
    // true the jumps stop meaning anything.
    const Track& t = track(0);
    int gap_node = -1;
    for (uint16_t i = 0; i < t.node_count; ++i)
        if (t.nodes[i].flags & kGap) { gap_node = i; break; }
    if (gap_node < 0) return;

    Race race;
    race_init(race, 0, 0);
    const TrackNode& n = t.nodes[gap_node];
    race.pod.node = static_cast<uint16_t>(gap_node);
    race.pod.x = node_x(n);
    race.pod.z = node_z(n);
    race.pod.y = node_y(n) + fp(2);
    Input in{};
    bool died = false;
    for (int i = 0; i < 400 && !died; ++i) {
        race_tick(race, in);
        if (race.pod.wreck_ticks > 0) died = true;
    }
    check(died, "a pod parked over a gap falls and wrecks");
}

void test_the_pod_never_gets_below_the_surface() {
    // Reported from playing it: "I should never sink below the ground
    // whatsoever." The hover field was a spring and only a spring, so a hard
    // arrival pushed through the surface for a few ticks while the spring
    // caught up, which is exactly the thing a force field that always holds
    // you above the ground is supposed to make impossible.
    //
    // Every track, both engine shapes, a whole lap each, and the number this
    // asserts is the floor itself: while the field has hold of the pod, the
    // clearance may not go under it, ever, on any tick.
    for (int ti = 0; ti < k_track_count; ++ti) {
        const Track& t = track(ti);
        for (int ri = 0; ri < 2; ++ri) {
            Race race;
            race_init(race, ti, ri * 2);
            Input in{};
            int32_t worst = INT32_MAX;
            for (int i = 0; i < 6000; ++i) {
                drive(race, t, in);
                race_tick(race, in);
                if (race.pod.grounded && race.pod.wreck_ticks == 0
                    && race.pod.clearance < worst) {
                    worst = race.pod.clearance;
                }
            }
            check(worst >= k_hover_floor,
                  "the field never lets the pod under its own floor");
            if (ri == 0) {
                std::printf("  %-10s closest the pod came to the surface: "
                            "%.2f units (floor %.2f)\n", t.name,
                            worst / 65536.0, k_hover_floor / 65536.0);
            }
        }
    }
}

void test_a_hard_landing_costs_engines_and_not_the_run() {
    // The other half of the same report: "pod should instead take damage if it
    // hits the ground too hard." Instead of what it used to do, which was sink
    // through the road and wreck.
    //
    // Dropped from sixty units up onto the middle of the start straight, which
    // is a worse arrival than any ramp on any of the four tracks produces.
    const Track& t = track(0);
    Race race;
    race_init(race, 0, 0);
    Input in{};
    for (int i = 0; i < 300; ++i) { drive(race, t, in); race_tick(race, in); }

    const Surface s = surface_at(t, race.pod.node, race.pod.x, race.pod.z);
    race.pod.y = s.y + fp(60);
    race.pod.vy = 0;
    const int16_t before = race.pod.engine[0];

    int32_t worst = INT32_MAX;
    int32_t fastest_fall = 0;
    Input coast{};
    for (int i = 0; i < 240; ++i) {
        race_tick(race, coast);
        if (-race.pod.vy > fastest_fall) fastest_fall = -race.pod.vy;
        if (race.pod.grounded && race.pod.clearance < worst) worst = race.pod.clearance;
    }
    check(race.pod.wreck_ticks == 0, "a hard landing does not wreck the pod");
    check(worst >= k_hover_floor, "and it does not go through the surface");
    check(race.pod.engine[0] < before, "it costs the left engine health");
    check(race.pod.engine[1] < before, "and the right engine equally");
    check(race.pod.engine[0] == race.pod.engine[1],
          "a slam is symmetric: both engines hit the ground together");
    // Enough to be worth avoiding and not enough to end a race outright, which
    // is the whole point of moving the penalty from the run to the engines.
    const int lost = before - race.pod.engine[0];
    check(lost > 40, "the damage is felt");
    check(lost < race.pod.engine_max / 2, "and one bad landing is survivable");
    std::printf("  sixty unit drop: fell at %d u/s, cost %d of %d health an "
                "engine, closest to the road %.2f units\n",
                fastest_fall * k_tick_hz / k_one, lost, race.pod.engine_max,
                worst / 65536.0);
}

void test_the_sea_is_a_surface_and_not_a_hazard() {
    // TIDEBREAK's road runs from eighteen units under the waterline to twelve
    // above it, which is the track the brief asked for and is undrivable
    // unless water holds the pod up. It does: surface_at hands the hover field
    // the waterline whenever the rock is lower, so the pod skims the sea in
    // exactly the way it skims a road.
    int tide = -1;
    for (int i = 0; i < k_track_count; ++i) if (has_water(track(i))) tide = i;
    check(tide >= 0, "one track has a sea");
    if (tide < 0) return;
    const Track& t = track(tide);
    const int32_t sea = water_level(t);

    // Only one, and the sentinel really is a sentinel: three dry planets must
    // not quietly acquire a sea at y = -32768.
    int wet = 0;
    for (int i = 0; i < k_track_count; ++i) if (has_water(track(i))) ++wet;
    check(wet == 1, "exactly one track has a sea");

    // The deepest node on the circuit, which is where the difference between
    // a sea and a hole in the ground is thirteen units of drop.
    int deepest = 0;
    for (uint16_t i = 0; i < t.node_count; ++i)
        if (node_y(t.nodes[i]) < node_y(t.nodes[deepest])) deepest = i;
    check(node_y(t.nodes[deepest]) < sea - fp(8),
          "the deepest part of the circuit is well under the waterline");

    Race race;
    race_init(race, tide, 0);
    const TrackNode& n = t.nodes[deepest];
    race.pod.node = static_cast<uint16_t>(deepest);
    race.pod.x = node_x(n);
    race.pod.z = node_z(n);
    race.pod.y = sea + fp(14);      // dropped in from above
    race.pod.vx = race.pod.vz = race.pod.vy = 0;

    Input in{};
    int32_t lowest = INT32_MAX;
    for (int i = 0; i < 400; ++i) {
        race_tick(race, in);
        if (race.pod.y < lowest) lowest = race.pod.y;
    }
    check(race.pod.wreck_ticks == 0, "the sea does not drown a pod");
    check(race.pod.over_water, "and the pod knows it is over water");
    check(lowest >= sea, "the pod never gets below the waterline");
    check(race.pod.y < sea + k_hover_height + fp(1),
          "it settles ON the sea rather than hovering somewhere above it");
    std::printf("  %s: sea at %.1f, deepest road %.1f, pod settled at %.2f "
                "(lowest %.2f)\n", t.name, sea / 65536.0,
                node_y(t.nodes[deepest]) / 65536.0, race.pod.y / 65536.0,
                lowest / 65536.0);

    // And the same for a gap. A hole in the road with the sea underneath is a
    // splash, not a grave, and that follows from the one clamp in surface_at
    // rather than from a case anywhere else.
    int gap = -1;
    for (uint16_t i = 0; i < t.node_count; ++i)
        if ((t.nodes[i].flags & kGap) && node_y(t.nodes[i]) < sea) { gap = i; break; }
    if (gap >= 0) {
        Race fall;
        race_init(fall, tide, 0);
        fall.pod.node = static_cast<uint16_t>(gap);
        fall.pod.x = node_x(t.nodes[gap]);
        fall.pod.z = node_z(t.nodes[gap]);
        fall.pod.y = node_y(t.nodes[gap]) + fp(2);
        Input none{};
        for (int i = 0; i < 400; ++i) race_tick(fall, none);
        check(fall.pod.wreck_ticks == 0, "a gap over the sea is survivable");
        check(fall.pod.y >= sea, "and it lands you on the surface");
    }
}

void test_the_sea_costs_time() {
    // Water has to be a stretch of the circuit rather than a free one, or a
    // third of TIDEBREAK's lap is a rest. Two pods on the same track, one
    // running the submerged section and one the dry, coasting from the same
    // speed.
    int tide = -1;
    for (int i = 0; i < k_track_count; ++i) if (has_water(track(i))) tide = i;
    if (tide < 0) return;
    const Track& t = track(tide);
    const int32_t sea = water_level(t);

    int wet_node = -1, dry_node = -1;
    for (uint16_t i = 0; i < t.node_count; ++i) {
        const bool gap = (t.nodes[i].flags & (kGap | kWall)) != 0;
        if (gap) continue;
        if (wet_node < 0 && node_y(t.nodes[i]) < sea - fp(6)) wet_node = i;
        if (dry_node < 0 && node_y(t.nodes[i]) > sea + fp(8)) dry_node = i;
    }
    check(wet_node >= 0 && dry_node >= 0, "the circuit has both a sea and a causeway");
    if (wet_node < 0 || dry_node < 0) return;

    int32_t speed[2] = {0, 0};
    const int nodes[2] = {dry_node, wet_node};
    for (int k = 0; k < 2; ++k) {
        Race race;
        race_init(race, tide, 0);
        const TrackNode& n = t.nodes[nodes[k]];
        const TrackNode& ahead = t.nodes[(nodes[k] + 1) % t.node_count];
        const int32_t head = fatan2(node_x(ahead) - node_x(n), node_z(ahead) - node_z(n));
        race.pod.node = static_cast<uint16_t>(nodes[k]);
        race.pod.x = node_x(n);
        race.pod.z = node_z(n);
        race.pod.yaw = head;
        const Surface s = surface_at(t, race.pod.node, race.pod.x, race.pod.z);
        race.pod.y = s.y + k_hover_height;
        race.pod.vx = ftrig(per_s(fp(60)), fsin(head));
        race.pod.vz = ftrig(per_s(fp(60)), fcos(head));
        Input coast{};
        for (int i = 0; i < 60; ++i) race_tick(race, coast);
        speed[k] = pod_speed(race.pod);
        check(race.pod.over_water == (k == 1), "each pod is on the surface it was put on");
    }
    check(speed[1] < speed[0], "coasting over the sea is slower than the causeway");
    std::printf("  coasting: %d u/s on the causeway against %d u/s over the sea\n",
                speed[0] * k_tick_hz / k_one, speed[1] * k_tick_hz / k_one);
}

void test_rivals_move_smoothly() {
    // Reported from playing it: "the enemy racers are stuttering in their
    // movement, like a different update rate with no interpolation." They are
    // on the same 100 Hz tick as everything else, and they were still doing it.
    //
    // A rival's yaw was the heading of the SEGMENT it was on: a step function
    // that jumped at every node, once every eight units, five times a second at
    // racing speed. The yaw snapped, and because the lateral weave is measured
    // off that heading, the rival's POSITION jumped sideways with it.
    //
    // Measured as jerk: the change in a rival's per tick displacement. A rival
    // running a smooth line changes its step by a tiny fraction of the step
    // itself; one that teleports sideways shows a spike. Same for the yaw.
    for (int ti = 0; ti < k_track_count; ++ti) {
        const Track& t = track(ti);
        Race race;
        race_init(race, ti, 0);
        Input in{};
        int32_t worst_jerk = 0, worst_step = 0, worst_yaw_jerk = 0;
        int32_t last_dx = 0, last_dz = 0, last_turn = 0;
        int32_t px = race.rivals[0].x, pz = race.rivals[0].z;
        int32_t pyaw = race.rivals[0].yaw;
        for (int i = 0; i < 4000; ++i) {
            race_tick(race, in);
            const Rival& r = race.rivals[0];
            const int32_t dx = r.x - px, dz = r.z - pz;
            const int32_t turn = angle_diff(r.yaw, pyaw);
            if (i > 2) {
                const int32_t jerk = flength(dx - last_dx, dz - last_dz);
                if (jerk > worst_jerk) worst_jerk = jerk;
                const int32_t step = flength(dx, dz);
                if (step > worst_step) worst_step = step;
                const int32_t yj = turn - last_turn < 0 ? last_turn - turn : turn - last_turn;
                if (yj > worst_yaw_jerk) worst_yaw_jerk = yj;
            }
            last_dx = dx; last_dz = dz; last_turn = turn;
            px = r.x; pz = r.z; pyaw = r.yaw;
        }
        // A tenth of the step it takes each tick. A rival that snapped sideways
        // at a node boundary measured well over half of it.
        check(worst_jerk * 10 < worst_step,
              "a rival never jumps a large fraction of its own step in one tick");
        // And its heading turns rather than switching: a fiftieth of a turn in
        // one tick is five radians a second, which nothing on the track does.
        check(worst_yaw_jerk < k_turn / 50,
              "a rival's heading never snaps");
        std::printf("  %-10s rival step %.3f u/tick, worst jerk %.4f (%d%% of a "
                    "step), worst heading snap %.2f degrees\n",
                    t.name, worst_step / 65536.0, worst_jerk / 65536.0,
                    worst_step ? worst_jerk * 100 / worst_step : 0,
                    worst_yaw_jerk * 360.0 / 65536.0);
    }
}

void test_a_wall_stops_the_pod_rather_than_lifting_it() {
    // Reported from playing it: "when I crash into a wall I'm popping up above
    // the wall." Exactly what happened, and it followed from making a canyon
    // wall TERRAIN: past the road edge the ground was the top of the rock, so
    // the hover field found eleven units of it above the pod, the hard floor
    // placed the pod on top, and one tick after touching a canyon you were
    // standing on it.
    //
    // The profile is clamped to the blocking line now, so the field pushes off
    // the road at the foot of the wall and the lateral push does the rest.
    for (int ti = 0; ti < k_track_count; ++ti) {
        const Track& t = track(ti);
        int wall_node = -1;
        for (uint16_t i = 0; i < t.node_count; ++i)
            if (t.nodes[i].flags & kWall) { wall_node = i + 4; break; }
        if (wall_node < 0) continue;

        Race race;
        race_init(race, ti, 0);
        const TrackNode& n = t.nodes[wall_node % t.node_count];
        const TrackNode& b = t.nodes[(wall_node + 1) % t.node_count];
        const int32_t head = fatan2(node_x(b) - node_x(n), node_z(b) - node_z(n));
        race.pod.node = static_cast<uint16_t>(wall_node % t.node_count);
        race.pod.x = node_x(n);
        race.pod.z = node_z(n);
        race.pod.y = node_y(n) + k_hover_height;
        race.pod.yaw = head;
        // Straight at the wall at racing speed, sixty degrees off the road.
        const int32_t into = head + k_turn / 6;
        race.pod.vx = ftrig(per_s(fp(70)), fsin(into));
        race.pod.vz = ftrig(per_s(fp(70)), fcos(into));

        Input in{};
        in.throttle = true;
        in.right = true;
        int32_t highest = INT32_MIN;
        int32_t widest = 0;
        bool scraped = false;
        for (int i = 0; i < 200; ++i) {
            race_tick(race, in);
            if (race.pod.wreck_ticks) break;
            const Surface s = surface_at(t, race.pod.node, race.pod.x, race.pod.z);
            const int32_t above = race.pod.y - node_y(t.nodes[s.node]);
            if (above > highest) highest = above;
            const int32_t away = s.lateral < 0 ? -s.lateral : s.lateral;
            if (away > widest) widest = away;
            if (race.pod.scraping) scraped = true;
        }
        check(scraped, "the pod actually reached the wall");
        // The pod may hover and it may bounce, but it may not climb: anything
        // over the field's own reach is the pod standing on the wall.
        check(highest < k_hover_height + k_hover_reach,
              "hitting a wall does not lift the pod on top of it");
        std::printf("  %-10s into the wall at 70 u/s: highest %.1f over the road "
                    "(field reaches %.1f), widest %.1f from the centreline\n",
                    t.name, highest / 65536.0,
                    (k_hover_height + k_hover_reach) / 65536.0, widest / 65536.0);
    }
}

void test_the_railing_bounds_the_pod() {
    // Reported from playing it: off the main path, "massive geometry gaps", and
    // a request for something to keep the ship from drifting off. The drawn
    // plain is clamped short of the fold, so past it there was nothing to see
    // and nothing to aim at, and the sim happily let a pod fly out there
    // forever.
    //
    // Eighteen units past the road edge there is a railing. This drives every
    // track with a pilot trying to leave and measures how far it gets.
    for (int ti = 0; ti < k_track_count; ++ti) {
        const Track& t = track(ti);
        Race race;
        race_init(race, ti, 0);
        Input in{};
        int32_t widest = 0;
        for (int i = 0; i < 3000; ++i) {
            // Hold a turn away from the road, whichever way that is.
            const Surface s = surface_at(t, race.pod.node, race.pod.x, race.pod.z);
            in.throttle = true;
            in.right = s.lateral >= 0;
            in.left = s.lateral < 0;
            race_tick(race, in);
            if (race.pod.wreck_ticks) continue;
            const int32_t away = race.pod.lateral < 0 ? -race.pod.lateral
                                                      : race.pod.lateral;
            const int32_t half = node_half_width(t.nodes[race.pod.node]);
            if (away - half > widest) widest = away - half;
        }
        // The push happens after the pod has moved, so it can be one tick's
        // travel past the line before it is put back. A pod at top speed covers
        // about a unit a tick.
        check(widest < k_verge + fp(3),
              "a pod trying to leave the track is stopped by the railing");
        std::printf("  %-10s a pilot aiming off the road gets %.1f units past the "
                    "edge (railing at %.0f)\n",
                    t.name, widest / 65536.0, k_verge / 65536.0);
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
    test_running_wide_costs_time_and_not_the_run();
    test_only_a_gap_is_fatal();
    test_the_pod_never_gets_below_the_surface();
    test_a_hard_landing_costs_engines_and_not_the_run();
    test_the_sea_is_a_surface_and_not_a_hazard();
    test_the_sea_costs_time();
    test_rivals_move_smoothly();
    test_a_wall_stops_the_pod_rather_than_lifting_it();
    test_the_railing_bounds_the_pod();
    test_state_is_small();
    test_tracks_cost_what_they_claim();

    if (g_failures) {
        std::printf("%d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("twinflare sim tests passed\n");
    return 0;
}
