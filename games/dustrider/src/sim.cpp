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

// Lay down one more chunk of world. Height first (the chunk records the
// height at its own start), then the feature state machines advance.
void generate_chunk(World& world) {
    Chunk& chunk = world.chunks[world.gen_next & (k_track_chunks - 1)];
    chunk.h = static_cast<int16_t>(world.gen_h);
    chunk.flags = 0;
    chunk.cactus_off = 0;

    const bool calm = world.gen_next < k_calm_chunks || world.gen_flat;
    if (!calm) {
        // Terrain: ease the slope toward its target; pick a new feature
        // when the current one runs out. Drops are drawn slightly steeper
        // than climbs so the horizon keeps coming back down to meet you.
        if (world.gen_feat_left <= 0) {
            const uint32_t r = next_rand(world);
            const int kind = static_cast<int>(r & 3);
            if (kind == 0) {
                world.gen_slope_target = 0;
            } else {
                const int32_t size =
                    20 + static_cast<int32_t>((r >> 8) % (k_slope_max - 20));
                world.gen_slope_target = (kind == 1) ? size : -size;
            }
            world.gen_feat_left =
                3 + static_cast<int32_t>((r >> 20) % 7);
        }
        world.gen_feat_left--;

        // Keep the world inside the height band by bending the target back
        // toward zero altitude before the clamp ever engages.
        if (world.gen_h > k_height_limit - 1024 && world.gen_slope_target > 0) {
            world.gen_slope_target = -world.gen_slope_target;
        }
        if (world.gen_h < -k_height_limit + 1024 && world.gen_slope_target < 0) {
            world.gen_slope_target = -world.gen_slope_target;
        }

        world.gen_slope += clamp32(world.gen_slope_target - world.gen_slope,
                                   -k_slope_ease, k_slope_ease);

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

        // Cactus: only where it can be dodged. Never in or near a rail run
        // (the rail must stay a lane lock, not a trap) and never closer to
        // the previous cactus than a lane change costs at top speed.
        // gen_rail_after is read before it decays so the full clear count
        // of chunks actually passes after a run ends.
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
                if (r & 256) chunk.flags |= k_flag_cactus_sand;
                chunk.cactus_off = static_cast<uint8_t>((r >> 16) & 127);
                world.gen_cactus_gap = k_cactus_min_gap;
            }
        }
    }

    world.gen_h += world.gen_slope * 2;   // slope * chunk length / 256
    world.gen_h = clamp32(world.gen_h, -k_height_limit, k_height_limit);
    world.gen_next++;
}

