#pragma once

#include <cstdint>

#include "picomon_data.hpp"

// The whole game, as integers.
//
// No SDK, no floats, no allocation, no globals shared with the renderer. The
// RP2040 has no FPU, so anything that runs per frame stays in integers, and
// everything here is sized as a fixed array with a count rather than a
// container that could grow at the worst possible moment.
//
// The renderer reads this and writes nothing back. That is what lets the same
// sim run in the host tests, in the preview harness, and on the device without
// any of them knowing about the others.

namespace pm {

constexpr int k_max_party = 6;
constexpr int k_max_bag = 24;
constexpr int k_flag_bytes = 8;          // 64 flags, checked at build time
constexpr int k_dex_bytes = 4;           // 32 species of seen and caught bits

// A step takes this many ticks. Tile based movement with a visible slide is
// what makes a grid feel like a place rather than a spreadsheet, and holding
// the sub tile position as a step counter keeps it in integers.
constexpr int k_step_ticks = 8;

enum class Mode : uint8_t {
    Title,
    Overworld,
    Dialogue,
    Battle,
    Bag,
    Party,
    Faded,          // a beat of black between zones
};

enum class BattleState : uint8_t {
    Intro,          // "a wild X appeared"
    Menu,           // fight / bag / mon / run
    Moves,
    Message,
    Attack,         // the lunge and the hit
    Throw,          // a ball is in the air
    Wobble,
    Caught,
    Over,
};

enum class Sfx : uint8_t {
    None, Step, Bump, Select, Cancel, Encounter, Hit, SuperHit, Faint,
    Throw, Click, Caught, Heal, LevelUp, Item,
};

struct Mon {
    uint8_t species;
    uint8_t level;
    uint16_t xp;
    uint8_t hp;
    uint8_t max_hp;
    uint8_t moves[4];
    uint8_t pp[4];
};

struct BagSlot {
    uint8_t item;
    uint8_t count;
};

// What goes to flash. One block, written on a zone change and after a battle,
// never inside a render call: write_save disables XIP while it programs, and
// core 1 only survives that while parked in its RAM resident idle loop.
struct SaveData {
    uint8_t version;
    uint8_t zone, x, y, facing;
    uint16_t money;
    uint8_t party_count;
    Mon party[k_max_party];
    uint8_t bag_count;
    BagSlot bag[k_max_bag];
    uint8_t flags[k_flag_bytes];
    uint8_t seen[k_dex_bytes];
    uint8_t caught[k_dex_bytes];
};

constexpr uint8_t k_save_version = 1;

struct Input {
    bool up, down, left, right;
    bool up_pressed, down_pressed, left_pressed, right_pressed;
    bool a_pressed, b_pressed, x_pressed, y_pressed;
};

// What the battle wants said, as a code and two arguments. The sim never
// builds a string: the renderer turns these into text, which keeps every
// character array out of the state and out of the save block.
enum class Msg : uint8_t {
    None, WildAppeared, TrainerSent, YouUsed, FoeUsed, SuperEffective, NotVery,
    FoeFainted, YouFainted, GotAway, CouldNotRun, NoRunning, CaughtIt,
    BrokeFree, LevelUp, Evolved, OutOfPP, StatFell, StatRose,
};

struct Message {
    Msg kind;
    uint8_t a, b;
};

struct Battle {
    Mon foe;
    uint8_t active;              // index into the party
    BattleState state;
    uint8_t cursor;              // the 2x2 menu
    uint8_t move_cursor;
    uint8_t timer;               // counts down through an animation
    uint8_t anim_kind;           // the move's type, for the effect colour
    bool wild;
    bool player_first;
    uint8_t queued_move;         // the move the player picked this turn
    uint8_t pending;             // what to do when the message finishes

    // Stat stages, -6 to +6, applied as a fraction rather than a float.
    int8_t atk_stage, def_stage, spd_stage;
    int8_t foe_atk_stage, foe_def_stage, foe_spd_stage;

    // Trainer battles remember who sent the creature out.
    uint8_t trainer_npc;         // 0xFF for a wild battle
    uint8_t trainer_next;        // index into the trainer's party list
    uint16_t reward;

    // Capture.
    uint8_t ball_item;
    uint8_t wobbles;
    bool caught;

    // A turn never needs a fifth line, so the queue is four and overflow is
    // dropped rather than grown.
    Message msgq[4];
    uint8_t msg_head, msg_count;
};

struct World {
    Mode mode;
    uint32_t rng;

    // Overworld.
    uint8_t zone;
    uint8_t tx, ty;              // the tile the player occupies
    uint8_t facing;
    uint8_t step;                // 0 standing, else ticks into the current step
    uint8_t step_from_x, step_from_y;
    uint8_t anim_phase;          // which walk frame
    uint8_t ledge_hop;           // ticks left of a ledge hop
    uint8_t steps_walked;        // encounter accumulator

    // Dialogue, which is also how every other mode says something.
    uint16_t text_first;
    uint8_t text_count, text_page;
    uint8_t talking_npc;         // 0xFF when the text is not an NPC's

    // Menus.
    uint8_t menu_cursor;
    uint8_t menu_pocket;

    // Fade between zones.
    uint8_t fade;

    Battle battle;

    // The player.
    Mon party[k_max_party];
    uint8_t party_count;
    BagSlot bag[k_max_bag];
    uint8_t bag_count;
    uint16_t money;
    uint8_t flags[k_flag_bytes];
    uint8_t seen[k_dex_bytes];
    uint8_t caught[k_dex_bytes];

    bool save_pending;
    Sfx sfx;                     // one event per tick, read and cleared by audio
};

// ---- lifecycle
void world_init(World& w, uint32_t seed);
void world_new_game(World& w);
void world_tick(World& w, const Input& in);
void world_make_save(const World& w, SaveData& out);
bool world_load(World& w, const SaveData& in);

// ---- queries the renderer needs, and nothing it does not
const Zone& zone_of(const World& w);
uint8_t tile_at(const Zone& z, int x, int y);
bool tile_walkable(const Zone& z, int x, int y);
bool npc_present(const World& w, const NpcDef& n);
// Sub tile offset of the player, in 1/256ths of a tile, for the smooth step.
void player_offset(const World& w, int16_t& ox, int16_t& oy);

// ---- flags
bool flag_get(const World& w, uint8_t flag);
void flag_set(World& w, uint8_t flag);

// ---- party and stats, shared with the renderer for the HP plates
uint8_t stat_hp(uint8_t species, uint8_t level);
uint8_t stat_of(uint8_t base, uint8_t level);
uint16_t xp_for_level(uint8_t level);
Mon make_mon(uint8_t species, uint8_t level);

// ---- the two formulas worth naming
int damage_of(const Mon& attacker, const Mon& defender, uint8_t move,
              int8_t atk_stage, int8_t def_stage, uint8_t roll);
int type_multiplier(uint8_t attack_type, uint8_t defend_type);
uint8_t catch_value(const Mon& target, uint8_t ball_quarters);
uint8_t shake_threshold(uint8_t catch_a);

}  // namespace pm
