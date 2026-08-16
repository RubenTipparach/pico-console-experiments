#include "sim.hpp"

#include "fixed.hpp"

namespace twinflare {
namespace {

// The roster. Every pod totals 20 points, so the choice is a shape rather than
// a ladder, and only two engine meshes exist between the six.
// The arc colour is per racer and it is not the livery. The binder is the one
// part of a podracer that is not painted, so it reads as the machine's own
// energy rather than as a stripe: a pale core of whatever the reactor is
// burning. It is also the fastest way to tell six pods apart in a pack, since
// it is the brightest thing on any of them and it is visible from behind.
constexpr Racer k_racers[k_racer_count] = {
    {"SCARAB", "SEBO VANE",  3, 3, 4, 4, 3, 3, 0,
     {{214, 124, 40}, {150, 78, 26}, {74, 52, 40}}, {255, 186, 120}},
    {"WISP", "TIK MA'ALA",   2, 5, 5, 3, 3, 2, 1,
     {{96, 214, 206}, {44, 132, 140}, {226, 232, 238}}, {150, 255, 244}},
    {"ANVIL", "GROLL HEXX",  3, 2, 2, 5, 3, 5, 2,
     {{186, 190, 198}, {96, 102, 116}, {188, 52, 52}}, {255, 128, 128}},
    {"NEEDLE", "CYRN VAST",  5, 4, 3, 1, 2, 1, 3,
     {{232, 206, 64}, {150, 126, 20}, {36, 34, 32}}, {255, 244, 140}},
    {"NIGHTJAR", "OMBRA TAL", 2, 3, 3, 3, 5, 4, 4,
     {{142, 106, 196}, {70, 50, 104}, {40, 38, 52}}, {214, 150, 255}},
    {"FANG", "KEST RHO",     4, 4, 5, 2, 3, 2, 5,
     {{122, 196, 86}, {58, 110, 48}, {184, 150, 72}}, {176, 255, 140}},
};

// Damage, which is the one thing two engines do not share.
//
// `out` is where an engine reaching zero is reported, and it is a parameter
// rather than a reach into the race because hurt is called from inside the
// hover field, the wall push and the heat model, none of which otherwise know
// there is such a thing as an Events.
bool hurt(Pod& pod, int which, int32_t amount, bool spark = true) {
    if (amount <= 0 || engine_dead(pod, which)) return false;
    pod.engine[which] = static_cast<int16_t>(pod.engine[which] - amount);
    // Sparks fly where the blow landed, and that is the whole reason damage is
    // routed through one function. Every source used to take health off both
    // engines and show nothing; a wall was the only one the renderer could see,
    // because a wall was the only one that said which side.
    if (spark) pod.hit[which] = k_hit_ticks;
    if (pod.engine[which] <= 0) {
        pod.engine[which] = 0;
        pod.dead |= static_cast<uint8_t>(1 << which);
        return true;
    }
    return false;
}

// A blow that landed on one side, shared the way a rigid frame shares one.
// `far_share` is what the engine on the OTHER side gets, in thousandths of
// what the struck one gets, so a thousand is an even split and zero is all on
// one engine.
//
// The far share is taken out first and the near engine gets the remainder,
// rather than both being computed from the ratio: these are whole health
// points and two roundings of a small amount lose a health point per hit,
// which over a long scrape is a quarter of the damage quietly not happening.
bool hurt_side(Pod& pod, int which, int32_t amount, int32_t far_share) {
    if (amount <= 0) return false;
    const int32_t far = amount * far_share / (1000 + far_share);
    bool out = hurt(pod, which ^ 1, far);
    out |= hurt(pod, which, amount - far);
    return out;
}

void wreck(Pod& pod) {
    if (pod.wreck_ticks > 0) return;
    pod.wreck_ticks = k_respawn_ticks;
    // What the pod was doing when it went, so the respawn can hand some of it
    // back. Captured HERE and not at the respawn, because by then the pod has
    // been tumbling for a second and a half and its velocity is the tumble's
    // rather than the racing line's.
    pod.wreck_speed = pod_speed(pod);
    if (pod.vy < per_s(fp(6))) pod.vy = per_s(fp(6));
}

// Back on the road with both engines at half, which is the only forgiving
// thing in the whole model. A three lap race that ends on lap one is a race
// nobody runs twice.
//
// BACK ONTO ROAD, AND WITH A RUN UP. The node the pod wrecked at is very often
// the node it wrecked IN: a gap reports no surface at all, so falling down one
// leaves the nearest node somewhere in the middle of the hole, and putting the
// pod back there drops it straight down the same hole. Measured on DUNE SEA,
// whose gap is seven nodes and fifty six units wide: a pod that lost an engine
// arrived too slow to jump it, fell, and respawned at node 66, which is the
// middle of the gap, at eleven units a second.
//
// So walk back until there is a run of real road, and place the pod at the far
// end of that run rather than at its lip. Both halves matter. Without the walk
// the pod is in the hole; without the run up it is on the edge of the hole with
// no room to get moving, which is the same outcome one second later.
void respawn(Pod& pod, const Track& t) {
    uint16_t node = pod.node;
    int road_run = 0;
    for (int back = 0; back < k_respawn_search; ++back) {
        // Written the long way round on purpose: `a + n - back % n` reads as
        // the wrap it is meant to be and is not, because the modulo binds
        // tighter than the subtraction. It happens to come out right while the
        // search is shorter than a lap, which is a bug waiting for a short
        // track.
        const int32_t step = back % t.node_count;
        const int32_t i = (pod.node + t.node_count - step) % t.node_count;
        if (t.nodes[i].flags & kGap) { road_run = 0; continue; }
        ++road_run;
        node = i;
        if (road_run >= k_respawn_runup) break;
    }
    pod.node = node;
    const TrackNode& n = t.nodes[node];
    const TrackNode& ahead = t.nodes[(node + 1) % t.node_count];
    const int32_t heading = fatan2(node_x(ahead) - node_x(n), node_z(ahead) - node_z(n));
    pod.x = node_x(n);
    pod.z = node_z(n);
    pod.y = node_y(n) + k_hover_height;
    // MOVING. Eleven units a second on a track whose top speed is ninety is a
    // standing start, and a standing start after every mistake turns one error
    // into most of a lap. Half of what the pod was doing when it went, which is
    // the racing convention, with a floor so a crash at walking pace still
    // rolls away and a cap so a crash at full speed is still a real penalty.
    int32_t speed = pod.wreck_speed / 2;
    const int32_t top = pod_top_speed(pod);
    const int32_t floor_speed = fscale(top, k_respawn_floor);
    const int32_t cap = fscale(top, k_respawn_cap);
    if (speed < floor_speed) speed = floor_speed;
    if (speed > cap) speed = cap;
    pod.wreck_speed = 0;
    pod.vx = ftrig(speed, fsin(heading));
    pod.vz = ftrig(speed, fcos(heading));
    pod.vy = 0;
    pod.yaw = heading;
    pod.yaw_rate = 0;
    pod.pitch = 0;
    pod.pitch_rate = 0;
    pod.roll = 0;
    pod.swing = 0;
    pod.swing_rate = 0;
    pod.dead = 0;
    pod.fuse = 0;
    pod.engine[0] = pod.engine[1] = static_cast<int16_t>(pod.engine_max / 2);
    pod.heat = 0;
    pod.locked = false;
    pod.boost_ticks = 0;
    pod.wreck_ticks = 0;
}

// Touching a rival, which is the one collision this game did not have. Rivals
// were scenery: five shapes moving past the camera at a plausible speed, drawn
// and never asked about. Racing through the pack cost nothing, so the pack was
// something to look at rather than something to get past.
//
// Tested in the POD'S OWN FRAME, because the answer the damage model needs is
// not "how far apart" but "which side", and that is a projection onto the
// pod's across axis rather than a distance. A box rather than a radius for the
// same reason: a pod is three and a half times longer than it is wide, and a
// circle round it is either too wide alongside or too short in front.
//
// Returns the engine that was hit, or -1 for no contact.
int rival_contact(const Pod& pod, const Rival& r, int32_t& across_out) {
    const int32_t dy = r.y - pod.y;
    // The height band first, and it is the cheap test as well as the one that
    // matters: without it a rival on the road under a bridge is a collision
    // with something the player cannot even see.
    if (dy > k_bump_high || dy < -k_bump_high) return -1;
    const int32_t dx = r.x - pod.x, dz = r.z - pod.z;
    const int32_t fx = fsin(pod.yaw), fz = fcos(pod.yaw);
    const int32_t along = ftrig(dx, fx) + ftrig(dz, fz);
    if (along > k_bump_long || along < -k_bump_long) return -1;
    const int32_t across = ftrig(dx, fz) - ftrig(dz, fx);
    if (across > k_bump_lat || across < -k_bump_lat) return -1;
    across_out = across;
    // Positive across is to starboard, which is engine 1. A rival dead centre
    // is charged to the port engine, which is arbitrary and has to be
    // something: two pods exactly nose to nose is a case, not an error.
    return across > 0 ? 1 : 0;
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
    // IN 64 BITS. `frac` runs to a whole node spacing, which is fp(8) =
    // 524,288, and 524,288 << 16 is 3.4e10: past the top of an int32 by a
    // factor of sixteen. Only the first six percent of a segment fitted, so a
    // rival's position along the segment it was on was correct for half a unit
    // and aliased rubbish for the remaining seven and a half. What that looked
    // like was a rival sitting at a node and then jumping the whole eight units
    // to the next one, five times a second: "a different update rate with no
    // interpolation", which is exactly what it was, and it had been there since
    // the rivals were written.
    const int32_t u = static_cast<int32_t>(
        (static_cast<int64_t>(frac) << k_fp) / k_node_spacing);

    const int32_t ax = node_x(a), az = node_z(a), ay = node_y(a);
    const int32_t bx = node_x(b), bz = node_z(b), by = node_y(b);

    // THE HEADING IS INTERPOLATED, and that is the whole of the rivals'
    // stutter. It used to be the heading of the segment the rival was on: a
    // step function that jumped at every node, once every eight units, five
    // times a second at racing speed. The yaw visibly snapped, and worse, the
    // lateral weave below is measured off it, so the rival's POSITION jumped
    // sideways by up to a unit at every one of those steps. It looked exactly
    // like something being updated at a slower rate than the frame and never
    // interpolated, because that is what it was.
    //
    // Mitred at each node, the same average of arriving and leaving segments
    // the renderer offsets its ground bands along, then lerped across the
    // segment. Three arctangents a rival a tick.
    const TrackNode& before = t.nodes[(index + count - 1) % count];
    const TrackNode& after = t.nodes[(index + 2) % count];
    const int32_t h_prev = fatan2(ax - node_x(before), az - node_z(before));
    const int32_t h_here = fatan2(bx - ax, bz - az);
    const int32_t h_next = fatan2(node_x(after) - bx, node_z(after) - bz);
    const int32_t turn_a = angle_diff(h_here, h_prev);
    const int32_t turn_b = angle_diff(h_next, h_here);
    const int32_t head_a = h_prev + turn_a / 2;
    const int32_t head_b = h_here + turn_b / 2;
    const int32_t heading = head_a + fmul(angle_diff(head_b, head_a), u);

    // A weave, so the pack is not a line of pods nose to tail. Deterministic
    // from the tick and the rival's own phase, which is what makes a replay of
    // the same race the same race.
    const int32_t wob = ftrig(fp(2, 400), fsin((r.phase << 8) + (r.distance >> 7)));
    const int32_t off = ftrig(fp(3), fsin(r.phase << 9)) + wob;

    // AND THE LINE IS A CURVE. Straight between two nodes, the position is
    // continuous and its DIRECTION is not: the velocity turns a corner at every
    // node, so a rival visibly kinks five times a second even with a smooth
    // heading. Catmull-Rom through the four nodes around it, written as offsets
    // from the one it is on so no coefficient ever gets near the top of an
    // int32, and Horner so it is three multiplies an axis.
    const int32_t p0x = node_x(before) - ax, p2x = bx - ax, p3x = node_x(after) - ax;
    const int32_t p0z = node_z(before) - az, p2z = bz - az, p3z = node_z(after) - az;
    const auto spline = [u](int32_t p0, int32_t p2, int32_t p3) {
        int32_t v = -p0 - 3 * p2 + p3;
        v = fmul(v, u) + (2 * p0 + 4 * p2 - p3);
        v = fmul(v, u) + (p2 - p0);
        return fmul(v, u) / 2;
    };
    r.x = ax + spline(p0x, p2x, p3x) + ftrig(off, fcos(heading));
    r.z = az + spline(p0z, p2z, p3z) - ftrig(off, fsin(heading));
    r.y = ay + fmul(by - ay, u) + k_hover_height;
    r.yaw = heading;
    // Banked off the curvature, lerped across the segment for the same reason
    // the heading is: a roll that changes in steps reads as a twitch.
    const int32_t turn = turn_a + fmul(turn_b - turn_a, u);
    r.roll = clamp32(-turn * 14, -k_swing_max, k_swing_max);
}

// Where the player is in the field, which is one comparison against every
// rival's distance and therefore wants writing down once. It is called from
// race_init as well as from race_tick, because the countdown returns before the
// end of the tick and a grid that reports position zero for three seconds is a
// HUD with a hole in it.
//
// pod.lap COUNTS FROM ONE and a rival's distance counts from zero at the line,
// so the minus one is the whole of this function. Without it the player was
// credited a lap they had not driven: measured over a full three lap race that
// was position 1 for all 9,412 ticks of it, whatever the rest of the field did,
// because the pack had to make up 2,408 units of nothing before the number
// could move.
void set_place(Race& race, const Track& t) {
    const int32_t mine = race.pod.node * k_node_spacing
                       + (race.pod.lap - 1) * t.node_count * k_node_spacing;
    int place = 1;
    for (int i = 0; i < k_rival_count; ++i)
        if (race.rivals[i].distance > mine) ++place;
    race.place = static_cast<uint8_t>(place);
}

// What flies the pod once the player's race is over. Aim a few nodes up the
// road and hold the throttle, which is all a pod needs to keep looking like a
// pod for the twenty seconds the rest of the field takes to come in.
//
// Deliberately not a good driver: it does not brake, boost or repair, so it
// cannot beat the lap the player just set, and it cannot be mistaken for a
// rival AI either. It exists so there is something in front of the camera.
Input autopilot(const Race& race, const Track& t) {
    Input in{};
    const Pod& p = race.pod;
    // Far enough ahead to turn early. Aiming at the next node steers at the
    // road under the pod, which on anything tighter than a sweeper is a pod
    // sawing between the two walls of its own corner.
    const TrackNode& ahead = t.nodes[(p.node + 4) % t.node_count];
    const int32_t want = fatan2(node_x(ahead) - p.x, node_z(ahead) - p.z);
    const int32_t err = angle_diff(want, p.yaw);
    in.throttle = true;
    in.left = err < -k_auto_deadband;
    in.right = err > k_auto_deadband;
    return in;
}

}  // namespace

void merge_events(Events& into, const Events& from) {
    into.count |= from.count;
    into.go |= from.go;
    into.lap |= from.lap;
    into.finish |= from.finish;
    into.boost |= from.boost;
    into.launch |= from.launch;
    into.flood |= from.flood;
    into.bump |= from.bump;
    into.scrape |= from.scrape;
    into.slam |= from.slam;
    into.engine_out |= from.engine_out;
    into.fuse_lit |= from.fuse_lit;
    into.fuse_beat |= from.fuse_beat;
    into.wreck |= from.wreck;
    // Levels, not edges: the latest value wins. OR'd like the rest, `grinding`
    // would latch on for the whole frame after one tick against the rock and
    // `rev` would climb to the loudest tick and stay there.
    into.grinding = from.grinding;
    into.rev = from.rev;
}

int countdown_number(const Race& race) {
    if (race.phase != Phase::Countdown) return 0;
    if (race.countdown <= 0) return 0;
    // Ceiling, so the whole of the last second reads 1 rather than the number
    // flicking to zero a second before the pod is allowed to move. GO is the
    // zero, and it belongs to the tick the countdown actually ends on.
    const int n = (race.countdown + k_tick_hz - 1) / k_tick_hz;
    return n > 3 ? 3 : n;
}

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

int32_t node_wall(const TrackNode& n) {
    if (n.flags & kTunnel) return k_tunnel_height;
    if (n.flags & kWall) return k_wall_height;
    return 0;
}

int32_t ground_offset(int32_t wall_h, int32_t over) {
    if (over <= 0) return 0;                       // on the road
    // A wall is a vertical face at the road edge and a plateau beyond it, so
    // past the edge the ground simply IS the top of the rock.
    if (wall_h > 0) return wall_h;
    if (over >= k_shoulder_run) return -k_shoulder_drop;
    return -static_cast<int32_t>(
        (static_cast<int64_t>(over) * k_shoulder_drop) / k_shoulder_run);
}

Surface surface_at(const Track& t, uint16_t near_node, int32_t x, int32_t z) {
    uint16_t i = nearest_node(t, near_node, x, z);
    // WHICH SEGMENT, not which node. A point just behind the nearest node lies
    // on the segment ARRIVING at it, and reading it off the segment leaving
    // instead pins its height to that node's own rather than interpolating
    // back. The surface then steps at every cell boundary: invisible on flat
    // desert, and two and a half units of ground appearing out of nowhere at
    // the lip of a ramp, where the road drops five units in one node.
    int32_t heading = 0, dx = 0, dz = 0, along = 0;
    for (int pass = 0; pass < 2; ++pass) {
        const TrackNode& a = t.nodes[i];
        const TrackNode& b = t.nodes[(i + 1) % t.node_count];
        heading = fatan2(node_x(b) - node_x(a), node_z(b) - node_z(a));
        dx = x - node_x(a);
        dz = z - node_z(a);
        along = ftrig(dx, fsin(heading)) + ftrig(dz, fcos(heading));
        if (along >= 0 || pass == 1) break;
        i = static_cast<uint16_t>((i + t.node_count - 1) % t.node_count);
    }
    const TrackNode& a = t.nodes[i];
    const TrackNode& b = t.nodes[(i + 1) % t.node_count];

    Surface s{};
    s.node = i;
    // Signed lateral offset, in the node's own frame.
    s.lateral = ftrig(dx, fcos(heading)) - ftrig(dz, fsin(heading));

    // Height INTERPOLATED along the segment rather than read off the nearest
    // node. The nearest node is a step function eight units wide, and the road
    // the renderer draws is not: a quad runs from node i's height to node
    // j's, so on a slope the drawn surface and the surface the hover field
    // pushes off disagreed by up to half a step, and the pod rode visibly sunk
    // into the road going downhill and floating over it going up. Two answers
    // for the same piece of road, and the eye believes the one it can see.
    const int32_t u = static_cast<int32_t>(
        (static_cast<int64_t>(clamp32(along, 0, k_node_spacing)) << k_fp)
        / k_node_spacing);
    const int32_t ground = node_y(a) + fmul(node_y(b) - node_y(a), u);
    // The rock at the road edge, interpolated along the segment exactly as the
    // renderer draws it. A canyon therefore rises out of the desert and sinks
    // back into it over one node's spacing rather than appearing whole, and the
    // sim and the renderer agree about every height in between.
    const int32_t wall_h = node_wall(a) + fmul(node_wall(b) - node_wall(a), u);

    if (a.flags & kGap) {
        // A gap carries no surface at all. The hover field has nothing to push
        // against, so you fall, and that is the entire implementation of
        // "jump this or fall in": no trigger volume and no jump code.
        s.y = fp(-10000);
        s.road = false;
    } else {
        const int32_t half = node_half_width(a);
        const int32_t away = s.lateral < 0 ? -s.lateral : s.lateral;
        const int32_t over = away - half;
        // Where the pod runs out of road. A canyon stops it at the road edge; a
        // railing stops it eighteen units past one. Reported, because the push
        // in race_tick has to know which line it is pushing back to and those
        // are not the same line.
        s.limit = (wall_h > k_hover_height) ? half : half + k_verge;
        if (over > 0) {
            // Off the road is a SHOULDER, not a cliff, and its exact shape is
            // ground_offset's, which the renderer draws from too.
            //
            // It used to fall away by up to thirty units, and the crash floor
            // is twenty six, so drifting wide did not cost you time, it killed
            // you: the ground vanished downward, the pod followed it, and the
            // run ended. That is not what going off line should mean in a
            // racing game, and it is not what the hover field is for either,
            // since a field that holds you over the road but drops you beside
            // it is a trapdoor.
            //
            // Three units down and no further. The hover field still has
            // something to push against out here, so the pod stays flyable and
            // can be driven back on; what it costs is grip and speed, applied
            // in race_tick. Falling is reserved for a GAP, where there is
            // genuinely no road.
            // THE SURFACE UNDER THE BARRIER, not the top of it. A canyon wall
            // used to be terrain all the way up, so hitting one lifted the pod
            // eleven units onto the plateau: the hover field found the rock
            // above it, the hard floor placed the pod on top, and you popped
            // over the wall you had just crashed into. Clamping the profile to
            // the blocking line means the field is pushing off the ground the
            // pod is actually standing on, which is the road at the foot of the
            // wall. The push a few lines down does the rest.
            const int32_t reach = s.limit - half;
            s.y = ground + ground_offset(wall_h, over < reach ? over : reach);
            // Blocked only where something stands higher than the pod floats.
            // Below that the taper at a canyon mouth is drivable rock, which
            // is exactly what it looks like, and the pod is pushed out the
            // moment the rock is tall enough to be a wall.
            if (away >= s.limit) s.wall = true;
        } else {
            // Banking: the road tilts into its own turn, which is free grip on
            // a corner and the reason a circuit can be fast and tight at once.
            const TrackNode& c = t.nodes[(i + 3) % t.node_count];
            const int32_t next = fatan2(node_x(c) - node_x(b), node_z(c) - node_z(b));
            const int32_t bank =
                -fmul(ftrig(s.lateral, fsin(angle_diff(next, heading))), fp(1, 900));
            s.y = ground + bank;
            s.road = true;
        }
        // A roof, likewise interpolated, so a tunnel mouth closes over the pod
        // rather than snapping shut on it.
        if ((a.flags | b.flags) & kTunnel) {
            s.roofed = true;
            s.roof = ground + k_tunnel_height;
        }
    }

    // THE SEA, where there is nothing else to push off.
    //
    // This used to be the last word on where the ground is: anything below the
    // waterline was raised TO the waterline, so the hover field always had
    // waves under it and the pod skimmed the surface. That made the causeway
    // drivable, and it also meant TIDEBREAK had no underwater section at all.
    // Reported from playing it, and the numbers agree: 98 of 304 nodes sit
    // under the sea, the deepest 14 units down, and the pod rode 2.6 units
    // ABOVE the waterline over every one of them. A third of the lap was
    // described as submerged and was flown across the top.
    //
    // So the sea is a surface only where the track has none: a GAP. Jump one
    // short and it is still a splash rather than a grave, which is the part of
    // the old rule worth keeping. Everywhere the track has real ground, that
    // ground is the surface whether or not it is under water, and the pod goes
    // down with it.
    if (has_water(t) && (a.flags & kGap) && s.y < water_level(t)) {
        s.y = water_level(t);
        s.water = true;
        s.wall = false;   // there is nothing to grind against out at sea
    }
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

    race.phase = Phase::Countdown;
    race.countdown = k_count_ticks;

    int slot = 0;
    for (int i = 0; i < k_racer_count; ++i) {
        if (i == racer_index) continue;
        Rival& r = race.rivals[slot];
        r.racer_index = static_cast<uint8_t>(i);
        r.phase = static_cast<uint8_t>(slot * 37);
        r.distance = -(slot + 1) * fp(13);
        r.lap = 1;
        const Racer& other = racer(i);
        // THE PACE, and it is left exactly where it was, which took measuring
        // to establish. Fixing the position number above made the whole field
        // look too fast, and the first move was to make it faster still on the
        // strength of one flat out driver that laps DUNE SEA in 108 seconds.
        // Measured properly, this pace brings the field home between 105 and
        // 136 seconds, and the two reference drivers in the test suite lap in
        // 108 and 142: the fast one finishes second, the careful one last. That
        // is the bracket a race wants, and it was already here.
        r.pace = per_s(fp(40)) + per_s(fp(5)) * other.top + per_s(fp(1)) * other.acc;
        rival_place(r, t);
        ++slot;
    }
    set_place(race, t);
}

void standings(const Race& race, Standing out[k_racer_count]) {
    const Track& t = track(race.track_index);
    out[0].racer_index = race.pod.racer_index;
    out[0].ticks = race.finished ? race.finish_tick : 0;
    out[0].lap = race.pod.lap;
    // The same origin the position number is measured against: the player's lap
    // counts from one and a rival's distance counts from zero on the line.
    out[0].progress = race.pod.node * k_node_spacing
                    + (race.pod.lap - 1) * t.node_count * k_node_spacing;
    out[0].player = true;
    out[0].finished = race.finished;
    for (int i = 0; i < k_rival_count; ++i) {
        Standing& s = out[i + 1];
        s.racer_index = race.rivals[i].racer_index;
        s.ticks = race.rivals[i].finish_tick;
        s.lap = race.rivals[i].lap;
        s.progress = race.rivals[i].distance;
        s.player = false;
        s.finished = race.rivals[i].finish_tick != 0;
    }
    // An insertion sort over six rows, which is the whole of the sorting this
    // game needs and costs less than the call to a real one would.
    //
    // A racer that has crossed is ordered by its time and sits above every
    // racer still out there, whatever lap that one is on: a rival on the last
    // lap has not finished and cannot be placed above somebody who has.
    for (int i = 1; i < k_racer_count; ++i) {
        Standing key = out[i];
        int j = i - 1;
        while (j >= 0) {
            const Standing& a = out[j];
            bool after;
            if (a.finished != key.finished) after = !a.finished;
            else if (a.finished) after = a.ticks > key.ticks;
            else after = a.progress < key.progress;
            if (!after) break;
            out[j + 1] = out[j];
            --j;
        }
        out[j + 1] = key;
    }
}

void race_tick(Race& race, const Input& raw) {
    const Track& t = track(race.track_index);
    const World& w = t.world;
    Pod& pod = race.pod;
    const Racer& rc = racer(pod.racer_index);
    race.ev = Events{};
    ++race.ticks;

    // Who is flying. Past the line the player's race is over and the pod keeps
    // going, because a pod abandoned in mid air while the rest of the field
    // comes in is a worse thing to look at than a chase camera on an empty
    // cockpit. The autopilot is the sim's, not the SDK layer's, so what happens
    // after the flag is in the half the host tests can run.
    const Input in = race.phase == Phase::Finished ? autopilot(race, t) : raw;

    if (pod.flash_ticks > 0) --pod.flash_ticks;
    for (int i = 0; i < 2; ++i) {
        if (pod.hit[i] > 0) --pod.hit[i];
    }
    if (pod.bump_ticks > 0) --pod.bump_ticks;
    if (pod.tap_age < 1000) ++pod.tap_age;

    // ---- the start ---------------------------------------------------------
    // Everything is held: the pod, the pack, and the clock. The one thing that
    // moves is the charge, which is what makes three seconds of nothing worth
    // sitting through.
    if (race.phase == Phase::Countdown) {
        --race.ticks;                       // the clock starts on green
        const int was = countdown_number(race);
        --race.countdown;
        if (countdown_number(race) != was) race.ev.count = true;

        if (!race.flooded) {
            if (in.throttle) {
                race.charge = static_cast<int16_t>(race.charge + k_charge_rise);
                if (race.charge > k_charge_one) race.charge = k_charge_one;
            } else if (race.charge > 0) {
                race.charge = static_cast<int16_t>(race.charge - k_charge_fall);
                if (race.charge < 0) race.charge = 0;
            }
            // Over the flood line the engines cook and the charge is gone with
            // them. That is the whole tension of the three seconds: the charge
            // worth the most is the one just short of the one that holes the
            // pod.
            //
            // AND IT LATCHES. Reset to zero alone, a finger still on the
            // throttle simply refills the charge and blows it again, which cost
            // three quarters of both engines in one countdown when it was
            // measured. One mistake is one mistake.
            if (race.charge >= k_charge_flood) {
                hurt(pod, 0, k_charge_burn);
                hurt(pod, 1, k_charge_burn);
                race.charge = 0;
                race.flooded = true;
                race.ev.flood = true;
                pod.flash_ticks = 20;
            }
        }

        // The engines wind up audibly with the charge, which is the whole of
        // "power up the engines during the countdown" as a sound: the note
        // rising is the instrument, and the bar on screen is the readout.
        race.ev.rev = static_cast<uint8_t>(30 + race.charge * 170 / k_charge_one);

        if (race.countdown <= 0) {
            race.phase = Phase::Racing;
            race.ev.go = true;
            // The charge cashes in as boost, so a launch is worth about what a
            // boost pad is and the pod leaves the line already moving.
            if (race.charge > 0) {
                pod.boost_ticks =
                    static_cast<int16_t>(k_launch_ticks * race.charge / k_charge_one);
                race.ev.launch = true;
                race.ev.boost = true;
            }
        }
        return;
    }

    for (int i = 0; i < k_rival_count; ++i) {
        Rival& r = race.rivals[i];
        if (r.finish_tick != 0) continue;   // parked on the line, its race is run
        // The pace wobbles, so the pack shuffles and the position number moves.
        r.distance += r.pace + fscale(r.pace, ftrig(80, fsin((race.ticks << 5)
                                                            + (r.phase << 8))));
        rival_place(r, t);
        // A rival's own lap counter, off the one number a rival has. It starts
        // behind the line at a negative distance, so a lap is only banked once
        // the distance is positive and integer division truncates the right
        // way round.
        const int32_t lap_len = t.node_count * k_node_spacing;
        const int32_t done = r.distance > 0 ? r.distance / lap_len : 0;
        r.lap = static_cast<uint8_t>(done < 250 ? done + 1 : 250);
        if (done >= t.laps) r.finish_tick = race.ticks;
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
            race.ev.boost = true;
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
        // Evenly, and with no sparks: heat is the one damage source that is not
        // an impact. Both engines are cooking, and a shower of sparks off a
        // pod nothing has touched reads as a collision the player did not have.
        if (hurt(pod, 0, burn, false)) race.ev.engine_out = true;
        if (hurt(pod, 1, burn, false)) race.ev.engine_out = true;
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
        // Off the road is slow, and so is the sea. This is the whole penalty
        // for leaving the track now that the ground beside it no longer kills,
        // and it wants to be felt rather than survived: a pod that runs wide
        // loses the corner, and a pod that cuts across the scenery loses more
        // than it saved. Whichever surface is worse wins, so the shallows at
        // the edge of the causeway are as rough as the rocks are.
        if (pod.grounded) {
            int64_t rough = 1000;
            if (!pod.on_road) rough = k_offroad_drag;
            // Skimming the sea and being under it are both wet, and being
            // under it is the slower of the two if anything, so neither gets
            // a free pass through the water drag.
            if ((pod.over_water || pod.submerged) && rough < k_water_drag)
                rough = k_water_drag;
            k = k * rough / 1000;
        }
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
    pod.over_water = surf.water;
    // Read off the pod's own height rather than off the surface, because that
    // is the question: a pod diving into the trench is under the sea from the
    // moment it crosses the line, whatever the road below it is doing.
    pod.submerged = has_water(t) && pod.y < water_level(t);
    // A band either side of the waterline rather than a crossing test, because
    // a crossing is one tick and spray that lasts one tick is spray nobody
    // sees. Wide enough to cover a pod hovering on the sea over a gap, which
    // is the other case that should throw it.
    const int32_t off_surface = pod.y - water_level(t);
    pod.splashing = has_water(t)
                 && off_surface > -k_splash_band && off_surface < k_splash_band;
    pod.roofed = surf.roofed;
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
        race.ev.lap = true;
        if (pod.lap > t.laps && race.phase == Phase::Racing) {
            race.finished = true;
            race.phase = Phase::Finished;
            race.finish_tick = race.ticks;
            race.ev.finish = true;
        }
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
            // Hitting the ground hard costs ENGINE HEALTH, on both engines,
            // and that is the whole price of a bad landing now: the pod cannot
            // be driven into the floor, so a slam has to be paid for somewhere
            // or a ramp is free. Measured on the velocity the pod arrived
            // with, before the spring and the floor below have had it.
            if (impact > k_slam_floor) {
                const int32_t d = fscale((impact - k_slam_floor) * k_tick_hz / k_one,
                                         k_slam);
                // ON WHICHEVER ENGINE IS LOW, because a pod does not land flat
                // unless it happens to be level. The local frame puts engine 0
                // to port and a positive roll drops it, so the sign of the roll
                // is the sign of which engine meets the ground first, and a
                // landing out of a corner costs the inside engine most.
                //
                // Level, the lean is zero and this is the even split it always
                // was, so a straight line drop is unchanged.
                const int32_t tilt = pod.roll < 0 ? -pod.roll : pod.roll;
                const int32_t lean =
                    k_slam_lean * (tilt > k_swing_max ? k_swing_max : tilt) / k_swing_max;
                const int low = pod.roll > 0 ? 0 : 1;
                // Both engines carry a slam, so the damage is doubled before
                // the split: the old line took `d` off each rather than d in
                // total, and a landing that stopped costing what it used to
                // cost is a landing that stopped mattering.
                if (hurt_side(pod, low, d * 2, 1000 - lean))
                    race.ev.engine_out = true;
                pod.flash_ticks = 14;
                race.ev.slam = true;
            }
        } else {
            // Above the rest height but still inside the field's reach, it
            // pulls down gently. Without this the pod floats a little higher
            // every crest and never settles.
            pod.vy -= static_cast<int32_t>(
                (static_cast<int64_t>(-pen) * k_hover_spring / 10)
                / (k_tick_hz * k_tick_hz));
        }

        // AND A FLOOR UNDER THE SPRING. The spring on its own is a spring: hit
        // it hard enough, or land on a rising slope, and the pod goes through
        // the surface for a few ticks while the spring catches up. It looked
        // exactly like the thing the hover field exists to prevent, because it
        // was: the pod sank into the road.
        //
        // The field is a floor as well, and this is the line that says so.
        // Below it the pod is placed, not pushed, and any downward velocity is
        // gone. Not a bounce: a bounce off a force field reads as a mistake.
        // The spring above still does all the feel, breathing over a crest and
        // compressing into a dip; this only catches what the spring cannot.
        const int32_t floor_y = surf.y + k_hover_floor;
        if (pod.y < floor_y) {
            pod.y = floor_y;
            pod.clearance = k_hover_floor;
            if (pod.vy < 0) pod.vy = 0;
        }
        pod.pitch -= fscale(pod.pitch, k_pitch_level);
    }

