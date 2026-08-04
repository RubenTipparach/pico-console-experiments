#pragma once

// Kingfisher's simulation. Pure integer C++, no SDK, no floats, no allocation.
//
// Everything here runs on the host test suite, which is where the tuning
// claims (every fish is landable, greedy reeling snaps the line on strong
// fish, the state machine can not wedge) are actually proven.
//
// Units: world positions are 24.8 fixed point, 256 = one world unit. One tick
// is 10 ms, the 32blit update cadence. Distances are compared in whole units
// (value >> 8) before squaring, so nothing here can overflow 32 bits.

#include <cstdint>

#include "tuning.hpp"

namespace kf {

constexpr int k_fp = 8;
constexpr int32_t k_one = 1 << k_fp;

constexpr int k_species_count = 12;
constexpr int k_max_fish = 8;

// Time of day masks.
constexpr uint8_t k_day = 1;
constexpr uint8_t k_night = 2;

struct Species {
    const char* name;
    uint8_t band;          // 0 shallow, 1 mid, 2 deep; deeper = farther cast
    uint8_t time_mask;     // k_day | k_night
    uint8_t rarity;        // spawn weight, higher = more common
    uint8_t strength;      // 1..10, drives the fight
    int16_t size_min;      // cm
    int16_t size_max;
    uint16_t points;
    uint8_t r, g, b;       // body tint
};

extern const Species k_species[k_species_count];

enum class Mode : uint8_t {
    Idle,       // rod ready
    Aiming,     // A held, power meter bouncing
    Flying,     // lure in the air
    Sinking,    // lure in the water, fish may bite
    Fight,      // fish hooked
    Landed,     // catch card showing
};

enum class FishState : uint8_t {
    Gone,       // slot free
    Wander,
    Curious,    // approaching the lure
    Nibbling,
    Biting,     // hook window open
    Hooked,
    Flee,
};

enum class FightPhase : uint8_t { Run, Tire };

struct Fish {
    uint8_t species;
    FishState state;
    int16_t size_cm;
    int32_t x, y, z;       // fp8; y is depth below the surface, positive down
    int32_t vx, vz;        // fp8 per tick
    int32_t tx, tz;        // wander target, fp8
    uint16_t timer;
    uint8_t nibbles_left;
    uint8_t reserved;
};

struct Records {
    int16_t best_cm[k_species_count];
    uint16_t caught[k_species_count];
    uint32_t score;
};

// What write_save persists. Bump the magic when the layout changes so a stale
// save is ignored instead of misread. sound_off belongs to the shell, not the
// sim: world_make_save zeroes it and the game layer stamps it before writing.
struct SaveData {
    uint32_t magic;        // 'K','F','R','2'
    Records records;
    uint8_t sound_off;
    uint8_t reserved[3];
};
constexpr uint32_t k_save_magic = 0x3252464Bu;

// One tick's worth of input, already edge detected by the caller.
struct Input {
    bool a;
    bool a_pressed;
    bool a_released;
    bool b_pressed;
    bool left;
    bool right;
    bool left_pressed;
    bool right_pressed;
};

// One tick's worth of things the presentation layers care about. Reset at the
// start of every tick; sound and particles both read it after world_tick.
struct Events {
    bool cast;
    bool splash;           // lure hit the water
    bool nibble;
    bool bite;             // hook window opened
    bool hooked;
    bool snap;             // line broke
    bool escape;           // line ran out
    bool caught;
    bool new_record;
    bool leap;             // hooked fish broke the surface
    bool wiggle;           // rod wiggle registered during a fight
};

struct World {
    uint32_t rng;
    uint32_t tick;
    uint16_t day_tick;     // wraps at k_day_length
    uint8_t raining;
    uint16_t weather_timer;

    Mode mode;
    uint8_t power;         // 0..255 while aiming
    int8_t power_dir;
    int8_t aim;            // -10..10, lateral cast offset

    int32_t lure_x, lure_y, lure_z;   // y positive down, 0 at the surface
    int32_t lure_vx, lure_vy, lure_vz;
    uint16_t twitch_timer;
    uint16_t retrieve_hold;   // ticks A has been held towing the lure
    uint16_t retrieve_frac;   // sub fp remainder of the tow speed

    int8_t hooked_fish;    // index into fish[], -1 when none
    uint16_t tension;      // 0..1023; the red zone starts at k_tension_danger
    uint16_t danger;       // ticks spent in the red zone, break at k_danger_ticks
    int32_t line_len;      // fp8 world units of line out
    int32_t line_max;
    uint16_t stamina;
    uint16_t stamina_max;
    int8_t run_dir;        // -1 or 1, which way the fish runs
    int8_t last_wiggle;    // last rod wiggle direction, 0 before the first
    FightPhase fight_phase;
    uint16_t phase_timer;
    uint16_t leap_timer;   // hooked fish surface break, cosmetic

    uint16_t bite_timer;   // hook window countdown on the biting fish
    uint16_t card_timer;
    int8_t card_species;
    int16_t card_size;
    bool card_record;

    Fish fish[k_max_fish];
    Records records;
    bool save_pending;

    Events ev;
};

constexpr uint16_t k_day_length = 18000;      // 3 minutes per full cycle

// Fight tuning, including the tension danger model, lives in tuning.hpp.

void world_init(World& world, uint32_t seed);
void world_tick(World& world, const Input& input);

bool world_load(World& world, const SaveData& data);
void world_make_save(const World& world, SaveData& out);

// 0..255 position in the day cycle, for palette lerping.
uint8_t day_phase(const World& world);
bool is_night(const World& world);

// The hook's distance from the boat in tenths of a meter, for the HUD. One
// world unit is one meter. Shown for the hook's whole life: rising as a
// cast flies, steady while the lure sinks, counting down through a fight.
// Measured from the collect radius with the same reach the fight
// initialises its line from, so the number is continuous at the hook, and
// rounded UP during a fight so it reads 0 exactly when the fish is
// collected and never a moment before: the meter reaching zero IS the
// catch. Returns 0 when no hook is out.
int hook_distance_dm(const World& world);

// Test hook: force a bite-ready fish of the given species and size so fight
// tuning can be exercised directly. Returns the fish index.
int world_test_hook(World& world, int species, int size_cm);

}  // namespace kf
