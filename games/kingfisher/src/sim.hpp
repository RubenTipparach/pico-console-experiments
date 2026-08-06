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
    // Where in that band's water column it holds: 0 near the surface, 1 in
    // midwater, 2 hard on the bottom. How far out to cast picks the band; the
    // hook's depth then picks between the species inside it, which is what
    // the up and down arrows are for.
    uint8_t depth;
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

// What the fishing is for.
//
// Free is the pond as it was: cast, catch, keep records, stop when you like.
// Tournament puts a clock and a quota on it: ten days, a weight to make each
// day, and one bad day ends it. Both run the same pond and the same fight;
// the tournament only watches what comes out of it.
enum class GameMode : uint8_t { Free, Tournament };

// Where a tournament is up to. Idle means none is running, which is also
// what Free mode leaves this in.
enum class TourState : uint8_t {
    Idle,
    Running,
    DayPassed,   // made the quota, the card is showing before the next day
    Lost,        // missed a quota, the run is over
    Won,         // all ten days made
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

constexpr int k_tour_days = 10;
constexpr int k_high_scores = 10;

struct Records {
    int16_t best_cm[k_species_count];
    uint16_t caught[k_species_count];
    uint32_t score;
    // Best tournament runs, highest first, zero for an empty slot.
    uint32_t high[k_high_scores];
};

// What write_save persists. Bump the magic when the layout changes so a stale
// save is ignored instead of misread. sound_off belongs to the shell, not the
// sim: world_make_save zeroes it and the game layer stamps it before writing.
struct SaveData {
    uint32_t magic;        // 'K','F','R','3'
    Records records;
    uint8_t sound_off;
    uint8_t reserved[3];
};
// Bumped from R2 when the high score board joined Records: an R2 save read
// as an R3 would put whatever followed the old struct into the board.
constexpr uint32_t k_save_magic = 0x3352464Bu;

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
    bool up;               // raise the lure in the water
    bool down;             // drop it
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
    bool reel_click;       // the ratchet: a chunk of line came in
    bool tour_day_passed;  // a tournament day's quota was made
    bool tour_won;         // all ten days made
    bool tour_lost;        // a day's quota was missed
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
    // The depth the player is holding the lure at. lure_y chases this, so a
    // depth chosen once is kept instead of having to be held with the dpad.
    int32_t lure_target_y;
    int32_t lure_vx, lure_vy, lure_vz;
    uint16_t twitch_timer;
    uint16_t retrieve_hold;   // ticks A has been held towing the lure
    uint16_t retrieve_frac;   // sub fp remainder of the tow speed
    uint16_t reel_click_acc;  // fp of line wound since the last ratchet click

    int8_t hooked_fish;    // index into fish[], -1 when none
    uint16_t tension;      // 0..1023, line stress against the rod's limit
    uint16_t line_stress;  // the truth the meter slews toward
    uint16_t strain;       // rod loading from a sustained pull, adds to stress
    uint16_t line_frac;    // sub fp remainder of this tick's line movement
    uint8_t fish_effort;   // 0..255, how hard the fish is working
    int8_t fish_dir;       // 1 away, 0 holding, -1 back toward the boat
    uint16_t dir_timer;    // ticks until the fish picks a direction again
    uint16_t danger;       // ticks spent in the red zone, break at k_danger_ticks
    int32_t line_len;      // fp8 world units of line out
    int32_t line_max;
    uint16_t stamina;
    uint16_t stamina_max;
    uint16_t stamina_cap;  // this wind's ceiling, a quarter lower each time
    uint16_t spent_timer;  // ticks left in a second wind, 0 when not in one
    int8_t run_dir;        // -1 or 1, which way the fish runs
    int8_t last_wiggle;    // last rod wiggle direction, 0 before the first
    uint16_t wiggle_cd;    // ticks until a wiggle bites again
    FightPhase fight_phase;
    uint16_t phase_timer;
    uint16_t leap_timer;   // hooked fish surface break, cosmetic

    uint16_t bite_timer;   // hook window countdown on the biting fish
    uint16_t card_timer;
    int8_t card_species;
    int16_t card_size;
    bool card_record;

    // ---- the tournament ----
    GameMode game_mode;
    TourState tour_state;
    uint8_t tour_day;         // 1..k_tour_days while running
    uint32_t tour_target_g;   // what today asks for
    uint32_t tour_today_g;    // landed so far today
    uint32_t tour_over_g;     // total made over quota across the days survived
    uint32_t tour_score;      // final, set when the run ends
    uint16_t tour_card_timer; // day result card

    Fish fish[k_max_fish];
    Records records;
    bool save_pending;

    Events ev;
};

constexpr uint16_t k_day_length = 18000;      // 3 minutes per full cycle

// The fishing day runs dawn to midnight, and day_tick spans exactly that. A
// tournament day therefore ends when the clock reads midnight, which is what
// makes the time on the HUD worth showing: it is the deadline, not decoration.
constexpr uint8_t k_day_start_hour = 6;
constexpr uint8_t k_day_end_hour = 24;
// When the light goes and the night feeders come up. Kept in step with the
// sky's night keyframe in render.cpp, so what the water looks like and what
// is biting in it never disagree.
constexpr uint8_t k_night_hour = 20;

// Fight tuning, including the tension danger model, lives in tuning.hpp.

void world_init(World& world, uint32_t seed);
void world_tick(World& world, const Input& input);

bool world_load(World& world, const SaveData& data);
void world_make_save(const World& world, SaveData& out);

// 0..255 position in the day cycle, for palette lerping.
uint8_t day_phase(const World& world);
bool is_night(const World& world);

// How deep the water is under the lure right now, in fp8, which is as deep as
// the lure can be wound down. Shallow near the boat and deep out in the middle,
// so the underwater viewport frames this rather than a fixed column: the bed
// belongs at the bottom of the band wherever the player has cast, or a shallow
// cast reads as a hook that will not sink.
int32_t water_depth_here(const World& world);

// The wall clock, in minutes since midnight. Runs from k_day_start_hour to
// one minute short of k_day_end_hour, so it never reads 24:00: the day is
// over at that point and the tick has already wrapped to the next dawn.
uint16_t clock_minutes(const World& world);

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

// ---- the tournament ----

// What a fish that long weighs, in grams. Length cubed against a condition
// factor, which is how a real fish's weight is estimated and what makes the
// difference between a perch and a sturgeon a matter of kilograms rather
// than centimetres. This is the number the quota is counted in.
uint32_t fish_weight_g(int species, int size_cm);

// What day n of a tournament asks for, in grams.
uint32_t tour_target_for_day(int day);

// Start a run. Free mode resets the tournament state and leaves it alone.
void world_start(World& world, GameMode mode);

// Record a run's score into the board, highest first. Exposed because the
// board outlives a run and the shell owns saving it.
void records_add_score(Records& records, uint32_t score);

}  // namespace kf