    // ---- the roof ------------------------------------------------------------
    // A tunnel takes the sky away, and that is the whole reason to put one on a
    // track whose answer to every other problem is to pitch up and glide. The
    // ceiling is the floor's mirror: the pod is placed, not pushed, and upward
    // velocity is gone. It applies whether or not the field has hold of the
    // pod, because a pod that entered the tunnel airborne is exactly the one
    // that needs stopping.
    if (surf.roofed && pod.y > surf.roof) {
        pod.y = surf.roof;
        pod.clearance = pod.y - surf.y;
        if (pod.vy > 0) pod.vy = 0;
    }

    // ---- walls -------------------------------------------------------------
    const bool was_scraping = pod.scraping;
    pod.scraping = false;
    pod.scrape = 0;
    if (surf.wall) {
        if (!was_scraping) race.ev.scrape = true;
        const TrackNode& a = t.nodes[surf.node];
        const TrackNode& b = t.nodes[(surf.node + 1) % t.node_count];
        const int32_t heading = fatan2(node_x(b) - node_x(a), node_z(b) - node_z(a));
        // Back to the line the surface said stops the pod, which is the road
        // edge inside a canyon and the railing out on the open desert.
        const int32_t mag = (surf.lateral < 0 ? -surf.lateral : surf.lateral)
                          - surf.limit;
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
        // Which side is grinding, so the renderer knows where to put the
        // sparks. The sign of the lateral offset is the side of the road the
        // pod ran out on, which is the side the wall is on.
        pod.scrape = surf.lateral > 0 ? 1 : -1;
        // ON A CADENCE, and it is not laziness. The scrape is a RATE, 460
        // health a second, which is four and a half a tick: an integer, so
        // four, and splitting four unevenly between two engines rounds the far
        // one to nothing. Applied four ticks at a time there are eighteen
        // points to divide and the split survives, at a granularity of forty
        // milliseconds that no bar on a 120 pixel screen can show.
        //
        // The old line took the same off both engines every tick, which is a
        // fair model of a pod dropped down a well and a poor one of a pod
        // running its port engine along a canyon.
        if (race.ticks % k_scrape_every == 0) {
            const int engine = pod.scrape > 0 ? 1 : 0;
            if (hurt_side(pod, engine, k_scrape * k_scrape_every / k_tick_hz,
                          k_scrape_far))
                race.ev.engine_out = true;
        }
    }

