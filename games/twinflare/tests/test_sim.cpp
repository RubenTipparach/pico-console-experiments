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
    race_start(race, 0, 0);
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
            race_start(race, ti, ri);
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
        race_start(race, ti, 0);
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
    race_start(race, 0, 0);
    Input in{};
    in.throttle = true;
    for (int i = 0; i < 300; ++i) race_tick(race, in);
    const int32_t straight_yaw = race.pod.yaw;

    Race hurt_race;
    race_start(hurt_race, 0, 0);
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
    race_start(race, 0, 0);
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
    race_start(race, 0, 0);
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

    // WITH MARGIN, and that is not padding. The tick the gate first opens is by
    // construction the marginal one: measured, the pod arms at 0.7% over the
    // gate and is back under it on the very next tick, so a double tap issued
    // there fails on the second press for reasons that have nothing to do with
    // the double tap. Driven on until the pod is comfortably over the line.
    const int32_t clear = fscale(fscale(pod_top_speed(race.pod), k_boost_gate), 1080);
    for (int i = 0; i < 3000 && pod_speed(race.pod) < clear; ++i) {
        drive(race, t, in);
        race_tick(race, in);
    }
    check(pod_speed(race.pod) >= clear, "and gets clear of the gate");

    // And a double tap while it is armed lights it.
    in.boost_press = true;
    race_tick(race, in);
    race_tick(race, in);
    check(race.pod.boost_ticks > 0, "a double tap at speed lights the boost");

    // A single press, far apart, does not: that is what makes it a double tap
    // rather than a boost button with extra steps.
    Race single;
    race_start(single, 0, 0);
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
    race_start(race, 0, 0);
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
    race_start(race, 0, 0);
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
        race_start(race, ti, 1);   // WISP, the lowest top speed on the roster
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
    race_start(race, 0, 0);
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
    race_start(rough, 0, 0);
    race_start(smooth, 0, 0);
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
    race_start(race, 0, 0);
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
            race_start(race, ti, ri * 2);
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

// Drop the pod sixty units straight down onto the start straight at a chosen
// bank angle, and report what each engine lost, how fast it fell, and how close
// it came to the road.
//
// THE WHOLE ATTITUDE IS HELD, not only the roll, and that is not belt and
// braces. Roll is recomputed inside every tick from the yaw rate and the
// cockpit swing, and the swing is measured off the pod's offset from the
// centreline, so a roll assigned before the tick is gone by the time the
// landing is evaluated. Pinning roll alone measured the level case twice, once
// under each name, and reported the pair as a mismatch. The sideways velocity
// goes too: a pod that keeps flying while it falls lands somewhere else, and on
// a walled stretch that is a scrape mixed into the landing it is supposed to be
// measuring.
//
// What survives the tick is 48% of the roll set here, which is the one figure
// this borrows from the attitude model. It does not need to be exact: what the
// test asks is which side is low and whether the two sides mirror.
struct Drop {
    int32_t lost[2];
    int32_t fastest_fall;
    int32_t closest;
    bool wrecked;
};

Drop drop_with_roll(int32_t roll) {
    const Track& t = track(0);
    Race race;
    race_start(race, 0, 0);
    Input in{};
    for (int i = 0; i < 300; ++i) { drive(race, t, in); race_tick(race, in); }

    const Surface s = surface_at(t, race.pod.node, race.pod.x, race.pod.z);
    race.pod.y = s.y + fp(60);
    race.pod.vy = 0;
    const int16_t before[2] = {race.pod.engine[0], race.pod.engine[1]};

    Drop d{};
    d.closest = INT32_MAX;
    Input coast{};
    for (int i = 0; i < 240; ++i) {
        race.pod.roll = roll;
        race.pod.yaw_rate = 0;
        race.pod.swing = 0;
        race.pod.swing_rate = 0;
        race.pod.lateral = 0;
        race.pod.pitch = 0;
        race.pod.pitch_rate = 0;
        race.pod.vx = 0;
        race.pod.vz = 0;
        race_tick(race, coast);
        if (-race.pod.vy > d.fastest_fall) d.fastest_fall = -race.pod.vy;
        if (race.pod.grounded && race.pod.clearance < d.closest)
            d.closest = race.pod.clearance;
    }
    for (int i = 0; i < 2; ++i) d.lost[i] = before[i] - race.pod.engine[i];
    d.wrecked = race.pod.wreck_ticks != 0;
    return d;
}

