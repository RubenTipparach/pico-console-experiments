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
// means deeper water means rarer fish: the power meter is the difficulty
// dial. The ranges themselves live in tuning.hpp with the rest of the pond.
struct Band { int32_t z_min, z_max; int32_t depth_min, depth_max; };
const Band k_bands[3] = {
    {k_lake_near_fp, k_shallow_max_fp, k_one / 4, k_one / 2},
    {k_shallow_max_fp, k_mid_max_fp, k_one / 2, 5 * k_one / 4},
    {k_mid_max_fp, k_lake_far_fp, 5 * k_one / 4, 2 * k_one},
};

int band_for_z(int32_t z) {
    if (z < k_bands[1].z_min) return 0;
    if (z < k_bands[2].z_min) return 1;
    return 2;
}

constexpr int32_t k_lake_half_width = k_lake_half_width_fp;
constexpr int32_t k_boat_z = 0;

// Fight tuning lives in tuning.hpp, one documented dial per behaviour. The
// host tests assert the consequences: patient technique lands everything,
// greedy reeling loses strong fish.

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

uint32_t fish_weight_g(int species, int size_cm) {
    if (species < 0 || species >= k_species_count || size_cm <= 0) return 0;
    // Length cubed over the condition factor. A 200 cm fish is 8000000 / 100,
    // which is 80 kg and still nowhere near overflowing 32 bits.
    const uint32_t cm = static_cast<uint32_t>(size_cm);
    return (cm * cm * cm) / k_weight_den;
}

uint32_t tour_target_for_day(int day) {
    if (day < 1) day = 1;
    if (day > k_tour_days) day = k_tour_days;
    return k_tour_target_base +
           k_tour_target_step * static_cast<uint32_t>(day - 1);
}

void records_add_score(Records& records, uint32_t score) {
    if (score == 0) return;
    // Insertion into a board of ten, highest first. Ten entries is small
    // enough that shuffling beats sorting, and it keeps the board's order an
    // invariant of the insert rather than something a caller has to redo.
    for (int i = 0; i < k_high_scores; i++) {
        if (score <= records.high[i]) continue;
        for (int j = k_high_scores - 1; j > i; j--) {
            records.high[j] = records.high[j - 1];
        }
        records.high[i] = score;
        return;
    }
}

int hook_distance_dm(const World& world) {
    int32_t dist;
    switch (world.mode) {
        case Mode::Fight:
            dist = world.line_len - k_catch_len;
            if (dist <= 0) return 1;   // still hooked: never zero mid fight
            break;
        case Mode::Flying:
        case Mode::Sinking: {
            const int32_t ax = world.lure_x < 0 ? -world.lure_x
                                                : world.lure_x;
            dist = world.lure_z + ax - k_catch_len;
            if (dist < 0) dist = 0;
            break;
        }
        default:
            return 0;
    }
    return static_cast<int>((dist * 10 + 255) / 256);
}

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
    out.sound_off = 0;
    out.reserved[0] = out.reserved[1] = out.reserved[2] = 0;
}

