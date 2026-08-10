#pragma once

// Cat Coin Pusher: the rules, with no renderer and no SDK behind them.
//
// A demake of cat_coin_pusher.p8, which ran at 128x128 and 60 Hz. Every tuning
// constant below is that cart's, put through one conversion rather than
// retuned by feel:
//
//     velocity per tick    = pico per frame  * 1.875 * (60/100) = x 1.125
//     acceleration         = pico per frame2 * 1.875 * (60/100)^2
//     a count of frames    = frames * 100/60
//     a damping factor d   = d ^ (60/100)
//
// That last one is worth stating: damping compounds, so the cart's 0.92 per
// frame at 60 Hz is 0.9509 per tick at 100 Hz, not 0.92.
//
// The field is the one thing NOT scaled by pixels, and the mockup in
// mockups/catcoin is where that was found. A coin pusher's geometry is in coin
// diameters, not pixels: what decides whether a shove reaches the lip is how
// many coins deep the shelf is. Scaled by pixels the shelf came out 11.2 coins
// deep against the cart's 4.4, and round 1 became unwinnable, with sixty
// seconds of simulation and every coin the round hands out scoring nothing.
// So the field is scaled in coins and the pixels that frees went to the panel.

#include <cstdint>

namespace cc {

constexpr int k_screen_w = 240;
constexpr int k_screen_h = 240;

constexpr int32_t k_fp = 16;
constexpr int32_t k_one = 1 << k_fp;
constexpr int32_t fx(double v) {
    return static_cast<int32_t>(v * k_one + (v < 0 ? -0.5 : 0.5));
}

// A coin is the unit of this game's geometry.
constexpr int k_coin_r = 5;
constexpr int k_coin_d = k_coin_r * 2;

// The cart's field in its own coins: 21.6 x 8.8, a 2 coin plate, 3.2 coins of
// travel, a 4.4 coin free shelf.
constexpr int k_fl = 16;
constexpr int k_fr = 224;
constexpr int k_ft = 44;
constexpr int k_fbot = k_ft + 88;                 // 8.8 coins deep
constexpr int k_fw = k_fr - k_fl;
constexpr int k_fh = k_fbot - k_ft;

constexpr int k_push_h = 2 * k_coin_d;            // 2 coins tall
constexpr int k_push_travel = 32;                 // 3.2 coins of sweep
constexpr int k_push_max = k_fbot - 44;           // leaving a 4.4 coin shelf
constexpr int k_push_min = k_push_max - k_push_travel;

// Still 0.76 s a sweep, which is what the cart's 16 px at 0.35 per frame took.
constexpr int32_t k_push_speed = fx(32.0 / 76.0);
constexpr int32_t k_disp_speed = fx(0.6 * 1.875 * 0.6);

constexpr int32_t k_coin_damp = fx(0.95093);      // 0.92 per frame at 60 Hz
constexpr int32_t k_particle_gravity = fx(0.03 * 1.875 * 0.36);

constexpr int k_fuse = 200;                       // the cart's two second fuse
constexpr int k_combo_hold = 150;
constexpr int k_mult_time = 600;
constexpr int k_crown_time = 800;
constexpr int k_ice_time = 500;
constexpr int k_drop_time = 20;
constexpr int32_t k_drop_lerp = fx(0.1927);       // the cart's 0.3 per frame
constexpr int32_t k_drop_shrink = fx(0.8385);     // the cart's 0.75 per frame

constexpr int k_num_specials = 11;
constexpr int k_max_rounds = 10;
constexpr int k_combo_threshold = 5;
// See open_spinner: a combo only ends when the lip goes quiet, so under
// sustained play the counter runs away and the cart fed it straight into the
// prize. This caps the prize only, never the scoring multiplier.
constexpr int k_spin_mult_cap = 10;
constexpr int k_inv_max = 5;

// 340 rather than the 76 a round seeds, because the clone special adds five at
// a time and a fixed array is what rule 8 asks for. 340 * 20 bytes is 6.8 KB.
constexpr int k_max_coins = 340;
constexpr int k_max_particles = 96;
constexpr int k_max_popups = 16;
constexpr int k_max_falling = 32;
constexpr int k_max_dropping = 8;

constexpr int k_buy_coin_base = 5;
constexpr int k_buy_coin_amount = 5;

// The collision grid. One cell per coin diameter (12 >= 10) so a colliding
// pair is never more than one cell apart, which is what makes checking the
// same cell plus four forward neighbours complete.
constexpr int k_cell = 12;
constexpr int k_grid_w = k_fw / k_cell + 2;
constexpr int k_grid_h = k_fh / k_cell + 2;
constexpr int k_grid_cells = k_grid_w * k_grid_h;

enum class State : uint8_t { title, play, shop, spinner, over, win };

// 0 is a plain coin. 1..11 are the cart's eleven, in its order, which is also
// the order of the cells in assets/coins.png.
enum Special : uint8_t {
    spc_none = 0, spc_bomb, spc_whirl, spc_hole, spc_magnet, spc_gold,
    spc_multi, spc_quake, spc_ice, spc_clone, spc_crown, spc_cash
};

struct Coin {
    int32_t x, y;          // Q16.16
    int32_t vx, vy;
    uint16_t fuse;
    uint8_t stype;
    uint8_t flags;         // bit 0 on the pusher, bit 1 already activated
};

constexpr uint8_t k_on_pusher = 1;
constexpr uint8_t k_activated = 2;

struct Particle {
    int32_t x, y, vx, vy;
    uint8_t life, max_life, colour;
};

struct Popup {
    int32_t x, y;
    int32_t value;
    uint16_t timer;
};

struct Falling {
    int32_t x, y, vx, vy, h, vh;
    uint8_t life;
};

struct Dropping {
    int32_t x, y, target_y, h;
    uint16_t timer;
    uint8_t stype;
};

struct ShopItem {
    uint8_t stype;
    uint16_t cost;
    bool sold;
};

struct SpinItem {
    uint8_t kind;          // 0 coins, 1 gold, 2 special
    uint16_t amount;
    uint8_t stype;
};

// Everything the player can do to the bag is a slot on one row.
//
// The cart printed three button prompts along the bottom, which rule 9
// forbids. Selection carries the meaning instead: left and right move along
// this row, one button uses what is selected, and nothing on screen names a
// key.
enum class SlotKind : uint8_t { item, buy, end };

struct Slot {
    SlotKind kind;
    uint8_t index;         // which bag slot, for SlotKind::item
};

struct Input {
    bool drop_pressed;     // A: the verb of the game, so it keeps its own button
    bool use_pressed;      // B: use whatever the row has selected
    bool left_pressed;
    bool right_pressed;
    bool up_pressed;
    bool down_pressed;
    bool any_pressed;
};

struct World {
    State state;
    uint32_t ticks;
    uint32_t rng;