void test_a_hard_landing_costs_engines_and_not_the_run() {
    // The other half of the same report: "pod should instead take damage if it
    // hits the ground too hard." Instead of what it used to do, which was sink
    // through the road and wreck.
    const Drop level = drop_with_roll(0);
    check(!level.wrecked, "a hard landing does not wreck the pod");
    check(level.closest >= k_hover_floor, "and it does not go through the surface");
    check(level.lost[0] > 0, "it costs the port engine health");
    check(level.lost[1] > 0, "and the starboard engine too");
    check(level.lost[0] == level.lost[1],
          "a LEVEL slam is symmetric: both engines hit the ground together");
    // Enough to be worth avoiding and not enough to end a race outright, which
    // is the whole point of moving the penalty from the run to the engines.
    const int32_t total = level.lost[0] + level.lost[1];
    check(level.lost[0] > 40, "the damage is felt");
    check(level.lost[0] < k_engine_max / 2, "and one bad landing is survivable");

    // AND A TILTED ONE IS NOT. The pod's local frame puts engine 0 to port and
    // a positive roll drops it, so a pod landing mid corner puts the blow on
    // the inside engine. This is the asked for behaviour ("one or both of the
    // engines take damage") and the reason a landing is now worth steering out
    // of rather than a tax on the whole machine.
    const Drop port_low = drop_with_roll(k_swing_max);
    const Drop starboard_low = drop_with_roll(-k_swing_max);
    check(port_low.lost[0] > port_low.lost[1],
          "rolled to port, the port engine takes the landing");
    check(starboard_low.lost[1] > starboard_low.lost[0],
          "rolled to starboard, the starboard engine does");
    check(port_low.lost[0] == starboard_low.lost[1]
          && port_low.lost[1] == starboard_low.lost[0],
          "and the two are mirror images of each other");

    // THE TOTAL IS THE SAME. Leaning redistributes a landing, it does not add
    // one: without this the check above passes just as well for a change that
    // left both engines on the old even split and piled extra damage on the low
    // one, which is a different game and a much harsher one.
    const int32_t tilted_total = port_low.lost[0] + port_low.lost[1];
    check(tilted_total == total,
          "a tilted landing costs the machine what a level one costs it");

    std::printf("  sixty unit drop: fell at %d u/s, cost %d + %d level and "
                "%d + %d rolled hard over, closest to the road %.2f units\n",
                level.fastest_fall * k_tick_hz / k_one,
                level.lost[0], level.lost[1], port_low.lost[0], port_low.lost[1],
                level.closest / 65536.0);
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
    race_start(race, tide, 0);
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
        race_start(fall, tide, 0);
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
        race_start(race, tide, 0);
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
        race_start(race, ti, 0);
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
        race_start(race, ti, 0);
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
        race_start(race, ti, 0);
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

void test_the_race_is_held_on_the_line() {
    // A race used to begin the instant the screen appeared. Nothing may move
    // until the light goes green: not the pod, not the pack, and not the clock,
    // because a countdown the lap timer runs through is a countdown that costs
    // the player three seconds of their own lap.
    Race race;
    race_init(race, 0, 0);
    check(race.phase == Phase::Countdown, "a race starts on the grid");

    const Pod start = race.pod;
    const int32_t rival_start = race.rivals[0].distance;
    Input flat{};
    flat.throttle = true;
    for (int i = 0; i < k_count_ticks - 1; ++i) race_tick(race, flat);

    check(race.phase == Phase::Countdown, "and it is still on the grid at 0.01s");
    check(race.pod.x == start.x && race.pod.z == start.z,
          "the pod has not moved off the line");
    check(race.pod.vx == 0 && race.pod.vz == 0, "and it is not even rolling");
    check(race.rivals[0].distance == rival_start, "nor has the field");
    check(race.ticks == 0, "and the clock has not started");

    race_tick(race, flat);
    check(race.phase == Phase::Racing, "then the light goes green");
    for (int i = 0; i < 100; ++i) race_tick(race, flat);
    check(race.ticks == 100, "the clock runs from green, not from the grid");
    check(race.pod.x != start.x || race.pod.z != start.z, "the pod is away");
    check(race.rivals[0].distance > rival_start, "and so is the field");
}

void test_the_countdown_counts_three_two_one() {
    // The number on screen comes from the sim rather than from a frame counter
    // in the renderer, so what the player is looking at and what the charge is
    // racing against cannot drift apart.
    Race race;
    race_init(race, 0, 0);
    Input idle{};
    int counts = 0, gos = 0;
    int seen[5] = {0, 0, 0, 0, 0};
    int last = countdown_number(race);
    check(last == 3, "it opens on three");
    while (race.phase == Phase::Countdown) {
        race_tick(race, idle);
        if (race.ev.count) ++counts;
        if (race.ev.go) ++gos;
        const int n = countdown_number(race);
        if (n != last) { seen[n]++; last = n; }
    }
    // Three changes of number: 3 to 2, 2 to 1, 1 to GO. A fourth would mean the
    // number flickered, and two would mean one of them never showed.
    check(counts == 3, "the countdown announces exactly three numbers");
    check(seen[2] == 1 && seen[1] == 1 && seen[0] == 1,
          "and it shows 2, then 1, then GO, once each");
    check(gos == 1, "the green light fires once");
    check(countdown_number(race) == 0, "and the number is gone once racing");
}

void test_winding_up_on_the_line_pays_and_can_blow() {
    // Three seconds of nothing is three seconds of nothing unless there is
    // something to do with them. Holding the throttle against the brakes fills
    // a charge that cashes in as boost, and past the flood line it holes both
    // engines instead, which is the whole tension of the grid.
    Input flat{};
    flat.throttle = true;
    const Input idle{};

    // Held all the way: it floods, and floods only once, because the charge is
    // spent when it goes.
    Race blown;
    race_init(blown, 0, 0);
    const int16_t whole = blown.pod.engine[0];
    int floods = 0;
    while (blown.phase == Phase::Countdown) {
        race_tick(blown, flat);
        if (blown.ev.flood) ++floods;
    }
    check(blown.pod.engine[0] < whole && blown.pod.engine[1] < whole,
          "holding it down all three seconds costs BOTH engines, not one");
    check(blown.pod.engine[0] == blown.pod.engine[1],
          "evenly, because nothing struck either of them");
    // ONCE, and this is the check that matters. Reset to zero without latching,
    // a finger still on the throttle simply refills the charge and blows it
    // again: measured, that was three floods in one countdown and 720 of 1000
    // health off both engines before the race had started. One mistake is one
    // mistake, and every check above passes just as well for three of them.
    check(floods == 1, "and it blows once, however long the throttle is held");
    check(whole - blown.pod.engine[0] == k_charge_burn,
          "for exactly one engine's worth of damage");
    check(blown.charge == 0, "with nothing left to cash in");

    // Start winding up late enough to arrive at green with a big charge and no
    // flood, which is the shot the whole mechanic is asking for. Held right up
    // to the light rather than released early: the charge BLEEDS off the
    // throttle, so a player who lets go at the top of it and waits arrives on
    // green with nothing, and this test measured that instead the first time.
    Race clean;
    race_init(clean, 0, 0);
    while (clean.countdown > 150) race_tick(clean, idle);
    while (clean.phase == Phase::Countdown) race_tick(clean, flat);
    const int16_t held = static_cast<int16_t>(150 * k_charge_rise);
    check(clean.pod.engine[0] == whole && clean.pod.engine[1] == whole,
          "a charge released in time costs nothing");
    check(clean.pod.boost_ticks > 0, "and it is worth a boost off the line");

    // Which is worth MORE than doing nothing at all, and more than blowing it.
    Race lazy;
    race_init(lazy, 0, 0);
    while (lazy.phase == Phase::Countdown) race_tick(lazy, idle);
    check(lazy.pod.boost_ticks == 0, "a pod that sat there gets nothing");
    check(blown.pod.boost_ticks == 0, "and neither does one that flooded");

    // The charge BLEEDS when the throttle comes off, so a launch is a release
    // at the right moment rather than a button held from the first tick.
    Race bled;
    race_init(bled, 0, 0);
    while (bled.charge < k_charge_flood / 2) race_tick(bled, flat);
    const int16_t peak = bled.charge;
    for (int i = 0; i < 20; ++i) race_tick(bled, idle);
    check(bled.charge < peak, "letting go bleeds the charge");

    std::printf("  launch: %d charge released clean is %d boost ticks, "
                "flooding costs %d health an engine\n",
                held, clean.pod.boost_ticks, whole - blown.pod.engine[0]);
}

void test_a_wall_grinds_the_engine_that_is_against_it() {
    // "when engine collides with wall ... one or both of the engines take
    // damage". It used to take the same off both, so the bar the player was
    // watching went down and nothing said which side was on the rock.
    //
    // Driven off both edges of the same walled stretch in turn, so the two runs
    // differ in nothing but which way the pod was steered.
    //
    // On HOARFROST, which is three quarters wall: on the open desert a lock to
    // one side finds a railing and a lock to the other finds nothing but
    // shoulder, and the run that never touched rock reported no damage on
    // either engine, which reads exactly like the split not working.
    const int frost = 3;
    const Track& t = track(frost);
    int32_t lost[2][2] = {{0, 0}, {0, 0}};
    int scraped[2] = {0, 0};
    for (int side = 0; side < 2; ++side) {
        Race race;
        race_start(race, frost, 0);
        Input in{};
        // Up to speed on the racing line first, then hold a lock against one
        // edge. The lock is re-applied every tick and the run is long enough to
        // find rock wherever on the lap the pod happens to be.
        for (int i = 0; i < 400; ++i) { drive(race, t, in); race_tick(race, in); }
        // ACCOUNTED PER TICK, AND ONLY THE SCRAPE. A pod held against a wall
        // also bounces off it, and a landing damages both engines: summing the
        // whole run credits the far engine with damage the wall never did, and
        // a version that put the ENTIRE scrape on the near engine passed a
        // check that the far engine had lost something. It had, to the floor.
        for (int i = 0; i < 3000; ++i) {
            in = Input{};
            in.throttle = true;
            in.right = side == 1;
            in.left = side == 0;
            const int16_t was[2] = {race.pod.engine[0], race.pod.engine[1]};
            const bool grinding = race.pod.scraping;
            race_tick(race, in);
            if (race.pod.scraping) ++scraped[side];
            if (!grinding || race.ev.slam) continue;
            for (int e = 0; e < 2; ++e) lost[side][e] += was[e] - race.pod.engine[e];
        }
    }
    check(scraped[0] > 50 && scraped[1] > 50, "both runs actually found a wall");
    check(lost[0][0] > 0 && lost[1][1] > 0, "and it cost them engine health");
    // Steered left, the pod runs out on the port side and the port engine is
    // the one on the rock. Steered right, the mirror.
    check(lost[0][0] > lost[0][1], "grinding the port side costs the port engine most");
    check(lost[1][1] > lost[1][0], "and the starboard side costs the starboard engine");
    // AND THE FAR ENGINE STILL PAYS. Without this the check above is satisfied
    // by moving the whole scrape onto one engine, which is a different and much
    // more punishing game: a pod could lean on a wall all lap with one engine
    // untouched.
    check(lost[0][1] > 0 && lost[1][0] > 0,
          "the frame carries some of a scrape across to the other engine");
    std::printf("  scrape: port lock cost %d + %d over %d ticks, starboard %d + %d "
                "over %d\n", lost[0][0], lost[0][1], scraped[0],
                lost[1][0], lost[1][1], scraped[1]);
}

// Park the pod alongside rival zero, `off` units to the rival's side of it, and
// facing the way the rival is going.
//
// THE POD IS MOVED, NOT THE RIVAL, and that is not a matter of taste. A rival
// has no position of its own: rival_place rebuilds x, y and z from its distance
// along the centreline at the top of every tick, so a rival dropped next to the
// pod is back on the racing line before the contact test ever sees it. Placing
// the rival was the first thing tried and it produced a collision test that
// could not make a collision happen.
//
// The pod is held still as well, so what it meets on the next tick is the
// rival's own three quarters of a unit of travel and nothing else.
void park_beside_rival(Race& race, int32_t off) {
    const Rival& r = race.rivals[0];
    Pod& p = race.pod;
    p.yaw = r.yaw;
    p.x = r.x - ftrig(off, fcos(r.yaw));
    p.z = r.z + ftrig(off, fsin(r.yaw));
    p.y = r.y;
    p.vx = p.vz = p.vy = 0;
}

void test_touching_a_rival_costs_the_engine_that_touched() {
    // "when engine collides with enemy that engine takes damage". Rivals were
    // scenery: five shapes going past at a plausible speed that the pod could
    // fly straight through, so the pack was something to look at rather than
    // something to get past.
    const Track& t = track(0);
    for (int side = 0; side < 2; ++side) {
        Race race;
        race_start(race, 0, 0);
        Input in{};
        for (int i = 0; i < 300; ++i) { drive(race, t, in); race_tick(race, in); }

        // Positive across is to starboard, so a rival on the starboard flank
        // sits at a positive offset from the pod.
        const int32_t off = side == 1 ? fp(1, 500) : -fp(1, 500);
        park_beside_rival(race, off);

        const int16_t before[2] = {race.pod.engine[0], race.pod.engine[1]};
        const int32_t yaw = race.pod.yaw;
        Input coast{};
        race_tick(race, coast);

        check(race.ev.bump, "a rival alongside is a collision");
        check(race.pod.engine[side] < before[side],
              "and it costs the engine on the side it happened");
        check(race.pod.engine[side ^ 1] == before[side ^ 1],
              "and only that one: a touch is not a landing");
        // Shoved AWAY, which is what stops the two riding along inside each
        // other for as long as the rival keeps pace. Measured in the pod's own
        // frame at the heading it had when it was hit.
        const int32_t push = ftrig(race.pod.vx, fcos(yaw)) - ftrig(race.pod.vz, fsin(yaw));
        check(side == 1 ? push < 0 : push > 0,
              "the pod is pushed away from what it hit, not into it");

        // ONE touch is ONE hit. Without the cooldown a rival that keeps station
        // takes an engine out in under a second at a hundred ticks a second,
        // which is not a collision, it is a blender.
        const int16_t after = race.pod.engine[side];
        for (int i = 0; i < k_bump_ticks - 2; ++i) {
            park_beside_rival(race, off);
            race_tick(race, coast);
        }
        check(race.pod.engine[side] == after,
              "and staying alongside does not grind the engine away");
        // The cooldown EXPIRES, though, so a second pass costs a second hit and
        // a pod cannot be made invulnerable by never leaving the pack.
        for (int i = 0; i < 6; ++i) {
            park_beside_rival(race, off);
            race_tick(race, coast);
        }
        check(race.pod.engine[side] < after, "a second touch costs a second time");
    }

    // A rival passing directly UNDERNEATH is not a collision: a pod off a crest
    // flies over the pack rather than through it. Without the height band, two
    // racers on the same piece of road at different heights collide, and on a
    // track with a tunnel that is a hit from something the player cannot see.
    //
    // The pod goes above rather than the rival below, and it has to: a rival
    // has no height of its own either, rival_place puts it a hover height over
    // the road every tick. Sinking the pod instead puts it under the road,
    // where the hover floor lifts it straight back into the band inside one
    // tick, and the test then measures the field rather than the height band.
    Race high;
    race_start(high, 0, 0);
    Input in{};
    for (int i = 0; i < 300; ++i) { drive(high, t, in); race_tick(high, in); }
    park_beside_rival(high, 0);
    high.pod.y = high.rivals[0].y + k_bump_high + fp(1);
    const int16_t before[2] = {high.pod.engine[0], high.pod.engine[1]};
    Input coast{};
    race_tick(high, coast);
    check(!high.ev.bump, "a rival passing overhead is not a collision");
    check(high.pod.engine[0] == before[0] && high.pod.engine[1] == before[1],
          "and it costs nothing");
}

void test_the_position_counts_from_the_line_the_field_does() {
    // pod.lap counts from one and a rival's distance counts from zero on the
    // line, so the player used to be credited a whole lap they had not driven.
    // Measured, that was position 1 for all 9,412 ticks of a three lap race
    // whatever the rest of the field did.
    Race race;
    race_start(race, 0, 0);
    check(race.place == 1, "on the line, ahead of a field that is behind it");

    // Park the pod on the start line and let the field run. The whole field
    // starts a few units BEHIND the line and the slowest pod covers that inside
    // a second, so a pod that has not moved has to be last of six within a
    // couple of seconds.
    //
    // WITHIN A COUPLE OF SECONDS, and the deadline is the test. Given a minute
    // the field laps a parked pod anyway, so "it ends up last" was true with
    // the free lap credit still in and the bug survived the check that was
    // written for it. Three hundred ticks is long enough for every rival to
    // reach the line and far too short for any of them to make up a whole lap
    // of imaginary progress.
    Input idle{};
    int at_300 = 0;
    for (int i = 0; i < 300; ++i) {
        race.pod.vx = race.pod.vz = 0;   // held, so this is about the field
        race_tick(race, idle);
    }
    at_300 = race.place;
    check(at_300 == k_racer_count,
          "three seconds later the whole field is past it, and the HUD says so");
    for (int i = 0; i < 5700; ++i) {
        race.pod.vx = race.pod.vz = 0;
        race_tick(race, idle);
    }
    check(race.place == k_racer_count, "and it stays last");
    std::printf("  a parked pod is %d of %d after three seconds and %d after a "
                "minute\n", at_300, k_racer_count, race.place);
}

void test_every_racer_gets_a_time_and_the_table_orders_them() {
    // "show the current placing, and total race time that player did, as well
    // for each other racer as they cross the finishing line."
    const Track& t = track(0);
    Race race;
    race_start(race, 0, 0);
    Input in{};
    int guard = 0;
    while (!race.done && guard++ < 40000) {
        in = Input{};
        const Pod& p = race.pod;
        const TrackNode& ahead = t.nodes[(p.node + 4) % t.node_count];
        const int32_t want = fatan2(node_x(ahead) - p.x, node_z(ahead) - p.z);
        const int32_t err = angle_diff(want, p.yaw);
        in.throttle = true;
        in.left = err < -300;
        in.right = err > 300;
        race_tick(race, in);
    }
    check(race.done, "the finish sequence ends");
    check(race.finished, "and the player got round");
    check(race.finish_tick > 0, "with a total time");

    Standing st[k_racer_count];
    standings(race, st);
    int players = 0, finishers = 0;
    for (int i = 0; i < k_racer_count; ++i) {
        if (st[i].player) ++players;
        if (st[i].finished) ++finishers;
    }
    check(players == 1, "the player appears exactly once in the table");
    check(finishers >= 3, "and most of the field is in by the time it ends");

    // Ordered: everyone who finished, by time, above everyone who did not.
    bool ordered = true, seen_unfinished = false;
    uint32_t last_time = 0;
    for (int i = 0; i < k_racer_count; ++i) {
        if (!st[i].finished) { seen_unfinished = true; continue; }
        if (seen_unfinished) ordered = false;   // a finisher below a non finisher
        if (st[i].ticks < last_time) ordered = false;
        last_time = st[i].ticks;
    }
    check(ordered, "the table runs fastest first, with the unfinished at the bottom");

    // Every racer, once: a table that lost or duplicated a row would still be
    // ordered and would still have one player in it.
    int seen[k_racer_count] = {0};
    for (int i = 0; i < k_racer_count; ++i) seen[st[i].racer_index % k_racer_count]++;
    bool all_once = true;
    for (int i = 0; i < k_racer_count; ++i) if (seen[i] != 1) all_once = false;
    check(all_once, "and every racer on the roster is on it exactly once");

    std::printf("  results after %.1fs: ", race.finish_tick / 100.0);
    for (int i = 0; i < k_racer_count; ++i)
        std::printf("%s%s ", racer(st[i].racer_index).name, st[i].player ? "*" : "");
    std::printf("\n");
}

void test_the_pod_keeps_flying_after_the_flag() {
    // "give control to the AI after player crosses the race". A pod abandoned
    // in mid air while the rest of the field comes in is a worse thing to look
    // at than a chase camera on a cockpit nobody is steering.
    const Track& t = track(0);
    Race race;
    race_start(race, 0, 0);
    Input in{};
    int guard = 0;
    while (!race.finished && guard++ < 40000) { drive(race, t, in); race_tick(race, in); }
    check(race.finished, "the player finished");
    check(race.phase == Phase::Finished, "and the race knows it");

    // Hands off from here: the input passed in is empty, so anything that
    // happens is the sim's own driver.
    const int32_t x = race.pod.x, z = race.pod.z;
    const uint8_t node = static_cast<uint8_t>(race.pod.node);
    const Input hands_off{};
    int on_road = 0;
    uint8_t modes = 0;
    for (int i = 0; i < 1500; ++i) {
        race_tick(race, hands_off);
        if (race.pod.on_road) ++on_road;
        modes = static_cast<uint8_t>(modes | (1 << race.cam_mode));
    }
    check(race.pod.x != x || race.pod.z != z, "the pod keeps flying with no input");
    check(race.pod.node != node, "and it makes progress along the track");
    check(on_road > 1000, "the autopilot keeps it on the road");
    // The camera cuts, which is the other half of the ask. Fifteen seconds is
    // seven cuts at two seconds each, so every angle has to have come up.
    check(modes == (1 << k_cam_modes) - 1,
          "and the camera works through every angle it has");
    std::printf("  after the flag: %d of 1500 ticks on the road, %d camera angles\n",
                on_road, k_cam_modes);
}

void test_a_damaged_engine_smokes_before_it_dies() {
    // The renderer asks the sim whether an engine is critical rather than
    // keeping its own threshold, so what smokes and what the bar shows cannot
    // disagree.
    Race race;
    race_start(race, 0, 0);
    Pod& p = race.pod;
    check(!engine_critical(p, 0) && !engine_critical(p, 1), "a fresh pod is fine");
    p.engine[0] = static_cast<int16_t>(p.engine_max * k_engine_critical / 1000 + 1);
    check(!engine_critical(p, 0), "just above the line it is not smoking yet");
    p.engine[0] = static_cast<int16_t>(p.engine_max * k_engine_critical / 1000 - 1);
    check(engine_critical(p, 0), "just below it, it is");
    check(!engine_critical(p, 1), "and the other engine is its own question");
    p.engine[0] = 0;
    p.dead |= 1;
    check(engine_critical(p, 0), "a dead engine counts as critical");
}

void test_a_hit_says_which_engine_took_it() {
    // The renderer used to strike sparks off a wall alone, because a wall was
    // the only damage source that said WHERE it landed. This is the seam that
    // lets any hit be drawn on the side it happened.
    const Track& t = track(0);
    Race race;
    race_start(race, 0, 0);
    Input in{};
    for (int i = 0; i < 300; ++i) { drive(race, t, in); race_tick(race, in); }
    check(race.pod.hit[0] == 0 && race.pod.hit[1] == 0,
          "a clean lap strikes no sparks");

    // A rival on the starboard flank.
    park_beside_rival(race, fp(1, 500));
    Input coast{};
    race_tick(race, coast);
    check(race.pod.hit[1] > 0, "a touch on the starboard side sparks to starboard");
    check(race.pod.hit[0] == 0, "and not to port");

    // And it burns out rather than staying lit, so a spark shower does not
    // outlive the blow that caused it.
    for (int i = 0; i < k_hit_ticks + 2; ++i) race_tick(race, coast);
    check(race.pod.hit[1] == 0, "and it stops");
}

void test_heat_does_not_throw_sparks() {
    // Heat is the one damage source that is not an impact: both engines are
    // cooking, and a shower of sparks off a pod nothing has touched reads as a
    // collision the player did not have.
    Race race;
    race_start(race, 0, 0);
    Pod& p = race.pod;
    p.heat = k_heat_one - 1;
    const int16_t before = p.engine[0];
    Input in{};
    in.throttle = true;
    for (int i = 0; i < 5; ++i) {
        p.heat = k_heat_one - 1;   // held over the line
        race_tick(race, in);
    }
    check(race.pod.engine[0] < before, "cooking costs engine health");
    check(race.pod.hit[0] == 0 && race.pod.hit[1] == 0,
          "and it does it without striking sparks");
}

void test_a_frame_hears_every_tick_in_it() {
    // A frame steps the sim up to eight times and calls the sound layer once,
    // so something has to carry the seven ticks that would otherwise be thrown
    // away. Edges accumulate; levels take the latest value.
    Events frame{};
    Events a{};
    a.lap = true;
    a.rev = 200;
    a.grinding = true;
    Events b{};
    b.bump = true;
    b.rev = 40;
    b.grinding = false;

    merge_events(frame, a);
    merge_events(frame, b);
    check(frame.lap, "a lap on the first tick of a frame survives to the end of it");
    check(frame.bump, "and so does a bump on the last");
    // THE LEVELS DO NOT ACCUMULATE, which is the half that is easy to get
    // wrong: OR'd like the rest, the rev would climb to the loudest tick of the
    // frame and stay there, and one tick against a wall would latch the grind
    // on for the whole frame.
    check(frame.rev == 40, "the engine note is the latest tick's, not the loudest");
    check(!frame.grinding, "and a scrape that ended during the frame has ended");
}

void test_the_race_says_what_happened() {
    // The whole point of Events: the sound layer holds no state about the race
    // and the race holds no opinion about the sound. Every cue below is raised
    // by driving the sim into the situation rather than by setting the flag.
    const Track& t = track(0);

    // The countdown announces its numbers and the green light.
    {
        Race race;
        race_init(race, 0, 0);
        int counts = 0, gos = 0;
        Input idle{};
        while (race.phase == Phase::Countdown) {
            race_tick(race, idle);
            counts += race.ev.count ? 1 : 0;
            gos += race.ev.go ? 1 : 0;
        }
        check(counts == 3 && gos == 1, "the grid says three, two, one, go");
    }

    // A lap, the flag, and a rev that rises off the throttle.
    {
        Race race;
        race_start(race, 0, 0);
        Input in{};
        int laps = 0, finishes = 0, boosts = 0;
        uint8_t quiet = 255, loud = 0;
        int guard = 0;
        while (!race.finished && guard++ < 40000) {
            drive(race, t, in);
            race_tick(race, in);
            laps += race.ev.lap ? 1 : 0;
            finishes += race.ev.finish ? 1 : 0;
            boosts += race.ev.boost ? 1 : 0;
            if (race.ev.rev < quiet) quiet = race.ev.rev;
            if (race.ev.rev > loud) loud = race.ev.rev;
        }
        check(laps == t.laps, "one lap event per lap of the track");
        check(finishes == 1, "and one flag, on the last of them");
        check(boosts > 0, "the boost pads announce themselves");
        // The engine is a LEVEL and has to actually move, or the drone it
        // drives is a constant tone with a volume knob.
        check(loud > 150, "the engine note reaches the top of its range");
        check(loud - quiet > 100, "and it has real range, not a fixed pitch");
        std::printf("  events over a race: %d laps, %d boosts, rev %d..%d\n",
                    laps, boosts, quiet, loud);
    }

    // A wall says so on the tick it starts, once, not on every tick it lasts.
    // On HOARFROST, which is three quarters wall, and driven against HOARFROST:
    // the driver takes the track it is steering round, and handing it the
    // desert while the race ran on the ice put the pod nowhere near a wall.
    {
        const Track& frost = track(3);
        Race race;
        race_start(race, 3, 0);
        Input in{};
        for (int i = 0; i < 400; ++i) { drive(race, frost, in); race_tick(race, in); }
        int starts = 0, grinding = 0;
        for (int i = 0; i < 1500; ++i) {
            in = Input{};
            in.throttle = true;
            in.left = true;
            race_tick(race, in);
            starts += race.ev.scrape ? 1 : 0;
            grinding += race.ev.grinding ? 1 : 0;
        }
        check(grinding > 50, "the pod really did grind along a wall");
        check(starts > 0, "and the scrape announced itself");
        check(starts < grinding / 4,
              "once per contact, not once per tick of it: a scrape is a held "
              "sound with an attack, not a machine gun");
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
    test_the_race_is_held_on_the_line();
    test_the_countdown_counts_three_two_one();
    test_winding_up_on_the_line_pays_and_can_blow();
    test_a_wall_grinds_the_engine_that_is_against_it();
    test_touching_a_rival_costs_the_engine_that_touched();
    test_the_position_counts_from_the_line_the_field_does();
    test_every_racer_gets_a_time_and_the_table_orders_them();
    test_the_pod_keeps_flying_after_the_flag();
    test_a_damaged_engine_smokes_before_it_dies();
    test_a_hit_says_which_engine_took_it();
    test_heat_does_not_throw_sparks();
    test_a_frame_hears_every_tick_in_it();
    test_the_race_says_what_happened();
    test_state_is_small();
    test_tracks_cost_what_they_claim();

    if (g_failures) {
        std::printf("%d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("twinflare sim tests passed\n");
    return 0;
}