namespace {

// The end of a tournament day. Made the quota and the run goes on with a
// harder one; missed it and the run is over there and then.
//
// Score is every gram over quota times the days survived, so a big fish on
// day one is worth ten times the same fish on day ten. That is deliberate:
// without the multiplier the best play is to land the minimum and idle, and
// a tournament nobody pushes in is not a tournament.
void tour_end_of_day(World& world) {
    if (world.tour_state != TourState::Running) return;

    if (world.tour_today_g < world.tour_target_g) {
        world.tour_state = TourState::Lost;
        const uint32_t days = world.tour_day > 0
            ? static_cast<uint32_t>(world.tour_day - 1) : 0;
        world.tour_score = (world.tour_over_g / k_tour_score_div) * days;
        world.ev.tour_lost = true;
        return;
    }

    world.tour_over_g += world.tour_today_g - world.tour_target_g;
    world.tour_today_g = 0;

    if (world.tour_day >= k_tour_days) {
        world.tour_state = TourState::Won;
        world.tour_score = (world.tour_over_g / k_tour_score_div) * k_tour_days;
        world.ev.tour_won = true;
        return;
    }

    world.tour_day++;
    world.tour_target_g = tour_target_for_day(world.tour_day);
    world.tour_state = TourState::DayPassed;
    world.tour_card_timer = k_tour_card_ticks;
    world.ev.tour_day_passed = true;
}

void fish_flee(Fish& fish);

void reset_lure(World& world) {
    world.mode = Mode::Idle;
    world.bite_timer = 0;
    world.lure_x = 0;
    world.lure_y = 0;
    world.lure_z = k_one;
    world.lure_vx = world.lure_vy = world.lure_vz = 0;
    world.twitch_timer = 0;
    world.retrieve_hold = 0;
    world.retrieve_frac = 0;
    world.reel_click_acc = 0;
    world.hooked_fish = -1;
    // Whatever was mouthing the lure has nothing to mouth now. Without this
    // a fish left Biting after a recall would hold that state forever, since
    // only the Sinking mode logic ticks the bite window.
    for (auto& fish : world.fish) {
        if (fish.state == FishState::Curious ||
            fish.state == FishState::Nibbling ||
            fish.state == FishState::Biting) {
            fish_flee(fish);
        }
    }
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
            fish.z = clamp32(fish.z, 2 * k_one, k_lake_far_fp);
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
    // The meter starts where the fish's first effort will put it, rather
    // than at an arbitrary number: a hooked fish is already pulling.
    world.tension = 0;
    world.line_stress = 0;
    world.strain = 0;
    world.line_frac = 0;
    world.fish_effort = 0;
    world.fish_dir = 1;
    world.dir_timer = k_dir_ticks_base;
    world.danger = 0;
    world.stamina = static_cast<uint16_t>(
        k_stamina_base + fish.size_cm * k_stamina_per_cm +
        s.strength * k_stamina_per_strength);
    world.stamina_max = world.stamina;
    world.stamina_cap = world.stamina;
    world.spent_timer = 0;
    world.run_dir = rnd(world.rng, 2) ? 1 : -1;
    world.last_wiggle = 0;
    world.wiggle_cd = 0;
    world.line_len = world.lure_z + (world.lure_x < 0 ? -world.lure_x
                                                      : world.lure_x);
    world.line_max = world.line_len + world.line_len / k_line_slack_div +
                     k_line_slack_fp;
    world.fight_phase = FightPhase::Run;
    world.phase_timer = static_cast<uint16_t>(
        k_run_ticks_base + rnd(world.rng, k_run_ticks_vary +
                                          s.strength * k_run_ticks_per_strength));
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
            // The quota counts weight, not fish. A day of minnows is not a
            // day's work.
            if (world.tour_state == TourState::Running) {
                world.tour_today_g += fish_weight_g(fish.species,
                                                    fish.size_cm);
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

// Pick a direction for the fish to put its effort into. A running fish
// mostly pulls away; a resting one mostly holds, and sometimes swims back at
// the boat, which is the moment that gives the player free line.
void reroll_direction(World& world) {
    const bool running = world.fight_phase == FightPhase::Run;
    const int roll = rnd(world.rng, 256);
    const int away = running ? k_dir_away_run : k_dir_away_tire;
    if (roll < away) {
        world.fish_dir = 1;
    } else if (!running && roll < away + k_dir_toward_tire) {
        world.fish_dir = -1;
    } else {
        world.fish_dir = 0;
    }
    world.dir_timer = static_cast<uint16_t>(
        k_dir_ticks_base + rnd(world.rng, k_dir_ticks_vary));
}

void update_fight(World& world, const Input& input) {
    const Fish& fish = world.fish[world.hooked_fish];
    const Species& s = k_species[fish.species];
    const bool reeling = input.a;

    // Rod wiggle: alternating left and right presses. Each swap eases the
    // line and costs the fish, which is what makes it worth doing while a
    // fish is working hard.
    if (world.wiggle_cd > 0) world.wiggle_cd--;
    int wiggle = 0;
    if (input.left_pressed && world.last_wiggle >= 0) wiggle = -1;
    else if (input.right_pressed && world.last_wiggle <= 0) wiggle = 1;
    // A swap still tracks direction while the rod is reloading, so the player
    // can keep the alternation going, but only a wiggle off cooldown does
    // anything. Spamming it is neither punished nor rewarded: it is simply
    // not the mechanic.
    if (wiggle != 0 && world.wiggle_cd > 0) {
        world.last_wiggle = static_cast<int8_t>(wiggle);
        wiggle = 0;
    }
    if (wiggle != 0) {
        world.last_wiggle = static_cast<int8_t>(wiggle);
        world.wiggle_cd = k_wiggle_cooldown;
        world.ev.wiggle = true;
        // A wiggle knocks the fish off its effort, which is what takes the
        // load off the line. Easing the meter directly would do nothing: it
        // is recomputed from the forces every tick, so relief has to come
        // from changing a force.
        world.fish_effort = static_cast<uint8_t>(
            world.fish_effort > k_wiggle_effort_drop
                ? world.fish_effort - k_wiggle_effort_drop : 0);
        // It dumps the rod's accumulated load too. Without this a wiggle can
        // only stop the strain climbing, never undo it, and a player who has
        // let it build has no move left except to stop reeling entirely.
        world.strain = static_cast<uint16_t>(
            world.strain > k_strain_wiggle_shed
                ? world.strain - k_strain_wiggle_shed : 0);
        // Countering the way the fish is running costs it most, but a jerked
        // rod tires a fish whatever it is doing. Without the second half a
        // holding fish regenerates through a wiggle for free, and the rod
        // stops being worth working between runs.
        if (world.stamina > 0) {
            const int cost = (world.fish_dir != 0 && wiggle == -world.run_dir)
                                 ? k_wiggle_drain
                                 : k_wiggle_drain / 2;
            world.stamina = static_cast<uint16_t>(
                world.stamina > cost ? world.stamina - cost : 0);
        }
    }

    // ---- what the fish is doing ----
    //
    // Effort walks toward a target rather than snapping to it, so a fish
    // works up to a surge and eases out of it. A tired fish cannot reach its
    // target at all: stamina is the ceiling on everything it does.
    const bool running = world.fight_phase == FightPhase::Run;
    int target = running
        ? k_effort_run_min + rnd(world.rng, k_effort_run_vary)
        : k_effort_tire_min + rnd(world.rng, k_effort_tire_vary);
    // A fish that is being pulled on fights back. This is what stops the
    // crank being free: hold it down and the fish answers with effort, which
    // is stress, which is the meter climbing.
    if (reeling) target += k_effort_reel_bump;
    if (world.stamina_max > 0) {
        target = (target * world.stamina) / world.stamina_max;
    } else {
        target = 0;
    }
    int effort = world.fish_effort;
    if (effort < target) effort = effort + k_effort_step > target
                                      ? target : effort + k_effort_step;
    else if (effort > target) effort = effort - k_effort_step < target
                                           ? target : effort - k_effort_step;
    world.fish_effort = static_cast<uint8_t>(clamp32(effort, 0, 255));

    if (world.dir_timer > 0) world.dir_timer--;
    if (world.dir_timer == 0) reroll_direction(world);

    // ---- forces on the line ----
    //
    // The fish's share can never reach the rod's limit on its own (see the
    // static_assert in tuning.hpp). The player's share is what can take it
    // over, so a break is always something the player did.
    const int fish_stress =
        (s.strength * k_stress_per_strength * world.fish_effort) / 255;
    const int tow_stress = reeling ? k_tow_stress_base : 0;
    // Only a fish that is actually pulling against the rod loads it. One
    // swimming at the boat is slack line, however hard it is working.
    const int loaded = world.fish_dir >= 0 ? fish_stress : fish_stress / 4;

    // Strain is the part of the reading that remembers. Pulling against a
    // fish with something left loads the rod further every tick it goes on,
    // so what breaks the line is the length of the haul rather than any one
    // moment of it. Nothing accumulates against a spent fish, which is what
    // keeps the exhaustion window worth playing for.
    const bool fighting = world.stamina > 0;
    if (reeling) {
        // A rod under load does not unload because the fish stopped pulling.
        // Holding through a spent fish holds the strain where it is, and only
        // easing off sheds it, which is what makes the length of a pull the
        // thing that costs: hold long enough, through enough of the fish's
        // comebacks, and even something small eventually takes the line past
        // what it will stand.
        if (fighting) {
            const int gain = (loaded * k_strain_gain_effort +
                              fish.size_cm * k_strain_gain_mass) / 64;
            world.strain = static_cast<uint16_t>(
                clamp32(world.strain + gain, 0, k_strain_max));
        }
    } else {
        world.strain = static_cast<uint16_t>(
            world.strain > k_strain_relief ? world.strain - k_strain_relief : 0);
    }

    world.line_stress = static_cast<uint16_t>(
        clamp32(loaded + tow_stress + world.strain / k_strain_fp, 0, 65535));

    // The meter is that stress against the rod, slewed: the climb is the
    // warning, and a bar that jumped would not be one.
    const int shown = clamp32((static_cast<int>(world.line_stress) * 1023) /
                                  k_rod_starter_max, 0, 1023);
    const int current = world.tension;
    int next = current;
    if (shown > current) {
        next = current + k_tension_slew > shown ? shown
                                               : current + k_tension_slew;
    } else if (shown < current) {
        next = current - k_tension_slew < shown ? shown
                                               : current - k_tension_slew;
    }
    world.tension = static_cast<uint16_t>(clamp32(next, 0, 1023));

    // The line breaks only after its stress has sat at the rod's limit for
    // the whole window. Easing off drops the tow out of the sum immediately,
    // which is why letting go is the answer.
    if (world.tension >= k_tension_full) {
        world.danger++;
        if (world.danger >= k_danger_ticks) {
            world.ev.snap = true;
            end_fight(world, false);
            return;
        }
    } else if (world.danger > 0) {
        world.danger--;
    }

    // ---- line movement ----
    //
    // Both sides move the line every tick and the net is what happens. The
    // reel slows down the harder the fish works, from 2 m/s against a spent
    // fish to a tenth of that against one fighting flat out, and it is never
    // as quick as the 4 m/s an empty hook tows at.
    int delta = 0;   // fp<<8, positive pays line out
    if (world.fish_dir != 0) {
        const int pull = (k_fish_pull_max_fp256 * s.strength *
                          world.fish_effort) / (10 * 255);
        // A fish swimming back at the boat gives line up, but it is not
        // swimming as hard as one fleeing, and it must not reel itself in.
        delta += world.fish_dir > 0 ? pull : -(pull / k_toward_div);
    }
    if (reeling) {
        // Mass first: a heavy fish is slow to move even limp. Then effort,
        // which is the fish actively refusing.
        int top = k_fight_reel_max_fp256 - fish.size_cm * k_reel_mass_drag;
        if (top < k_fight_reel_min_fp256) top = k_fight_reel_min_fp256;
        const int span = top - k_fight_reel_min_fp256;
        const int reel = top - (span * world.fish_effort) / 255;
        delta -= reel;
    }

    // Carry the fraction so a tenth of a metre a second is not rounded away.
    int total = static_cast<int>(world.line_frac) + delta;
    world.line_frac = static_cast<uint16_t>(total & 0xFF);
    const int moved = total >> 8;
    world.line_len += moved;
    if (moved < 0) {
        world.reel_click_acc =
            static_cast<uint16_t>(world.reel_click_acc - moved);
        if (world.reel_click_acc >= k_reel_click_fp) {
            world.reel_click_acc -= k_reel_click_fp;
            world.ev.reel_click = true;
        }
    }

    if (world.line_len >= world.line_max) {
        world.ev.escape = true;
        end_fight(world, false);
        return;
    }

    // ---- stamina ----
    //
    // Working costs the fish, and being cranked on costs it more. Easing off
    // lets it get its wind back fast, so parking the reel between runs hands
    // the fish everything back: the pressure has to stay on.
    if (world.spent_timer > 0) {
        // Second wind. The fish is refilling no matter what the player does,
        // so the easy window closes on its own and the player has to spend it
        // rather than wait it out. Rate is a share of this fish's own pool,
        // which is what makes the comeback take the same time for everything
        // in the lake.
        int rate = world.stamina_max / k_spent_recharge_ticks;
        if (rate < 1) rate = 1;
        const int gained = world.stamina + rate;
        world.stamina = static_cast<uint16_t>(
            gained > world.stamina_cap ? world.stamina_cap : gained);
        world.spent_timer--;
        if (world.stamina >= world.stamina_cap) world.spent_timer = 0;
    } else {
        const int spend = (world.fish_effort * k_drain_effort) / 255 +
                          (reeling ? k_drain_reel : 0);
        const int recover = (!reeling && wiggle == 0) ? k_stamina_regen : 0;
        const int net = spend - recover;
        if (net > 0) {
            world.stamina = static_cast<uint16_t>(
                world.stamina > net ? world.stamina - net : 0);
        } else if (net < 0 && world.stamina < world.stamina_cap) {
            const int gained = world.stamina - net;
            world.stamina = static_cast<uint16_t>(
                gained > world.stamina_cap ? world.stamina_cap : gained);
        }

        // Run it out and it comes back, but smaller. The ceiling dropping a
        // quarter each time is the whole reason the cycle converges: six
        // winds in, a legend has a quarter of the fish it started as.
        if (world.stamina == 0) {
            const int floor = (world.stamina_max * k_wind_cap_floor_num) /
                              k_wind_cap_floor_den;
            int next = (world.stamina_cap * k_wind_cap_num) / k_wind_cap_den;
            if (next < floor) next = floor;
            world.stamina_cap = static_cast<uint16_t>(next);
            world.spent_timer = k_spent_recharge_ticks;
        }
    }

    if (world.line_len <= k_catch_len) {
        end_fight(world, true);
        return;
    }

    // Phase changes. A spent fish cannot muster a run until it has breathed.
    if (world.phase_timer > 0) world.phase_timer--;
    if (world.phase_timer == 0) {
        const bool can_run = world.stamina > world.stamina_max / k_run_rest_div;
        if (world.fight_phase == FightPhase::Run || !can_run) {
            world.fight_phase = FightPhase::Tire;
            world.phase_timer = static_cast<uint16_t>(
                k_tire_ticks_base + rnd(world.rng, k_tire_ticks_vary));
        } else {
            world.fight_phase = FightPhase::Run;
            world.run_dir = rnd(world.rng, 2) ? 1 : -1;
            world.phase_timer = static_cast<uint16_t>(
                k_run_ticks_base + rnd(world.rng, k_run_ticks_vary +
                    s.strength * k_run_ticks_per_strength));
            // A fresh run sometimes breaks the surface.
            if (rnd(world.rng, 256) < 90) {
                world.leap_timer = 45;
                world.ev.leap = true;
            }
        }
        reroll_direction(world);
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
                world.lure_vz = k_cast_vz_base +
                                (world.power * k_cast_vz_per255) / 256;
                world.lure_vx = world.aim * 3;
                world.lure_vy = k_cast_vy;      // upward (y is down)
                world.ev.cast = true;
            }
            break;
        }

        case Mode::Flying:
            if (input.b_pressed) {              // recall out of the air
                reset_lure(world);
                break;
            }
            world.lure_vy += k_cast_gravity;    // y positive down
            world.lure_x += world.lure_vx;
            world.lure_y += world.lure_vy / 8;
            world.lure_z += world.lure_vz;
            world.lure_x = clamp32(world.lure_x, -k_lake_half_width,
                                   k_lake_half_width);
            world.lure_z = clamp32(world.lure_z, k_one, k_lake_far_fp);
            if (world.lure_y >= 0) {
                world.lure_y = 0;
                world.lure_vx = world.lure_vy = world.lure_vz = 0;
                world.mode = Mode::Sinking;
                world.ev.splash = true;
            }
            break;

        case Mode::Sinking: {
            if (input.b_pressed) {              // instant recall
                reset_lure(world);
                break;
            }
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
                // Towing: the reel winds up over a couple of seconds to its
                // cruising speed. A sub fp accumulator carries the fraction
                // so the speed survives integer math at tick granularity.
                if (world.retrieve_hold < k_retrieve_ramp_ticks) {
                    world.retrieve_hold++;
                }
                const int speed = (k_retrieve_max_fp256 *
                                   world.retrieve_hold) /
                                  k_retrieve_ramp_ticks;
                const int total = world.retrieve_frac + speed;
                world.retrieve_frac = static_cast<uint16_t>(total & 0xFF);
                const int move = total >> 8;
                if (move > 0) {
                    world.lure_z -= move;
                    world.lure_x -= world.lure_x / 64;
                    world.twitch_timer = 60;   // a moving lure draws eyes
                    world.reel_click_acc =
                        static_cast<uint16_t>(world.reel_click_acc + move);
                    if (world.reel_click_acc >= k_reel_click_fp) {
                        world.reel_click_acc -= k_reel_click_fp;
                        world.ev.reel_click = true;
                    }
                }
                if (world.lure_z <= k_one) reset_lure(world);
            } else {
                world.retrieve_hold = 0;
                world.retrieve_frac = 0;
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

void world_start(World& world, GameMode mode) {
    world.game_mode = mode;
    world.tour_day = 0;
    world.tour_target_g = 0;
    world.tour_today_g = 0;
    world.tour_over_g = 0;
    world.tour_score = 0;
    world.tour_card_timer = 0;
    world.tour_state = TourState::Idle;
    // The rod comes back to the boat whatever was happening. Starting a run
    // out of a finished one would otherwise begin it mid fight, with a fish
    // hooked from a tournament that is already over.
    reset_lure(world);
    world.card_species = -1;
    world.card_timer = 0;
    if (mode != GameMode::Tournament) return;

    world.tour_state = TourState::Running;
    world.tour_day = 1;
    world.tour_target_g = tour_target_for_day(1);
    // Day one starts at dawn rather than mid morning: a quota counted over a
    // day should get the whole of one.
    world.day_tick = 0;
}

void world_tick(World& world, const Input& input) {
    world.ev = Events{};
    world.tick++;

    // A tournament is frozen while its result card is up, so the day that
    // just ended cannot leak fishing time into the next one.
    if (world.tour_card_timer > 0) {
        world.tour_card_timer--;
        if (world.tour_card_timer == 0 &&
            world.tour_state == TourState::DayPassed) {
            world.tour_state = TourState::Running;
        }
        return;
    }
    // A finished run stops the pond. The score is on screen and the only way
    // on is out through the menu.
    if (world.tour_state == TourState::Lost ||
        world.tour_state == TourState::Won) {
        return;
    }

    world.day_tick++;
    if (world.day_tick >= k_day_length) {
        world.day_tick = 0;
        tour_end_of_day(world);
        if (world.tour_card_timer > 0) return;
    }

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