    Coin coins[k_max_coins];
    uint16_t coin_count;
    Dropping dropping[k_max_dropping];
    uint8_t dropping_count;
    Falling falling[k_max_falling];
    uint8_t falling_count;
    Particle particles[k_max_particles];
    uint8_t particle_count;
    Popup popups[k_max_popups];
    uint8_t popup_count;

    int32_t push_y;
    int8_t push_dir;
    uint16_t push_frozen;

    int32_t disp_x;
    int8_t disp_dir;

    uint16_t round;
    int32_t round_score;
    int32_t target;
    int32_t gold;
    int32_t coins_left;
    int32_t score_per_gold;
    int32_t round_gold_given;

    uint8_t score_mult;
    uint16_t mult_timer;
    uint8_t combo_mult;
    uint16_t combo_buff_timer;

    uint16_t combo;
    uint16_t combo_timer;
    uint16_t combo_best;
    bool spinner_pending;

    uint8_t inv[k_inv_max];
    uint8_t inv_count;
    uint8_t sel;

    uint16_t buy_count;
    int32_t buy_cost;

    ShopItem shop[3];
    uint8_t shop_sel;
    int32_t refresh_cost;

    SpinItem spin[3];
    uint8_t spin_mult;
    int32_t spin_pos;       // Q16.16 index into spin[]
    int32_t spin_speed;
    int16_t spin_timer;
    int8_t spin_result;
    bool spin_done;

    uint16_t cat_blink;
    uint16_t cat_twitch;
    uint8_t flash;
    int32_t high_score;

    // Scratch for the collision grid. In the World so it is never allocated
    // and never a static that two games would share in a console build.
    int16_t grid_head[k_grid_cells];
    int16_t grid_next[k_max_coins];
};

uint32_t rand_next(World& world);
uint32_t rand_below(World& world, uint32_t bound);

void world_init(World& world, uint32_t seed);
void world_tick(World& world, const Input& input);

int32_t target_for(int round);
bool can_end_round(const World& world);

// The row, built fresh because it depends on the bag and on whether the round
// can be ended at all. Returns how many slots were written.
int build_slots(const World& world, Slot* out, int max_slots);

// How many coins a round seeds, and how many the shelf holds. The cart asked
// for 100 + 10r into a shelf that could not hold half that, so its sampler
// saturated and the round number barely moved the result. Converting that
// literally put 65 coins on the table where the arithmetic said 135.
int seed_capacity();
int seed_count(int round);

// Counted so the game can be budgeted against a real number rather than a
// guess. Reset it, run a tick, read it.
extern uint32_t g_pair_tests;

}  // namespace cc