// Keep the ring stocked comfortably past everything the camera can see.
void ensure_generated(World& world, int32_t up_to_x) {
    const int32_t need = ((up_to_x + (24 << 8)) >> k_chunk_shift) + 1;
    while (world.gen_next <= need) generate_chunk(world);
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
    world.grounded = true;
    world.lane_z = k_lane_road_z;
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

    // ---- lane intent ----
    if (input.to_road) world.lane_z = k_lane_road_z;
    if (input.to_sand) world.lane_z = k_lane_sand_z;

    if (input.throttle && !world.started) {
        world.started = true;
        world.start_tick = world.tick;
    }
    world.throttling = input.throttle && world.grounded;

    // ---- forward speed ----
    const int32_t slope_here = track_slope(world, world.x);
    if (world.grounded) {
        if (input.throttle) world.v += k_bike_accel;
        if (input.brake) {
            world.v -= (world.v >> k_brake_shift) + k_brake_base;
        }
        world.v -= world.v >> k_drag_shift;
        if (world.z > k_road_edge_z) {
            world.v -= world.v >> k_sand_drag_shift;   // sand pays extra
        }
        world.v -= (slope_here * k_slope_pull) >> 8;   // gravity along the hill
    } else {
        world.v -= world.v >> k_air_drag_shift;
    }
    world.v = clamp32(world.v, 0, 32000);

    // ---- integrate x, keeping the fractional fp8 bits ----
    const int32_t x_step = world.v + world.x_frac;
    world.x += x_step >> 8;
    world.x_frac = static_cast<uint8_t>(x_step & 255);

    // ---- vertical: hug the ground until it falls away faster than
    // gravity, then fly ballistic until it comes back ----
    const int32_t ground16 = track_height(world, world.x) << 8;
    if (world.grounded) {
        const int32_t needed_vy = ground16 - world.y16;
        const int32_t falling = world.vy - k_gravity;
        if (needed_vy < falling) {
            world.grounded = false;
            world.vy = falling;
            world.y16 += world.vy;
            world.ev.takeoff = true;
        } else {
            world.y16 = ground16;
            world.vy = needed_vy;
        }
    } else {
        world.vy -= k_gravity;
        world.y16 += world.vy;
        if (world.y16 <= ground16) {
            world.y16 = ground16;
            world.grounded = true;
            world.ev.landed = true;
            // vy is NOT zeroed: on a downhill the wheels keep the terrain's
            // own descent rate, and next tick's grounded branch reads vy as
            // "last tick's dy". Zeroing it here made every landing on a
            // slope bounce straight back into phantom airtime.
        }
    }

    // ---- lane movement, and the rail that punishes crossing it ----
    const int32_t z_old = world.z;
    world.z += clamp32(world.lane_z - world.z, -k_lane_rate, k_lane_rate);
    const bool was_road_side = z_old <= k_road_edge_z;
    const bool is_road_side = world.z <= k_road_edge_z;
    if (was_road_side != is_road_side && track_rail_at(world, world.x) &&
        (world.y16 >> 8) - track_height(world, world.x) < k_rail_top) {
        die(world, Death::Rail);
        return;
    }

    // ---- cactus collision, jumpable by clearing its top ----
    const int32_t first = (world.x - k_cactus_half) >> k_chunk_shift;
    const int32_t last = (world.x + k_cactus_half) >> k_chunk_shift;
    for (int32_t index = first; index <= last; index++) {
        const Chunk& chunk = chunk_ref(world, index);
        if (!(chunk.flags & k_flag_cactus)) continue;
        const int32_t cactus_x =
            (index << k_chunk_shift) + chunk.cactus_off * 4;
        int32_t dx = world.x - cactus_x;
        if (dx < 0) dx = -dx;
        if (dx > k_cactus_half) continue;
        const int32_t lane_center = (chunk.flags & k_flag_cactus_sand)
                                        ? k_lane_sand_z : k_lane_road_z;
        int32_t dz = world.z - lane_center;
        if (dz < 0) dz = -dz;
        if (dz >= k_cactus_z_reach) continue;
        if ((world.y16 >> 8) - track_height(world, cactus_x) >= k_cactus_top) {
            continue;   // sailed clean over it
        }
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

int32_t track_height(const World& world, int32_t x) {
    if (x < 0) x = 0;
    const int32_t index = x >> k_chunk_shift;
    const int32_t h0 = chunk_ref(world, index).h;
    const int32_t h1 = chunk_ref(world, index + 1).h;
    const int32_t frac = x & (k_chunk_len - 1);
    return h0 + ((h1 - h0) * frac >> k_chunk_shift);
}

int32_t track_slope(const World& world, int32_t x) {
    if (x < 0) x = 0;
    const int32_t index = x >> k_chunk_shift;
    const int32_t h0 = chunk_ref(world, index).h;
    const int32_t h1 = chunk_ref(world, index + 1).h;
    return (h1 - h0) * 256 / k_chunk_len;
}

bool track_rail_at(const World& world, int32_t x) {
    if (x < 0) return false;
    return (chunk_ref(world, x >> k_chunk_shift).flags & k_flag_rail) != 0;
}

const Chunk& track_chunk_at(const World& world, int32_t x) {
    return chunk_ref(world, x < 0 ? 0 : x >> k_chunk_shift);
}

bool track_next_cactus(const World& world, int32_t from_x, int32_t max_ahead,
                       int32_t& out_x, bool& out_sand) {
    const int32_t first = (from_x < 0 ? 0 : from_x) >> k_chunk_shift;
    const int32_t last = (from_x + max_ahead) >> k_chunk_shift;
    for (int32_t index = first; index <= last; index++) {
        if (index >= world.gen_next) break;
        const Chunk& chunk = chunk_ref(world, index);
        if (!(chunk.flags & k_flag_cactus)) continue;
        const int32_t cactus_x =
            (index << k_chunk_shift) + chunk.cactus_off * 4;
        if (cactus_x <= from_x || cactus_x > from_x + max_ahead) continue;
        out_x = cactus_x;
        out_sand = (chunk.flags & k_flag_cactus_sand) != 0;
        return true;
    }
    return false;
}

void world_test_place_cactus(World& world, int32_t x, bool sand_lane) {
    Chunk& chunk = const_cast<Chunk&>(track_chunk_at(world, x));
    chunk.flags |= k_flag_cactus;
    if (sand_lane) {
        chunk.flags |= k_flag_cactus_sand;
    } else {
        chunk.flags &= static_cast<uint8_t>(~k_flag_cactus_sand);
    }
    chunk.cactus_off =
        static_cast<uint8_t>((x & (k_chunk_len - 1)) / 4);
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

void world_test_flat(World& world, bool flat) {
    world.gen_flat = flat;
    if (flat) {
        world.gen_slope = 0;
        world.gen_slope_target = 0;
    }
}

Chunk world_test_generate_chunk(World& world) {
    generate_chunk(world);
    return world.chunks[(world.gen_next - 1) & (k_track_chunks - 1)];
}

}  // namespace dr