    // ---- boost pads ---------------------------------------------------------
    if ((t.nodes[surf.node].flags & kBoost) && surf.road && pod.grounded
        && pod.boost_ticks <= 0 && !pod.locked) {
        pod.boost_ticks = k_pad_boost_ticks;
        race.ev.boost = true;
    }

    // ---- the floor -----------------------------------------------------------
    // Two ways to lose the road: fall through where it is, or fall past where
    // it would have been. The second is what a gap is, and it needs its own
    // test because a gap reports its surface as unreachably far below, so the
    // clearance above it is enormous and positive.
    const bool was_wrecked = pod.wreck_ticks > 0;
    if (pod.clearance < k_crash_floor) wreck(pod);
    if (pod.y < node_y(t.nodes[surf.node]) + k_crash_floor) wreck(pod);

    // ---- one engine is a fuse, not a handicap -----------------------------
    // Down to one engine, the pod is coming apart, and three seconds later it
    // does. Nothing stops the clock: repair cannot resurrect a dead engine, so
    // there is nothing to nurse and no button to hold. What the three seconds
    // buy is the chance to reach a line, back off a jump, or watch it coming.
    //
    // Evaluated AFTER every damage source this tick, so an engine that goes out
    // to a wall lights the fuse on the tick it went and not the one after.
    const int alive_now =
        (engine_dead(pod, 0) ? 0 : 1) + (engine_dead(pod, 1) ? 0 : 1);
    if (alive_now == 1 && pod.wreck_ticks == 0) {
        if (pod.fuse <= 0) {
            pod.fuse = k_fuse_ticks;
            race.ev.fuse_lit = true;
        } else {
            const int was_left = fuse_seconds(pod);
            --pod.fuse;
            if (fuse_seconds(pod) != was_left) race.ev.fuse_beat = true;
        }
        if (pod.fuse <= 0) {
            pod.fuse = 0;
            // It goes WITH the other one. The pod does not limp on with an
            // engine at zero, it comes apart, so the surviving engine is taken
            // too and the wreck below is the one that has always been there:
            // both engines out. One wreck path, not two.
            hurt(pod, 0, pod.engine_max);
            hurt(pod, 1, pod.engine_max);
            race.ev.engine_out = true;
        }
    } else {
        pod.fuse = 0;
    }
    if (engine_dead(pod, 0) && engine_dead(pod, 1)) wreck(pod);
    if (!was_wrecked && pod.wreck_ticks > 0) race.ev.wreck = true;

