#pragma once

// Dumb Lander: the rules, with no renderer and no SDK behind them.
//
// A demake of dumb_lander.p8, which ran at 128x128 and 30 Hz. Every tuning
// constant below is that cart's, put through one conversion rather than
// retuned by feel:
//
//     position or velocity per tick = pico per frame  * 1.875 * (30/100)
//     acceleration per tick squared = pico per frame2 * 1.875 * (30/100)^2
//
// 1.875 is 240/128 and 30/100 is the cart's frame against this device's tick,
// which is 10 ms. The mockup in mockups/dumblander flew all of this before any
// of it was written.
//
// Not to be confused with games/tomlander, which is the 3D lander with four
// independently fired pods. Different game.
//
// Everything here is integer. Positions and velocities are Q16.16 because a
// lander drifting at the crash threshold moves 0.56 px a tick, and a whole
// pixel of quantisation there is the difference between a landing and a wreck.

#include <cstdint>

namespace dl {

constexpr int k_screen_w = 240;
constexpr int k_screen_h = 240;

// Q16.16 throughout. 65536 is one pixel.
constexpr int32_t k_fp = 16;
constexpr int32_t k_one = 1 << k_fp;

constexpr int32_t fx(double v) { return static_cast<int32_t>(v * k_one + (v < 0 ? -0.5 : 0.5)); }

// The cart's four numbers, converted. The comment on each is what it was.
constexpr int32_t k_gravity = fx(0.1 * 1.875 * 0.3 * 0.3);      // 0.1  per frame2
constexpr int32_t k_thrust  = fx(-0.2 * 1.875 * 0.3 * 0.3);     // -0.2 per frame2
constexpr int32_t k_side    = fx(0.05 * 1.875 * 0.3 * 0.3);     // 0.05 per frame2
constexpr int32_t k_safe    = fx(1.0 * 1.875 * 0.3);            // 1.0  per frame

// The tank is the cart's 100 units. What changed is how fast it drains: the
// cart's rate was 3.3 seconds of burn, which was almost exactly one crossing
// of a 128 px screen, and the crossing is 1.875x longer here. Flying 24
// generated legs on the mockup, the cart's rate landed 3 of 6 with an empty
// tank every time and this rate landed 24 of 24 with about half in hand.
constexpr int32_t k_tank = 100 * k_one;
constexpr int32_t k_burn = fx(0.3 / 1.875);

constexpr int k_pad_w = 30;              // the cart's 16 px pad, scaled
constexpr int k_hull_w = 15;             // the cart's 8 px hull, scaled
constexpr int k_hull_h = 18;
constexpr int k_ceiling = 8;             // the cart had none, see world_tick

constexpr int k_max_rocks = 34;
constexpr int k_max_stars = 60;
constexpr int k_max_flame = 24;
constexpr int k_max_debris = 26;

constexpr int k_landed_hold = 90;        // ticks on the pad before the next leg
constexpr int k_refuel = 55 * k_one;     // topped up per leg, capped at the tank
constexpr int k_strand_hold = 60;        // ticks at rest and dry before it ends
constexpr int k_restart_hold = 40;       // ticks before a button restarts a run

enum class State : uint8_t { title, fly, landed, over };
enum class Ending : uint8_t { none, crashed, stranded };

struct Pad {
    int16_t x;
    int16_t w;
    uint16_t y;                          // Q8.8 pixels, as the terrain is
};

struct Rock {
    uint8_t x;
    uint16_t y;                          // Q8.8
    uint8_t kind;                        // which cell of the rock sheet
};

struct Star {
    uint8_t x;
    uint8_t y;
    uint8_t bright;
};

struct Particle {
    int32_t x, y, vx, vy;                // Q16.16
    uint8_t life;
    uint8_t colour;
};

// What the player is holding this tick. The game reads the SDK for this and
// nothing else in here knows the SDK exists.
struct Input {
    bool thrust;
    bool left;
    bool right;
    bool any_pressed;                    // edge, for start and restart
};

struct World {
    State state;
    Ending ending;

    int32_t x, y;                        // Q16.16, y is where the feet touch
    int32_t vx, vy;
    int32_t fuel;
    int32_t speed;                       // recomputed each tick, for the HUD

    uint16_t terrain[k_screen_w];        // Q8.8 pixels, one per column
    Pad start;
    Pad goal;

    Rock rocks[k_max_rocks];
    uint8_t rock_count;
    Star stars[k_max_stars];

    Particle flame[k_max_flame];
    uint8_t flame_count;
    Particle debris[k_max_debris];
    uint8_t debris_count;

    uint16_t leg;                        // 1 based: the leg being flown now
    bool took_off;                       // the cart's latch, so frame one is not a crash
    bool thrusting;                      // for the renderer, set by the tick
    int8_t jet;                          // -1, 0, 1: which way the side jets fire

    uint16_t hold;                       // ticks in the current non flying state
    uint16_t rest_ticks;
    uint8_t shake;

    uint32_t rng;
    uint32_t ticks;
};

// Deterministic, so a preview frame is the same frame every run and a test can
// name a seed. Same generator the mockup used.
uint32_t rand_next(World& world);
uint32_t rand_below(World& world, uint32_t bound);

void world_init(World& world, uint32_t seed);
void world_tick(World& world, const Input& input);

// True when the hull's whole footprint is inside the gold deck.
//
// The cart tested `x >= goal.x and x + 8 <= goal.x + 16` with x as the hull's
// centre, which actually demanded the centre sit in the left half of the pad
// and made the right half not count. This is the same difficulty with no dead
// half.
bool on_goal(const World& world);

// Ground height under a column, in Q8.8. Clamped, so a caller never reads off
// the end of the array.
uint16_t ground_at(const World& world, int column);

}  // namespace dl
