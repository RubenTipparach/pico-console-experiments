#include "sim.hpp"

namespace kf {
namespace {

// Deterministic RNG. rand() is not guaranteed to match between the device
// build and the web build, and a pond that differs per platform makes tuning
// and bug reports meaningless.
uint32_t next_random(uint32_t& state) {
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return state;
}

int32_t rnd(uint32_t& state, int32_t n) {
    return static_cast<int32_t>(next_random(state) % static_cast<uint32_t>(n));
}

int32_t clamp32(int32_t v, int32_t lo, int32_t hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

// Depth bands as z ranges from the boat, in fp8 world units. Farther cast
// means deeper water means rarer fish: the power meter is the difficulty dial.
struct Band { int32_t z_min, z_max; int32_t depth_min, depth_max; };
const Band k_bands[3] = {
    {static_cast<int32_t>(2.5 * k_one), 6 * k_one,
     k_one / 4, k_one / 2},
    {6 * k_one, static_cast<int32_t>(10.5 * k_one),
     k_one / 2, 5 * k_one / 4},
    {static_cast<int32_t>(10.5 * k_one), 15 * k_one,
     5 * k_one / 4, 2 * k_one},
};

int band_for_z(int32_t z) {
    if (z < k_bands[1].z_min) return 0;
    if (z < k_bands[2].z_min) return 1;
    return 2;
}

constexpr int32_t k_lake_half_width = 8 * k_one;
constexpr int32_t k_boat_z = 0;

// Fight tuning. All per tick (10 ms). The host tests assert the consequences:
// patient reeling lands everything, greedy reeling snaps on strong fish.
constexpr int k_reel_tire = 14;      // fp8 line per tick reeling a tired fish
constexpr int k_reel_run = 5;        // reeling against a running fish
constexpr int32_t k_catch_len = 340; // ~1.3 units from the boat lands it

}  // namespace

const Species k_species[k_species_count] = {
    // name        band  time            rarity str size      points colour
    {"MINNOW",     0, k_day | k_night,   30,  1,   5,  12,     5, 200, 205, 215},
    {"BLUEGILL",   0, k_day,             24,  2,  12,  25,    10,  90, 140, 190},
    {"PERCH",      0, k_day,             20,  3,  15,  30,    15, 150, 160,  90},
    {"GHOST KOI",  0, k_night,            6,  4,  30,  60,    60, 235, 235, 240},
    {"BASS",       1, k_day,             16,  5,  25,  55,    25,  90, 150,  80},
    {"CARP",       1, k_day | k_night,   14,  4,  30,  70,    20, 170, 130,  70},
    {"GOLD CARP",  1, k_day,              4,  5,  35,  75,    90, 230, 180,  60},
    {"PIKE",       1, k_day | k_night,   10,  7,  40,  90,    40,  60, 110,  70},
    {"MOONFISH",   1, k_night,            5,  6,  30,  60,    80, 170, 190, 230},
    {"CATFISH",    2, k_night,            8,  8,  50, 110,    50, 120, 100,  90},
    {"STURGEON",   2, k_day | k_night,    4,  9,  80, 160,   110, 130, 135, 145},
    {"THE OLD ONE",2, k_night,            1, 10, 120, 200,   250,  60,  80,  70},
};

uint8_t day_phase(const World& world) {
    return static_cast<uint8_t>((world.day_tick * 256u) / k_day_length);
}

bool is_night(const World& world) {
    return world.day_tick >= k_day_length / 2;
}

void world_init(World& world, uint32_t seed) {
    world = World{};
    world.rng = seed ? seed : 0xF150001u;   // seed 0 would jam xorshift
    world.mode = Mode::Idle;
    world.hooked_fish = -1;
    world.card_species = -1;
    world.day_tick = k_day_length / 8;   // start mid morning
    world.weather_timer = 6000;
    for (auto& fish : world.fish) fish.state = FishState::Gone;
    for (auto& best : world.records.best_cm) best = 0;
}

bool world_load(World& world, const SaveData& data) {
    if (data.magic != k_save_magic) return false;
    world.records = data.records;
    return true;
}

void world_make_save(const World& world, SaveData& out) {
    out.magic = k_save_magic;
    out.records = world.records;
}

namespace {

void reset_lure(World& world) {
    world.mode = Mode::Idle;
    world.bite_timer = 0;
    world.lure_x = 0;
    world.lure_y = 0;
    world.lure_z = k_one;
    world.lure_vx = world.lure_vy = world.lure_vz = 0;
    world.twitch_timer = 0;
    world.hooked_fish = -1;
}

void fish_flee(Fish& fish) {
    if (fish.state == FishState::Gone) return;
    fish.state = FishState::Flee;
    fish.timer = 0;
    fish.tx = fish.x >= 0 ? k_lake_half_width + 2 * k_one
                          : -(k_lake_half_width + 2 * k_one);
    fish.tz = fish.z;
}

// Weighted species pick among those allowed right now, or -1.
int pick_species(World& world) {
    const bool night = is_night(world);
    int total = 0;
    for (const auto& s : k_species) {
        if (s.time_mask & (night ? k_night : k_day)) total += s.rarity;
    }
    if (total == 0) return -1;
    int roll = rnd(world.rng, total);
    for (int i = 0; i < k_species_count; i++) {
        const auto& s = k_species[i];
        if (!(s.time_mask & (night ? k_night : k_day))) continue;
        roll -= s.rarity;
        if (roll < 0) return i;
    }
    return -1;
}

void spawn_fish(World& world) {
    for (auto& fish : world.fish) {
        if (fish.state != FishState::Gone) continue;

        const int species = pick_species(world);
        if (species < 0) return;
        const Species& s = k_species[species];
        const Band& band = k_bands[s.band];

        fish.species = static_cast<uint8_t>(species);
        fish.state = FishState::Wander;
        // Square the roll so small fish are common and big ones are an event.
        const int range = s.size_max - s.size_min;
        const int r1 = rnd(world.rng, 256);
        fish.size_cm = static_cast<int16_t>(
            s.size_min + (range * r1 * r1) / (255 * 255));

        fish.x = rnd(world.rng, 2) ? k_lake_half_width : -k_lake_half_width;
        fish.z = band.z_min + rnd(world.rng, band.z_max - band.z_min);
        fish.y = band.depth_min +
                 rnd(world.rng, band.depth_max - band.depth_min);
        fish.vx = 0;
        fish.vz = 0;
        fish.tx = rnd(world.rng, 2 * k_lake_half_width) - k_lake_half_width;
        fish.tz = fish.z;
        fish.timer = static_cast<uint16_t>(200 + rnd(world.rng, 300));
        fish.nibbles_left = 0;
        return;
    }
}

// Move toward the target one axis step at a time. No square roots: the drift
// this produces reads as fish meandering, which is what a pond looks like.
void steer(Fish& fish, int32_t speed) {
    const int32_t dx = fish.tx - fish.x;
    const int32_t dz = fish.tz - fish.z;
    int32_t vx = clamp32(dx / 24, -speed, speed);
    int32_t vz = clamp32(dz / 24, -speed, speed);
    // Truncating division stalls 23 fp short of the target, which strands a
    // fleeing fish just before its despawn point forever and parks a curious
    // fish fractionally out of nibbling range. Always finish the approach.
    if (vx == 0 && dx != 0) vx = dx > 0 ? 1 : -1;
    if (vz == 0 && dz != 0) vz = dz > 0 ? 1 : -1;
    fish.vx = vx;
    fish.vz = vz;
    fish.x += vx;
    fish.z += vz;
}

bool lure_in_water(const World& world) {
    return world.mode == Mode::Sinking;
}

// Whole-unit squared distance from this fish to the lure. Max ~500, no
// overflow anywhere near.
int32_t lure_dist2_units(const World& world, const Fish& fish) {
    const int32_t dx = (world.lure_x - fish.x) >> k_fp;
    const int32_t dz = (world.lure_z - fish.z) >> k_fp;
    return dx * dx + dz * dz;
}

void update_fish(World& world, Fish& fish, int index) {
    const Species& s = k_species[fish.species];
    const int32_t speed = 3 + s.strength / 2;

    switch (fish.state) {
        case FishState::Gone:
            return;

        case FishState::Wander: {
            steer(fish, speed);
            if (fish.timer > 0) fish.timer--;
            if (fish.timer == 0) {
                // A fish whose time of day has passed drifts away; otherwise
                // pick a new spot in its band.
                const bool night = is_night(world);
                if (!(s.time_mask & (night ? k_night : k_day))) {
                    fish_flee(fish);
                    break;
                }
                const Band& band = k_bands[s.band];
                fish.tx = rnd(world.rng, 2 * k_lake_half_width) -
                          k_lake_half_width;
                fish.tz = band.z_min +
                          rnd(world.rng, band.z_max - band.z_min);
                fish.timer = static_cast<uint16_t>(200 + rnd(world.rng, 300));
            }

            if (!lure_in_water(world)) break;
            if (world.hooked_fish >= 0) break;
            if (band_for_z(world.lure_z) != s.band) break;
            if (lure_dist2_units(world, fish) > 16) break;

            // Interest roll every 16 ticks. Twitching the lure and rain both
            // help; rare fish are pickier.
            if ((world.tick & 15) == 0) {
                int chance = 26 + (world.twitch_timer ? 22 : 0) +
                             (world.raining ? 16 : 0) - s.rarity / 2;
                if (fish.species == k_species_count - 1 && !world.raining) {
                    chance = 2;   // the legend waits for rain
                }
                if (rnd(world.rng, 256) < chance) {
                    fish.state = FishState::Curious;
                }
            }
            break;
        }

        case FishState::Curious: {
            fish.tx = world.lure_x;
            fish.tz = world.lure_z;
            steer(fish, speed + 2);
            if (!lure_in_water(world) || world.hooked_fish >= 0) {
                fish.state = FishState::Wander;
                fish.timer = 100;
                break;
            }
            const int32_t adx = world.lure_x > fish.x ? world.lure_x - fish.x
                                                       : fish.x - world.lure_x;
            const int32_t adz = world.lure_z > fish.z ? world.lure_z - fish.z
                                                       : fish.z - world.lure_z;
            if (adx + adz <= 320) {
                fish.state = FishState::Nibbling;
                fish.nibbles_left =
                    static_cast<uint8_t>(2 + rnd(world.rng, 3));
                fish.timer = 20;
            }
            break;
        }

        case FishState::Nibbling: {
            if (!lure_in_water(world)) {
                fish_flee(fish);
                break;
            }
            if (fish.timer > 0) {
                fish.timer--;
                break;
            }
            if (fish.nibbles_left > 0) {
                fish.nibbles_left--;
                world.ev.nibble = true;
                fish.timer = static_cast<uint16_t>(24 + rnd(world.rng, 20));
                break;
            }
            // Commit or lose interest. Only one fish may hold the bite
            // window: a second hopeful keeps mouthing the lure until the
            // window is free.
            {
                bool window_taken = false;
                for (const auto& other : world.fish) {
                    if (&other != &fish &&
                        other.state == FishState::Biting) {
                        window_taken = true;
                    }
                }
                if (window_taken) {
                    fish.timer = static_cast<uint16_t>(
                        30 + rnd(world.rng, 30));
                    break;
                }
            }
            if (rnd(world.rng, 256) < 96 + s.strength * 10) {
                fish.state = FishState::Biting;
                world.bite_timer = 50;
                world.ev.bite = true;
            } else {
                fish_flee(fish);
            }
            break;
        }

        case FishState::Biting: {
            // The window is decremented in the mode logic so exactly one fish
            // can hold it. Nothing to do here but hold position.
            break;
        }

        case FishState::Hooked: {
            // Constrain the fish to the line. Manhattan approximation: real
            // radial distance is presentation detail, and this cannot
            // overflow or need a square root.
            const int32_t line_units = world.line_len >> k_fp;
            const int32_t dx = fish.x >> k_fp;
            const int32_t dz = (fish.z - k_boat_z) >> k_fp;
            const int32_t manhattan = (dx < 0 ? -dx : dx) + dz;
            if (manhattan > line_units + 1) {
                fish.x -= fish.x / 8;
                fish.z -= (fish.z - k_boat_z) / 8;
            }
            if (world.fight_phase == FightPhase::Run) {
                // Dart away from the boat with a side jitter.
                fish.z += speed;
                fish.x += (rnd(world.rng, 2) ? 1 : -1) *
                          rnd(world.rng, speed + 1);
            } else {
                fish.x += (fish.x > 0 ? -1 : 1) * (speed / 2);
            }
            fish.z = clamp32(fish.z, 2 * k_one, 16 * k_one);
            fish.x = clamp32(fish.x, -k_lake_half_width, k_lake_half_width);
            break;
        }

        case FishState::Flee: {
            steer(fish, speed * 2);
            const int32_t dx = fish.tx - fish.x;
            if (dx > -64 && dx < 64) fish.state = FishState::Gone;
            break;
        }
    }
    (void)index;
}

void start_fight(World& world, int fish_index) {
    Fish& fish = world.fish[fish_index];
    const Species& s = k_species[fish.species];
    fish.state = FishState::Hooked;
    world.mode = Mode::Fight;
    world.hooked_fish = static_cast<int8_t>(fish_index);
    world.tension = 250;
    world.stamina = static_cast<uint16_t>(200 + fish.size_cm * 4 +
                                          s.strength * 60);
    world.line_len = world.lure_z + (world.lure_x < 0 ? -world.lure_x
                                                      : world.lure_x);
    world.line_max = world.line_len + world.line_len / 2 + 3 * k_one;
    world.fight_phase = FightPhase::Run;
    world.phase_timer = static_cast<uint16_t>(50 + rnd(world.rng,
                                                       50 + s.strength * 8));
    world.leap_timer = 0;
    world.bite_timer = 0;
    world.ev.hooked = true;
    // Everyone else scatters.
    for (int i = 0; i < k_max_fish; i++) {
        if (i != fish_index) fish_flee(world.fish[i]);
    }
}

void end_fight(World& world, bool caught) {
    if (world.hooked_fish >= 0) {
        Fish& fish = world.fish[world.hooked_fish];
        if (caught) {
            const Species& s = k_species[fish.species];
            Records& rec = world.records;
            rec.caught[fish.species]++;
            // Points scale from 1x at minimum size to 2x at maximum.
            const int range = s.size_max - s.size_min;
            const int over = fish.size_cm - s.size_min;
            rec.score += s.points + (range > 0 ?
                (static_cast<uint32_t>(s.points) * over) / range : 0);
            world.card_species = static_cast<int8_t>(fish.species);
            world.card_size = fish.size_cm;
            world.card_record = fish.size_cm > rec.best_cm[fish.species];
            if (world.card_record) {
                rec.best_cm[fish.species] = fish.size_cm;
                world.ev.new_record = true;
            }
            world.card_timer = 220;
            world.save_pending = true;
            world.ev.caught = true;
            fish.state = FishState::Gone;
        } else {
            fish_flee(world.fish[world.hooked_fish]);
        }
    }
    reset_lure(world);
    if (caught) world.mode = Mode::Landed;
}

void update_fight(World& world, const Input& input) {
    const Fish& fish = world.fish[world.hooked_fish];
    const Species& s = k_species[fish.species];
    const bool reeling = input.a;
    const bool exhausted = world.stamina == 0;

    // Tension. The numbers here are the game: patient reeling in Tire is
    // safe, reeling against a Run is how lines snap.
    int delta;
    if (world.fight_phase == FightPhase::Run) {
        delta = reeling ? 2 + s.strength / 2 : (s.strength >= 6 ? 1 : 0);
        if (exhausted) delta = reeling ? 1 : 0;
    } else {
        delta = reeling ? -2 : -4;
    }
    const int tension = static_cast<int>(world.tension) + delta;
    world.tension = static_cast<uint16_t>(clamp32(tension, 0, 1023));

    if (world.tension >= k_tension_snap) {
        world.ev.snap = true;
        end_fight(world, false);
        return;
    }

    // Line length.
    if (world.fight_phase == FightPhase::Run && !reeling && !exhausted) {
        world.line_len += s.strength;
        if (world.line_len >= world.line_max) {
            world.ev.escape = true;
            end_fight(world, false);
            return;
        }
    }
    if (reeling) {
        world.line_len -= (world.fight_phase == FightPhase::Tire || exhausted)
                              ? k_reel_tire : k_reel_run;
        if (world.stamina > 0) {
            world.stamina -= (world.fight_phase == FightPhase::Tire) ? 3 : 1;
            if (static_cast<int16_t>(world.stamina) < 0) world.stamina = 0;
        }
    }

    if (world.line_len <= k_catch_len) {
        end_fight(world, true);
        return;
    }

    // Phase changes. An exhausted fish never runs again, which is the reward
    // for wearing it down instead of muscling it.
    if (world.phase_timer > 0) world.phase_timer--;
    if (world.phase_timer == 0) {
        if (world.fight_phase == FightPhase::Run || exhausted) {
            world.fight_phase = FightPhase::Tire;
            world.phase_timer = static_cast<uint16_t>(
                70 + rnd(world.rng, 70));
        } else {
            world.fight_phase = FightPhase::Run;
            world.phase_timer = static_cast<uint16_t>(
                50 + rnd(world.rng, 50 + s.strength * 8));
            // A fresh run sometimes breaks the surface.
            if (rnd(world.rng, 256) < 90) {
                world.leap_timer = 45;
                world.ev.leap = true;
            }
        }
    }
    if (world.leap_timer > 0) world.leap_timer--;

    // The lure rides toward the fish so the line and splash read correctly.
    world.lure_x += (fish.x - world.lure_x) / 8;
    world.lure_z += (fish.z - world.lure_z) / 8;
    world.lure_y = k_one / 8;
}

void update_lure(World& world, const Input& input) {
    switch (world.mode) {
        case Mode::Idle:
            if (input.a_pressed) {
                world.mode = Mode::Aiming;
                world.power = 0;
                world.power_dir = 1;
            }
            break;

        case Mode::Aiming: {
            // The meter bounces, so casting is a timing skill and full power
            // takes deliberate timing rather than a long hold.
            int power = world.power + world.power_dir * 4;
            if (power >= 255) { power = 255; world.power_dir = -1; }
            if (power <= 0) { power = 0; world.power_dir = 1; }
            world.power = static_cast<uint8_t>(power);
            if (input.left) world.aim = static_cast<int8_t>(
                clamp32(world.aim - 1, -10, 10));
            if (input.right) world.aim = static_cast<int8_t>(
                clamp32(world.aim + 1, -10, 10));

            if (!input.a && !input.a_released) {
                world.mode = Mode::Idle;   // release edge was lost, stand down
                break;
            }
            if (input.a_released) {
                world.mode = Mode::Flying;
                world.lure_x = 0;
                world.lure_y = -k_one;          // rod tip, above the surface
                world.lure_z = k_one;
                world.lure_vz = 9 + (world.power * 51) / 256;
                world.lure_vx = world.aim * 3;
                world.lure_vy = -20;            // upward (y is down)
                world.ev.cast = true;
            }
            break;
        }

        case Mode::Flying:
            world.lure_vy += 2;                 // gravity, y positive down
            world.lure_x += world.lure_vx;
            world.lure_y += world.lure_vy / 8;
            world.lure_z += world.lure_vz;
            world.lure_x = clamp32(world.lure_x, -k_lake_half_width,
                                   k_lake_half_width);
            world.lure_z = clamp32(world.lure_z, k_one, 15 * k_one);
            if (world.lure_y >= 0) {
                world.lure_y = 0;
                world.lure_vx = world.lure_vy = world.lure_vz = 0;
                world.mode = Mode::Sinking;
                world.ev.splash = true;
            }
            break;

        case Mode::Sinking: {
            // Settle toward the bottom of the local band.
            const Band& band = k_bands[band_for_z(world.lure_z)];
            if (world.lure_y < band.depth_max) world.lure_y += 2;

            if (world.twitch_timer > 0) world.twitch_timer--;
            if (input.left || input.right) {
                world.lure_x += input.left ? -20 : 20;
                world.lure_x = clamp32(world.lure_x, -k_lake_half_width,
                                       k_lake_half_width);
                world.twitch_timer = 180;
            }

            // A biting fish owns the button. Otherwise A retrieves, which is
            // also how you recast: reel the lure home and cast again.
            int biting = -1;
            for (int i = 0; i < k_max_fish; i++) {
                if (world.fish[i].state == FishState::Biting) biting = i;
            }
            if (biting >= 0) {
                // A press hooks. So does already holding A when the window
                // opens: a fish that grabs a lure mid retrieve is struck by
                // the pull the player is already applying, instead of being
                // silently unhookable until a release and re press.
                const bool held_strike = input.a && world.bite_timer >= 48;
                if (input.a_pressed || held_strike) {
                    start_fight(world, biting);
                    break;
                }
                if (world.bite_timer > 0) world.bite_timer--;
                if (world.bite_timer == 0) {
                    // The missed fish bolts with a splash, and that spooks
                    // everything mouthing the lure. Without this a waiting
                    // nibbler could open a fresh window one tick after the
                    // player just watched the strike fail, which reads as
                    // the same fish refusing to leave.
                    for (auto& f : world.fish) {
                        if (f.state == FishState::Biting ||
                            f.state == FishState::Nibbling) {
                            fish_flee(f);
                        }
                    }
                    world.ev.splash = true;
                }
            } else if (input.a) {
                world.lure_z -= 8;
                world.lure_x -= world.lure_x / 32;
                world.twitch_timer = 60;   // a moving lure draws attention
                if (world.lure_z <= k_one) reset_lure(world);
            }
            break;
        }

        case Mode::Fight:
            update_fight(world, input);
            break;

        case Mode::Landed:
            if (world.card_timer > 0) world.card_timer--;
            if (world.card_timer == 0 || input.a_pressed) {
                world.mode = Mode::Idle;
            }
            break;
    }
}

}  // namespace

int world_test_hook(World& world, int species, int size_cm) {
    Fish& fish = world.fish[0];
    fish.species = static_cast<uint8_t>(species);
    fish.state = FishState::Biting;
    fish.size_cm = static_cast<int16_t>(size_cm);
    fish.x = 0;
    fish.z = 8 * k_one;
    fish.y = k_one;
    world.mode = Mode::Sinking;
    world.lure_x = 0;
    world.lure_y = k_one;
    world.lure_z = 8 * k_one;
    world.bite_timer = 50;
    return 0;
}

void world_tick(World& world, const Input& input) {
    world.ev = Events{};
    world.tick++;
    world.day_tick++;
    if (world.day_tick >= k_day_length) world.day_tick = 0;

    // Weather. Rain raises interest, and one resident of the deep only bites
    // in it.
    if (world.weather_timer > 0) world.weather_timer--;
    if (world.weather_timer == 0) {
        if (world.raining) {
            world.raining = 0;
            world.weather_timer = static_cast<uint16_t>(
                4500 + rnd(world.rng, 7500));
        } else if (rnd(world.rng, 256) < 90) {
            world.raining = 1;
            world.weather_timer = static_cast<uint16_t>(
                2500 + rnd(world.rng, 2500));
        } else {
            world.weather_timer = 3000;
        }
    }

    if ((world.tick % 64) == 0) spawn_fish(world);

    update_lure(world, input);

    for (int i = 0; i < k_max_fish; i++) {
        update_fish(world, world.fish[i], i);
    }
}

}  // namespace kf