    // ---- touching a rival ------------------------------------------------------
    // After the integration and after the wall push, so the position tested is
    // the one the renderer is about to draw. Only one rival can be hit per
    // tick: two pods arriving on both flanks at once is a case worth having,
    // and taking two hits from it in one tick is not.
    if (pod.wreck_ticks == 0 && pod.bump_ticks == 0) {
        for (int i = 0; i < k_rival_count; ++i) {
            const Rival& r = race.rivals[i];
            if (r.finish_tick != 0) continue;
            int32_t across = 0;
            const int engine = rival_contact(pod, r, across);
            if (engine < 0) continue;
            if (hurt(pod, engine, k_bump)) race.ev.engine_out = true;
            pod.bump_ticks = k_bump_ticks;
            pod.flash_ticks = 12;
            race.ev.bump = true;
            // Shoved apart along the pod's own across axis, away from whatever
            // it touched. The rival is on rails and cannot be pushed, so the
            // whole exchange lands on the pod, which is also the honest reading
            // of a light pod bouncing off a heavier one.
            const int32_t fx = fsin(pod.yaw), fz = fcos(pod.yaw);
            const int32_t dir = across > 0 ? -1 : 1;
            pod.vx += ftrig(dir * k_bump_push, fz);
            pod.vz -= ftrig(dir * k_bump_push, fx);
            break;
        }
    }

