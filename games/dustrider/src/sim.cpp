#include "sim.hpp"

namespace dr {
namespace {

inline uint32_t next_rand(World& world) {
    uint32_t x = world.rng;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    world.rng = x;
    return x;
}

inline int32_t clamp32(int32_t v, int32_t lo, int32_t hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

// Ring slot for an absolute chunk index, clamped into the generated span so
// a stale query returns the nearest real chunk instead of garbage.
int32_t clamp_index(const World& world, int32_t index) {
    const int32_t high = world.gen_next - 1;
    const int32_t low = world.gen_next - k_track_chunks;
    return clamp32(index, low < 0 ? 0 : low, high);
}

const Chunk& chunk_ref(const World& world, int32_t index) {
    return world.chunks[clamp_index(world, index) & (k_track_chunks - 1)];
}

// Pick the next stretch of road: a straight, or a decisive bend one way.
// Alternating the two is what gives the road its plateau, transition,
// plateau shape instead of one long wobble.
void next_feature(World& world) {
    const uint32_t r = next_rand(world);
    if (world.gen_bending) {
        world.gen_bending = false;
        world.gen_curve_target = 0;
        world.gen_feat_left = 6 + static_cast<int32_t>((r >> 4) % 9);
        return;
    }
    world.gen_bending = true;
    const int32_t size =
        60 + static_cast<int32_t>((r >> 8) % (k_curve_max - 60));
    // Bend away from the edges of the band the road is allowed to wander
    // in, otherwise pick a side.
    int32_t sign = (r & 1) ? 1 : -1;
    if (world.gen_c > k_center_limit - 1536) sign = -1;
    if (world.gen_c < -k_center_limit + 1536) sign = 1;
    world.gen_curve_target = size * sign;
    world.gen_feat_left = 3 + static_cast<int32_t>((r >> 20) % 5);
}

// Lay down one more chunk of world. The centerline first (the chunk records
// the centerline at its own start), then the feature state machines.
void generate_chunk(World& world) {
    Chunk& chunk = world.chunks[world.gen_next & (k_track_chunks - 1)];
    chunk.c = static_cast<int16_t>(world.gen_c);
    chunk.flags = 0;
    chunk.cactus_x = 0;
    chunk.cactus_z = 0;

    const bool calm = world.gen_next < k_calm_chunks || world.gen_straight;
    if (!calm) {
        if (world.gen_feat_left <= 0) next_feature(world);
        world.gen_feat_left--;

        world.gen_curve += clamp32(world.gen_curve_target - world.gen_curve,
                                   -k_curve_ease, k_curve_ease);

        // Guardrails: alternate gap and run.
        if (world.gen_rail_left > 0) {
            chunk.flags |= k_flag_rail;
            world.gen_rail_left--;
            if (world.gen_rail_left == 0) {
                world.gen_rail_after = k_rail_clear_chunks;
                world.gen_rail_gap =
                    k_rail_gap_min +
                    static_cast<int32_t>(next_rand(world) % k_rail_gap_span);
            }
        } else {
            world.gen_rail_gap--;
            if (world.gen_rail_gap <= 0) {
                world.gen_rail_left =
                    k_rail_run_min +
                    static_cast<int32_t>(next_rand(world) % k_rail_run_span);
            }
        }

        // Cactus: on the north shoulder, never on the tarmac, and never
        // behind a rail where the bike could not reach it. gen_rail_after
        // is read before it decays so the full clear count of chunks
        // actually passes after a run ends.
        if (world.gen_cactus_gap > 0) world.gen_cactus_gap--;
        const bool rail_zone = (chunk.flags & k_flag_rail) != 0 ||
                               world.gen_rail_after > 0 ||
                               world.gen_rail_gap <= k_rail_clear_chunks;
        if ((chunk.flags & k_flag_rail) == 0 && world.gen_rail_after > 0) {
            world.gen_rail_after--;
        }
        if (!rail_zone && world.gen_cactus_gap == 0) {
            const int32_t meters = world.gen_next * 2;
            const int32_t chance =
                clamp32(k_cactus_base_256 + meters / k_cactus_ramp_per_m,
                        0, k_cactus_max_256);
            const uint32_t r = next_rand(world);
            if (static_cast<int32_t>(r & 255) < chance) {
                chunk.flags |= k_flag_cactus;
                chunk.cactus_x = static_cast<uint8_t>((r >> 16) & 127);
                chunk.cactus_z = static_cast<uint8_t>(
                    ((r >> 24) % (k_cactus_off_span / 4)));
                world.gen_cactus_gap = k_cactus_min_gap;
            }
        }
    }

    world.gen_c += world.gen_curve;
    world.gen_c = clamp32(world.gen_c, -k_center_limit, k_center_limit);
    world.gen_next++;
}

// Keep the ring stocked comfortably past everything the camera can see.
void ensure_generated(World& world, int32_t up_to_x) {
    const int32_t need = ((up_to_x + (24 << 8)) >> k_chunk_shift) + 1;
    while (world.gen_next <= need) generate_chunk(world);
}

// A cactus's world position from its chunk.
inline int32_t cactus_x_of(int32_t index, const Chunk& chunk) {
    return (index << k_chunk_shift) + chunk.cactus_x * 4;
}

inline int32_t cactus_z_of(const World& world, int32_t cactus_x,
                           const Chunk& chunk) {
    return track_center_z(world, cactus_x) + k_cactus_off_min +
           chunk.cactus_z * 4;
}

void die(World& world, Death cause) {
    if (!world.alive) return;
    world.alive = false;
    world.death = cause;
    world.ev.died = true;
    const uint32_t meters = static_cast<uint32_t>(distance_m(world));
    if (meters > world.best_m) {
        world.best_m = meters;
        world.save_pending = true;
    }
}

}  // namespace

void world_init(World& world, uint32_t seed) {
    world = World{};
    world.rng = seed | 1u;
    world.alive = true;
    world.gen_rail_gap =
        k_rail_gap_min + static_cast<int32_t>(next_rand(world) % k_rail_gap_span);
    ensure_generated(world, 12 << 8);
}

void world_tick(World& world, const Input& input) {
    world.ev = Events{};
    if (!world.alive) return;
    world.tick++;

    ensure_generated(world, (world.x > world.screen_x ? world.x
                                                      : world.screen_x));

    if (input.throttle && !world.started) {
        world.started = true;
        world.start_tick = world.tick;
    }
    world.throttling = input.throttle;

    // ---- steering ----
    // z is absolute, so a bend that the rider does not follow carries the
    // road out from under the bike. The dunes stop the bike a few meters
    // out either side, measured from the centerline so the shoulder is the
    // same width wherever the road has wandered to.
    if (input.north) world.z += k_steer_rate;
    if (input.south) world.z -= k_steer_rate;
    const int32_t center = track_center_z(world, world.x);
    world.z = clamp32(world.z, center - k_offroad_max, center + k_offroad_max);
    const int32_t offset = world.z - center;

    // ---- the guardrail, which turns the north edge into a wall ----
    if (offset > k_road_half && track_rail_at(world, world.x)) {
        die(world, Death::Rail);
        return;
    }

    // ---- forward speed ----
    if (input.throttle) world.v += k_bike_accel;
    if (input.brake) {
        world.v -= (world.v >> k_brake_shift) + k_brake_base;
    }
    world.v -= world.v >> k_drag_shift;
    if (offset > k_road_half || offset < -k_road_half) {
        world.v -= world.v >> k_sand_drag_shift;   // sand pays extra
    }
    world.v = clamp32(world.v, 0, 32000);

    // ---- integrate x, keeping the fractional fp8 bits ----
    const int32_t x_step = world.v + world.x_frac;
    world.x += x_step >> 8;
    world.x_frac = static_cast<uint8_t>(x_step & 255);

    // ---- cactus collision ----
    const int32_t first = (world.x - k_cactus_half) >> k_chunk_shift;
    const int32_t last = (world.x + k_cactus_half) >> k_chunk_shift;
    for (int32_t index = first; index <= last; index++) {
        if (index < 0) continue;
        const Chunk& chunk = chunk_ref(world, index);
        if (!(chunk.flags & k_flag_cactus)) continue;
        const int32_t cx = cactus_x_of(index, chunk);
        int32_t dx = world.x - cx;
        if (dx < 0) dx = -dx;
        if (dx > k_cactus_half) continue;
        int32_t dz = world.z - cactus_z_of(world, cx, chunk);
        if (dz < 0) dz = -dz;
        if (dz >= k_cactus_z_reach) continue;
        die(world, Death::Cactus);
        return;
    }

    // ---- the window ----
    const bool released =
        world.started && world.tick - world.start_tick > k_start_grace;
    if (!released) {
        // Until the chase begins the window rides glued to the bike, and it
        // releases at the bike's own speed so the hand off cannot fling an
        // edge at the rider.
        world.screen_x = world.x;
        world.screen_frac = world.x_frac;
        world.screen_v = world.v;
    } else {
        int32_t target = k_screen_v0 +
                         (world.screen_x >> 8) * k_screen_ramp_per_m;
        if (target > k_screen_vmax) target = k_screen_vmax;
        world.screen_v += clamp32(target - world.screen_v,
                                  -k_screen_accel, k_screen_accel);
        const int32_t step = world.screen_v + world.screen_frac;
        world.screen_x += step >> 8;
        world.screen_frac = static_cast<uint8_t>(step & 255);

        const int32_t rel = world.x - world.screen_x;
        if (rel < -k_window_half) {
            die(world, Death::Behind);
            return;
        }
        if (rel > k_window_half) {
            die(world, Death::Ahead);
            return;
        }
    }
}

bool world_load(World& world, const SaveData& data) {
    if (data.magic != k_save_magic) return false;
    world.best_m = data.best_m;
    return true;
}

void world_make_save(const World& world, SaveData& out) {
    out = SaveData{};
    out.magic = k_save_magic;
    out.best_m = world.best_m;
}

int32_t track_center_z(const World& world, int32_t x) {
    if (x < 0) x = 0;
    const int32_t index = x >> k_chunk_shift;
    const int32_t c0 = chunk_ref(world, index).c;
    const int32_t c1 = chunk_ref(world, index + 1).c;
    const int32_t frac = x & (k_chunk_len - 1);
    return c0 + ((c1 - c0) * frac >> k_chunk_shift);
}

bool track_rail_at(const World& world, int32_t x) {
    if (x < 0) return false;
    return (chunk_ref(world, x >> k_chunk_shift).flags & k_flag_rail) != 0;
}

const Chunk& track_chunk_at(const World& world, int32_t x) {
    return chunk_ref(world, x < 0 ? 0 : x >> k_chunk_shift);
}

bool track_next_cactus(const World& world, int32_t from_x, int32_t max_ahead,
                       int32_t& out_x, int32_t& out_z) {
    const int32_t first = (from_x < 0 ? 0 : from_x) >> k_chunk_shift;
    const int32_t last = (from_x + max_ahead) >> k_chunk_shift;
    for (int32_t index = first; index <= last; index++) {
        if (index >= world.gen_next) break;
        const Chunk& chunk = chunk_ref(world, index);
        if (!(chunk.flags & k_flag_cactus)) continue;
        const int32_t cx = cactus_x_of(index, chunk);
        if (cx <= from_x || cx > from_x + max_ahead) continue;
        out_x = cx;
        out_z = cactus_z_of(world, cx, chunk);
        return true;
    }
    return false;
}

void world_test_place_cactus(World& world, int32_t x, int32_t z_offset) {
    Chunk& chunk = const_cast<Chunk&>(track_chunk_at(world, x));
    chunk.flags |= k_flag_cactus;
    chunk.cactus_x = static_cast<uint8_t>((x & (k_chunk_len - 1)) / 4);
    chunk.cactus_z = static_cast<uint8_t>(
        clamp32((z_offset - k_cactus_off_min) / 4, 0,
                k_cactus_off_span / 4 - 1));
}

void world_test_set_rail(World& world, int32_t x, bool rail) {
    Chunk& chunk = const_cast<Chunk&>(track_chunk_at(world, x));
    if (rail) {
        chunk.flags |= k_flag_rail;
    } else {
        chunk.flags &= static_cast<uint8_t>(~k_flag_rail);
    }
}

void world_test_clear_hazards(World& world) {
    for (Chunk& chunk : world.chunks) {
        chunk.flags = 0;
    }
}

void world_test_straight(World& world, bool straight) {
    world.gen_straight = straight;
    if (!straight) return;
    // Flatten what is already in the ring as well as what comes next.
    // world_init has generated a first stretch by the time a test can call
    // this, and leaving that stretch bent would quietly start the bike in
    // the sand, which reads as a physics bug rather than a setup one.
    world.gen_curve = 0;
    world.gen_curve_target = 0;
    world.gen_c = 0;
    for (Chunk& chunk : world.chunks) {
        chunk.c = 0;
    }
}

Chunk world_test_generate_chunk(World& world) {
    generate_chunk(world);
    return world.chunks[(world.gen_next - 1) & (k_track_chunks - 1)];
}

}  // namespace dr
