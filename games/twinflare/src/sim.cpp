#include "sim.hpp"

#include "fixed.hpp"

namespace twinflare {
namespace {

// The roster. Every pod totals 20 points, so the choice is a shape rather than
// a ladder, and only two engine meshes exist between the six.
constexpr Racer k_racers[k_racer_count] = {
    {"SCARAB", "SEBO VANE",  3, 3, 4, 4, 3, 3, 0,
     {{214, 124, 40}, {150, 78, 26}, {74, 52, 40}}},
    {"WISP", "TIK MA'ALA",   2, 5, 5, 3, 3, 2, 0,
     {{96, 214, 206}, {44, 132, 140}, {226, 232, 238}}},
    {"ANVIL", "GROLL HEXX",  3, 2, 2, 5, 3, 5, 1,
     {{186, 190, 198}, {96, 102, 116}, {188, 52, 52}}},
    {"NEEDLE", "CYRN VAST",  5, 4, 3, 1, 2, 1, 0,
     {{232, 206, 64}, {150, 126, 20}, {36, 34, 32}}},
    {"NIGHTJAR", "OMBRA TAL", 2, 3, 3, 3, 5, 4, 1,
     {{142, 106, 196}, {70, 50, 104}, {40, 38, 52}}},
    {"FANG", "KEST RHO",     4, 4, 5, 2, 3, 2, 0,
     {{122, 196, 86}, {58, 110, 48}, {184, 150, 72}}},
};

// Damage, which is the one thing two engines do not share.
void hurt(Pod& pod, int which, int32_t amount) {
    if (amount <= 0 || engine_dead(pod, which)) return;
    pod.engine[which] = static_cast<int16_t>(pod.engine[which] - amount);
    if (pod.engine[which] <= 0) {
        pod.engine[which] = 0;
        pod.dead |= static_cast<uint8_t>(1 << which);
        pod.blast[which] = 50;
    }
}

void wreck(Pod& pod) {
    if (pod.wreck_ticks > 0) return;
    pod.wreck_ticks = k_respawn_ticks;
    if (pod.vy < per_s(fp(6))) pod.vy = per_s(fp(6));
}

// Back on the road with both engines at half, which is the only forgiving
// thing in the whole model. A three lap race that ends on lap one is a race
// nobody runs twice.
void respawn(Pod& pod, const Track& t) {
    const TrackNode& n = t.nodes[pod.node];
    const TrackNode& ahead = t.nodes[(pod.node + 1) % t.node_count];
    const int32_t heading = fatan2(node_x(ahead) - node_x(n), node_z(ahead) - node_z(n));
    pod.x = node_x(n);
    pod.z = node_z(n);
    pod.y = node_y(n) + k_hover_height;
    pod.vx = ftrig(k_respawn_speed, fsin(heading));
    pod.vz = ftrig(k_respawn_speed, fcos(heading));
    pod.vy = 0;
    pod.yaw = heading;
    pod.yaw_rate = 0;
    pod.pitch = 0;
    pod.pitch_rate = 0;
    pod.roll = 0;
    pod.swing = 0;
    pod.swing_rate = 0;
    pod.dead = 0;
    pod.engine[0] = pod.engine[1] = static_cast<int16_t>(pod.engine_max / 2);
    pod.heat = 0;
    pod.locked = false;
    pod.boost_ticks = 0;
    pod.wreck_ticks = 0;
}

void rival_place(Rival& r, const Track& t) {
    const int32_t count = t.node_count;
    const int32_t lap = count * k_node_spacing;
    int32_t d = r.distance % lap;
    if (d < 0) d += lap;
    const int32_t index = d / k_node_spacing;
    const int32_t frac = d - index * k_node_spacing;
    const TrackNode& a = t.nodes[index % count];
    const TrackNode& b = t.nodes[(index + 1) % count];
    const int32_t u = (frac << k_fp) / k_node_spacing;

    const int32_t ax = node_x(a), az = node_z(a), ay = node_y(a);
    const int32_t bx = node_x(b), bz = node_z(b), by = node_z(b) * 0 + node_y(b);
    const int32_t heading = fatan2(bx - ax, bz - az);

    // A weave, so the pack is not a line of pods nose to tail. Deterministic
    // from the tick and the rival's own phase, which is what makes a replay of
    // the same race the same race.
    const int32_t wob = ftrig(fp(2, 400), fsin((r.phase << 8) + (r.distance >> 7)));
    const int32_t off = ftrig(fp(3), fsin(r.phase << 9)) + wob;

    r.x = ax + fmul(bx - ax, u) + ftrig(off, fcos(heading));
    r.z = az + fmul(bz - az, u) - ftrig(off, fsin(heading));
    r.y = ay + fmul(by - ay, u) + k_hover_height;
    const int32_t turn = angle_diff(fatan2(node_x(t.nodes[(index + 4) % count]) - bx,
                                           node_z(t.nodes[(index + 4) % count]) - bz),
                                    heading);
    r.yaw = heading;
    r.roll = clamp32(-turn * 3, -k_swing_max, k_swing_max);
}

}  // namespace

const Racer& racer(int index) {
    return k_racers[static_cast<unsigned>(index) % k_racer_count];
}

int32_t pod_speed(const Pod& pod) { return flength(pod.vx, pod.vz); }

int32_t pod_top_speed(const Pod& pod) {
    return fscale(k_top_speed_ref, stat_scale(racer(pod.racer_index).top, k_spread_top));
}

bool boost_armed(const Pod& pod) {
    if (pod.locked || pod.wreck_ticks > 0) return false;
    return pod_speed(pod) >= fscale(pod_top_speed(pod), k_boost_gate);
}

uint16_t nearest_node(const Track& t, uint16_t hint, int32_t x, int32_t z) {
    const int32_t count = t.node_count;
    int32_t best = hint;
    int64_t best_d = INT64_MAX;
    // Outward from the hint, not over the whole ring. A pod moves at most a
    // fifth of a node per tick, so a window this wide is many seconds of
    // slack, and it is also what keeps a track that runs back alongside itself
    // from snapping the player onto the other carriageway.
    for (int32_t k = -8; k <= 14; ++k) {
        const int32_t i = ((hint + k) % count + count) % count;
        const int64_t dx = node_x(t.nodes[i]) - x;
        const int64_t dz = node_z(t.nodes[i]) - z;
        const int64_t d = dx * dx + dz * dz;
        if (d < best_d) { best_d = d; best = i; }
    }
    return static_cast<uint16_t>(best);
}

Surface surface_at(const Track& t, uint16_t near_node, int32_t x, int32_t z) {
    const uint16_t i = nearest_node(t, near_node, x, z);
    const TrackNode& a = t.nodes[i];
    const TrackNode& b = t.nodes[(i + 1) % t.node_count];
    const int32_t heading = fatan2(node_x(b) - node_x(a), node_z(b) - node_z(a));

    Surface s{};
    s.node = i;
    // Signed lateral offset, in the node's own frame.
    s.lateral = ftrig(x - node_x(a), fcos(heading)) - ftrig(z - node_z(a), fsin(heading));

    if (a.flags & kGap) {
        // A gap carries no surface at all. The hover field has nothing to push
        // against, so you fall, and that is the entire implementation of
        // "jump this or fall in": no trigger volume and no jump code.
        s.y = fp(-10000);
        s.road = false;
        return s;
    }

    const int32_t half = node_half_width(a);
    const int32_t over = (s.lateral < 0 ? -s.lateral : s.lateral) - half;
    if (over > 0) {
        if (a.flags & kWall) {
            s.y = node_y(a);
            s.wall = true;
            return s;
        }
        // Off the road is a SHOULDER, not a cliff.
        //
        // It used to fall away by up to thirty units, and the crash floor is
        // twenty six, so drifting wide did not cost you time, it killed you:
        // the ground vanished downward, the pod followed it, and the run
        // ended. That is not what going off line should mean in a racing game,
        // and it is not what the hover field is for either, since a field that
        // holds you over the road but drops you beside it is a trapdoor.
        //
        // Three units down and no further. The hover field still has something
        // to push against out here, so the pod stays flyable and can be driven
        // back on; what it costs is grip and speed, applied in race_tick.
        // Falling is reserved for a GAP, where there is genuinely no road.
        const int32_t drop = over > fp(12) ? fp(3) : fscale(over, 250);
        s.y = node_y(a) - drop;
        return s;
    }
    // Banking: the road tilts into its own turn, which is free grip on a
    // corner and the reason a circuit can be fast and tight at once.
    const TrackNode& c = t.nodes[(i + 3) % t.node_count];
    const int32_t next = fatan2(node_x(c) - node_x(b), node_z(c) - node_z(b));
    const int32_t bank = -fmul(ftrig(s.lateral, fsin(angle_diff(next, heading))), fp(1, 900));
    s.y = node_y(a) + bank;
    s.road = true;
    return s;
}

void race_init(Race& race, int track_index, int racer_index) {
    race = Race{};
    race.track_index = static_cast<uint8_t>(track_index);
    const Track& t = track(track_index);

    Pod& pod = race.pod;
    pod.racer_index = static_cast<uint8_t>(racer_index);
    const Racer& rc = racer(racer_index);
    pod.engine_max = static_cast<int16_t>(
        fscale(k_engine_max, stat_scale(rc.hull, k_spread_hull)));
    pod.engine[0] = pod.engine[1] = pod.engine_max;

    const TrackNode& n = t.nodes[0];
    const TrackNode& ahead = t.nodes[1];
    pod.yaw = fatan2(node_x(ahead) - node_x(n), node_z(ahead) - node_z(n));
    pod.x = node_x(n);
    pod.z = node_z(n);
    pod.y = node_y(n) + k_hover_height;
    pod.lap = 1;
    pod.grounded = true;
    pod.tap_age = 1000;

    int slot = 0;
    for (int i = 0; i < k_racer_count; ++i) {
        if (i == racer_index) continue;
        Rival& r = race.rivals[slot];
        r.racer_index = static_cast<uint8_t>(i);
        r.phase = static_cast<uint8_t>(slot * 37);
        r.distance = -(slot + 1) * fp(13);
        const Racer& other = racer(i);
        r.pace = per_s(fp(40)) + per_s(fp(5)) * other.top + per_s(fp(1)) * other.acc;
        rival_place(r, t);
        ++slot;
    }
}

void race_tick(Race& race, const Input& in) {
    const Track& t = track(race.track_index);
    const World& w = t.world;
    Pod& pod = race.pod;
    const Racer& rc = racer(pod.racer_index);
    ++race.ticks;

    if (pod.flash_ticks > 0) --pod.flash_ticks;
    for (int i = 0; i < 2; ++i) if (pod.blast[i] > 0) --pod.blast[i];
    if (pod.tap_age < 1000) ++pod.tap_age;

    for (int i = 0; i < k_rival_count; ++i) {
        Rival& r = race.rivals[i];
        // The pace wobbles, so the pack shuffles and the position number moves.
        r.distance += r.pace + fscale(r.pace, ftrig(80, fsin((race.ticks << 5)
                                                            + (r.phase << 8))));
        rival_place(r, t);
    }

    if (pod.wreck_ticks > 0) {
        --pod.wreck_ticks;
        pod.vy -= fscale(k_gravity, w.gravity);
        pod.x += pod.vx; pod.y += pod.vy; pod.z += pod.vz;
        pod.roll += 700;
        if (pod.wreck_ticks == 0) respawn(pod, t);
        return;
    }

    // ---- boost, heat --------------------------------------------------
    if (in.boost_press) {
        if (pod.tap_age <= k_double_tap_ticks && boost_armed(pod)) {
            pod.boost_ticks = k_boost_ticks;
            pod.tap_age = 1000;
        } else {
            pod.tap_age = 0;
        }
    }
    if (pod.boost_ticks > 0) {
        --pod.boost_ticks;
        pod.heat += k_heat_rise;
    } else {
        pod.heat -= fscale(fscale(k_heat_fall, w.cooling),
                           stat_scale(rc.cool, k_spread_cool));
    }
    if (pod.heat < 0) pod.heat = 0;
    if (pod.heat >= k_heat_one) {
        pod.heat = k_heat_one;
        pod.locked = true;
        pod.boost_ticks = 0;
    }
    if (pod.locked && pod.heat < k_heat_lock) pod.locked = false;
    if (pod.heat > k_heat_warn) {
        // Over the line the engines cook, both of them evenly, which is why
        // overheat is the one damage source you cannot nurse.
        const int32_t over = ((pod.heat - k_heat_warn) * 1000) / (k_heat_one - k_heat_warn);
        const int32_t burn = fscale(k_heat_burn, over) / k_tick_hz;
        hurt(pod, 0, burn);
        hurt(pod, 1, burn);
    }

    // ---- repair -------------------------------------------------------
    if (in.repair) {
        const int32_t rate =
            fscale(k_repair, stat_scale(rc.fix, k_spread_fix)) / k_tick_hz;
        for (int i = 0; i < 2; ++i) {
            if (engine_dead(pod, i)) continue;   // not a spare part
            pod.engine[i] = static_cast<int16_t>(pod.engine[i] + rate);
            if (pod.engine[i] > pod.engine_max) pod.engine[i] = pod.engine_max;
        }
    }

    // ---- thrust -------------------------------------------------------
    int32_t per = in.throttle
        ? fscale(k_thrust, stat_scale(rc.acc, k_spread_acc)) : 0;
    if (pod.boost_ticks > 0) per = fscale(per, k_boost_thrust);
    if (in.repair) per = fscale(per, k_repair_thrust);
    // A damaged engine does not just die at zero, it weakens on the way there,
    // and it weakens on one SIDE. That is what turns damage from a bar you
    // have to remember to look at into something arriving through the stick.
    int32_t thrust[2];
    for (int i = 0; i < 2; ++i) {
        thrust[i] = engine_dead(pod, i)
            ? 0
            : fscale(per, 400 + 600 * pod.engine[i] / pod.engine_max);
    }
    const int32_t total_thrust = thrust[0] + thrust[1];
    const int alive = (engine_dead(pod, 0) ? 0 : 1) + (engine_dead(pod, 1) ? 0 : 1);

    // ---- attitude -----------------------------------------------------
    const int32_t speed = pod_speed(pod);
    const int32_t top = pod_top_speed(pod);
    const int32_t fast = speed >= top ? 1000 : (speed * 1000) / (top ? top : 1);

    int32_t yaw_max = fscale(fscale(k_yaw_max, stat_scale(rc.grip, k_spread_grip)),
                             1000 - fscale(k_yaw_speed_fall, fast));
    if (in.brake) yaw_max = fscale(yaw_max, k_yaw_brake_gain);
    if (!pod.grounded) yaw_max = fscale(yaw_max, k_yaw_air);
    const int32_t steer = (in.right ? 1 : 0) - (in.left ? 1 : 0);
    const int32_t want_yaw = steer * yaw_max;
    pod.yaw_rate += fscale(want_yaw - pod.yaw_rate, k_yaw_accel);
    // The asymmetry, and it is not a special case bolted on: it is the same
    // torque the two engines always had, running at every level of damage
    // rather than only at zero.
    //
    // MINUS, and the sign is physics rather than preference. Forward is
    // (sin yaw, cos yaw), so a growing yaw turns right. The right engine sits
    // at +x and pushes forward, and a force ahead of and outboard of the
    // centre of mass yaws the body the OTHER way, so a pod that has lost its
    // left engine pulls left, toward the side that is missing. Written the
    // other way round it pulled toward the good engine, which reads as a pod
    // fleeing its own damage.
    pod.yaw_rate -= fscale(thrust[1] - thrust[0], k_asym_yaw);
    // Repair pulls toward whichever engine is worse, so holding X costs a line
    // as well as a third of the throttle.
    if (in.repair && alive == 2) {
        // Toward the worse engine, on the same sign convention as the thrust
        // torque above.
        const int32_t diff = (pod.engine[1] - pod.engine[0]) * 1000 / pod.engine_max;
        pod.yaw_rate -= fscale(k_repair_pull, diff);
    }
    pod.yaw += pod.yaw_rate >> k_rate_fp;

    const int32_t pitch_in = (in.up ? 1 : 0) - (in.down ? 1 : 0);
    int32_t want_pitch = pitch_in * k_pitch_max;
    if (in.brake) want_pitch += 2300;
    pod.pitch_rate += fscale(want_pitch - pod.pitch, k_pitch_accel);
    pod.pitch_rate -= fscale(pod.pitch_rate, k_pitch_damp);
    pod.pitch = clamp32(pod.pitch + pod.pitch_rate, -k_pitch_max, k_pitch_max);

    // ---- the cockpit on its cables -------------------------------------
    // A real degree of freedom with its own rate, pulled back by the cables,
    // damped, and feeding back into the yaw two lines later. That feedback is
    // why a pod flung wide keeps going wide for a beat after the stick has
    // centred, and it is the one thing a rigid body with a tilt cannot do.
    const int32_t swing_want =
        clamp32(-(pod.yaw_rate * 26 >> k_rate_fp) - pod.lateral / 512,
                -k_swing_max, k_swing_max);
    pod.swing_rate += fscale(swing_want - pod.swing, k_swing_spring);
    pod.swing_rate -= fscale(pod.swing_rate, k_swing_damp);
    pod.swing = clamp32(pod.swing + pod.swing_rate, -k_swing_max, k_swing_max);
    pod.yaw_rate += fscale(pod.swing, k_swing_feed) / 100;

    const int32_t roll_want = clamp32(-(pod.yaw_rate * 18 >> k_rate_fp)
                                          + fscale(pod.swing, 550),
                                      -k_swing_max, k_swing_max);
    pod.roll += fscale(roll_want - pod.roll, 520);

    // ---- forces --------------------------------------------------------
    const int32_t fx = fsin(pod.yaw), fz = fcos(pod.yaw);
    pod.vx += ftrig(total_thrust, fx);
    pod.vz += ftrig(total_thrust, fz);
    pod.vy += ftrig(fscale(total_thrust, 450), fsin(pod.pitch));
    pod.vy -= fscale(k_gravity, w.gravity);

    if (!pod.grounded && pod.pitch > 0) {
        // Lift is speed times angle of attack, capped BELOW the local gravity
        // so a glide always extends a jump and never cancels it. At 0.82 of
        // gravity the pod descended at 0.18 g whatever the track, and on the
        // moon that was 1,889 units of glide on a 2,760 unit lap: not a jump,
        // a flight.
        const int32_t g_here = fscale(k_gravity, w.gravity);
        // Lift is speed times angle of attack, and the whole of the difficulty
        // is the units. Speed is fp16 per TICK and the angle is brads, so
        // reaching an fp16 per tick squared acceleration is
        //
        //   0.62 * (speed * 100/65536) * (pitch / 10430) * 65536/10000
        //
        // which collapses to speed * pitch * 620 / 1.043e9. Written instead as
        // a shifted sine it came out about two hundred times gravity, and the
        // pod left the planet the first time an autopilot pitched up over a
        // gap. Nothing caught it earlier because nothing had pitched up.
        int32_t lift = static_cast<int32_t>(
            (static_cast<int64_t>(speed) * pod.pitch * k_lift) / 1043000000LL);
        lift = fscale(lift, w.air);
        const int32_t ceiling = fscale(g_here, k_lift_ceiling);
        if (lift > ceiling) lift = ceiling;
        if (lift > 0) {
            pod.vy += lift;
            // The same angle bleeds forward speed, in the same units.
            const int32_t bleed = static_cast<int32_t>(
                (static_cast<int64_t>(speed) * pod.pitch * k_induced) / 1043000000LL);
            pod.vx -= ftrig(bleed, fx);
            pod.vz -= ftrig(bleed, fz);
        }
    }

    // ---- drag -----------------------------------------------------------
    // Quadratic, and the constant is solved for the top speed rather than
    // tuned: the fraction of velocity a tick removes is k * v, with v in world
    // units per tick, so the terminal velocity is where thrust and that meet.
    {
        // In 64 bits the whole way. This was int32, and `k * 1000000` with
        // the air brake out came to 2.33e9, which is past the top of an int32:
        // it wrapped NEGATIVE, so braking accelerated the pod instead of
        // slowing it and the velocity ran away to 300 units a second. ASHFALL
        // never showed it because its thin air kept the same expression under
        // the limit, which is the worst way for a bug like this to present:
        // three tracks broken, one fine, and nothing in common between them
        // that points at arithmetic.
        int64_t k = static_cast<int64_t>(324) * w.air / 1000;
        if (in.brake) k = k * k_drag_brake / 1000;
        // Off the road is slow. This is the whole penalty for leaving the
        // track now that the ground beside it no longer kills, and it wants to
        // be felt rather than survived: a pod that runs wide loses the corner,
        // and a pod that cuts across the scenery loses more than it saved.
        if (!pod.on_road && pod.grounded) k = k * k_offroad_drag / 1000;
        if (alive == 1) k = k * (1000 + k_asym_drag) / 1000;
        if (pod.boost_ticks > 0) k = k * 1000 / k_boost_top;
        const int64_t top_scale = stat_scale(rc.top, k_spread_top);
        k = k * 1000000 / (top_scale * top_scale);
        const int32_t v = flength(flength(pod.vx, pod.vz), pod.vy);
        if (v > 0) {
            int32_t frac = fmul(static_cast<int32_t>(k), v);
            if (frac > k_one) frac = k_one;
            pod.vx -= fmul(pod.vx, frac);
            pod.vz -= fmul(pod.vz, frac);
            pod.vy -= fmul(pod.vy, fscale(frac, 350));
            // Rolling drag, which is what stops a stationary pod creeping.
            const int32_t roll = fscale(k_roll_drag, w.air);
            if (v > roll) {
                pod.vx -= (int32_t)((int64_t)pod.vx * roll / v);
                pod.vz -= (int32_t)((int64_t)pod.vz * roll / v);
            } else {
                pod.vx = 0; pod.vz = 0;
            }
        }
    }

    // ---- grip: bleed the part of the velocity across the heading ---------
    // This is the drift, and HOARFROST is this one number halved.
    {
        const int32_t lat = ftrig(pod.vx, fz) - ftrig(pod.vz, fx);
        pod.lateral = lat;
        int32_t g = fscale(fscale(pod.grounded ? k_grip : k_grip_air, w.grip),
                           stat_scale(rc.grip, k_spread_grip));
        if (in.brake) g = fscale(g, 1400);
        if (g > 1000) g = 1000;
        const int32_t bleed = fscale(lat, g);
        pod.vx -= ftrig(bleed, fz);
        pod.vz += ftrig(bleed, fx);
    }

    // ---- integrate -------------------------------------------------------
    pod.x += pod.vx;
    pod.y += pod.vy;
    pod.z += pod.vz;

    // ---- the hover field --------------------------------------------------
    const uint16_t was = pod.node;
    const Surface surf = surface_at(t, pod.node, pod.x, pod.z);
    pod.node = surf.node;
    pod.on_road = surf.road;
    pod.lateral = surf.lateral;
    pod.clearance = pod.y - surf.y;
    pod.grounded = pod.clearance < k_hover_reach;

    // A lap is counted where the node index wraps forward past the start, not
    // by a trigger volume, so a pod that reverses over the line does not score.
    if (was > t.node_count - 20 && surf.node < 20) {
        if (pod.lap < 250) ++pod.lap;
        race.last_lap = race.ticks - race.lap_tick;
        if (race.best_lap == 0 || race.last_lap < race.best_lap)
            race.best_lap = race.last_lap;
        race.lap_tick = race.ticks;
        if (pod.lap > t.laps) race.finished = true;
    }

    if (pod.grounded) {
        const int32_t rest = surf.y + k_hover_height;
        const int32_t pen = rest - pod.y;
        if (pen > 0) {
            const int32_t impact = -pod.vy;
            // Only ever upward. That one line is why the pod can leave the
            // ground at all: above the rest height there is no field.
            //
            // The spring is in world units per second squared per unit of
            // penetration, so reaching fp16 per TICK squared is a division by
            // the tick rate twice, and the 65536 of the fp16 penetration
            // cancels against the 65536 the answer wants. Getting that wrong
            // by the two factors of 100 made the field about seven hundred
            // times too weak, and the pod calmly sank through the middle of
            // its own road at walking pace and wrecked on the floor below,
            // once every four seconds, dead centre of a straight.
            pod.vy += static_cast<int32_t>(
                (static_cast<int64_t>(pen) * k_hover_spring)
                / (k_tick_hz * k_tick_hz));
            pod.vy -= fscale(pod.vy, k_hover_damp);
            if (impact > k_slam_floor) {
                const int32_t d = fscale((impact - k_slam_floor) * k_tick_hz / k_one,
                                         k_slam);
                hurt(pod, 0, d);
                hurt(pod, 1, d);
                pod.flash_ticks = 14;
            }
        } else {
            // Above the rest height but still inside the field's reach, it
            // pulls down gently. Without this the pod floats a little higher
            // every crest and never settles.
            pod.vy -= static_cast<int32_t>(
                (static_cast<int64_t>(-pen) * k_hover_spring / 10)
                / (k_tick_hz * k_tick_hz));
        }
        pod.pitch -= fscale(pod.pitch, k_pitch_level);
    }

    // ---- walls -------------------------------------------------------------
    pod.scraping = false;
    if (surf.wall) {
        const TrackNode& a = t.nodes[surf.node];
        const TrackNode& b = t.nodes[(surf.node + 1) % t.node_count];
        const int32_t heading = fatan2(node_x(b) - node_x(a), node_z(b) - node_z(a));
        const int32_t half = node_half_width(a);
        const int32_t mag = (surf.lateral < 0 ? -surf.lateral : surf.lateral) - half;
        const int32_t dir = surf.lateral > 0 ? -1 : 1;
        const int32_t nx = ftrig(dir * k_one, fcos(heading));
        const int32_t nz = -ftrig(dir * k_one, fsin(heading));
        pod.x += fmul(nx, mag);
        pod.z += fmul(nz, mag);
        // nx and nz are an fp16 unit vector, so the component of velocity
        // going into the wall is a plain fp16 dot product. It was written with
        // ftrig, which expects a Q14 trig value, and then multiplied by four
        // to "undo" the shift: the two did not cancel, the impulse came out
        // four times too strong, and HOARFROST, which is three quarters wall
        // and half the grip, accelerated itself to 19,000 units a second off
        // its own scenery.
        const int32_t into = -(fmul(pod.vx, nx) + fmul(pod.vz, nz));
        if (into > 0) {
            pod.vx += fmul(nx, fscale(into, 1250));
            pod.vz += fmul(nz, fscale(into, 1250));
        }
        pod.scraping = true;
        hurt(pod, 0, k_scrape / k_tick_hz / 2);
        hurt(pod, 1, k_scrape / k_tick_hz / 2);
    }

    // ---- boost pads ---------------------------------------------------------
    if ((t.nodes[surf.node].flags & kBoost) && surf.road && pod.grounded
        && pod.boost_ticks <= 0 && !pod.locked) {
        pod.boost_ticks = k_pad_boost_ticks;
    }

    // ---- the floor -----------------------------------------------------------
    // Two ways to lose the road: fall through where it is, or fall past where
    // it would have been. The second is what a gap is, and it needs its own
    // test because a gap reports its surface as unreachably far below, so the
    // clearance above it is enormous and positive.
    if (pod.clearance < k_crash_floor) wreck(pod);
    if (pod.y < node_y(t.nodes[surf.node]) + k_crash_floor) wreck(pod);
    if (engine_dead(pod, 0) && engine_dead(pod, 1)) wreck(pod);

    // ---- position -------------------------------------------------------------
    {
        const int32_t mine = pod.node * k_node_spacing
                           + pod.lap * t.node_count * k_node_spacing;
        int place = 1;
        for (int i = 0; i < k_rival_count; ++i)
            if (race.rivals[i].distance > mine) ++place;
        race.place = static_cast<uint8_t>(place);
    }
}

}  // namespace twinflare