    // ---- position -------------------------------------------------------------
    set_place(race, t);

    // ---- the flag ---------------------------------------------------------------
    // The sequence has an end even if the field does not come in: a rival is a
    // number that always rises, but a track long enough and a pace low enough
    // is a wait nobody sits through, and the whole point of the shot is that it
    // finishes.
    if (race.phase == Phase::Finished) {
        ++race.after_ticks;
        race.cam_mode = static_cast<uint8_t>(
            (race.after_ticks / k_cam_cut_ticks) % k_cam_modes);
        bool all_in = true;
        for (int i = 0; i < k_rival_count; ++i)
            if (race.rivals[i].finish_tick == 0) all_in = false;
        if (all_in || race.after_ticks >= k_finish_ticks) race.done = true;
    }

    // ---- the engine note ---------------------------------------------------------
    // A level, not an event: the one sound in this game that never stops. Off
    // the speed rather than the throttle, so it falls away when the pod is
    // coasting and rises again under power, and lifted by the boost so the
    // overdrive is audible as well as visible.
    {
        const int32_t top = pod_top_speed(pod);
        int32_t rev = top > 0 ? pod_speed(pod) * 200 / top : 0;
        if (in.throttle) rev += 30;
        if (pod.boost_ticks > 0) rev += 40;
        if (engine_dead(pod, 0) || engine_dead(pod, 1)) rev = rev * 600 / 1000;
        race.ev.rev = static_cast<uint8_t>(rev < 0 ? 0 : (rev > 255 ? 255 : rev));
        race.ev.grinding = pod.scraping;
    }
}

}  // namespace twinflare
