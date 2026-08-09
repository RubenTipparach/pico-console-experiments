#pragma once

#include <cstdint>

#include "tracks.hpp"
#include "tuning.hpp"

// The race, as pure integer state. No SDK, no float, no allocation, and no
// drawing: this is the half of the game the host tests can run, and keeping
// the line here is easier than introducing it later.

namespace twinflare {

// Six pods. Twenty stat points each across six stats, so no pod is an upgrade
// on another and the choice is a shape rather than a ladder. All six share two
// engine meshes, recoloured, which is the only version of six racers that fits
// in flash.
struct Racer {
    const char* name;
    const char* pilot;
    uint8_t top, acc, grip, cool, fix, hull;
    uint8_t heavy;              // which of the two engine meshes
    uint8_t colour[3][3];       // hull, trim, intake
};

constexpr int k_racer_count = 6;
const Racer& racer(int index);

struct Input {
    bool throttle;
    bool brake;
    bool repair;
    bool left, right, up, down;
    // A fresh press of the boost button this tick. The sim owns the double tap
    // window rather than the SDK layer, so the timing is in the tested half.
    bool boost_press;
};

// One pod under the flight model. The player's; the rivals run something far
// cheaper, below.
struct Pod {
    int32_t x, y, z;             // fp16 world units
    int32_t vx, vy, vz;          // fp16 world units per tick
    int32_t yaw, pitch, roll;    // brads
    int32_t yaw_rate;            // Q8 brads per tick, see k_rate_fp
    int32_t pitch_rate;
    int32_t swing, swing_rate;   // the cockpit on its cables

    int16_t engine[2];
    int16_t engine_max;
    uint8_t dead;                // bit 0 left, bit 1 right
    int32_t heat;
    int16_t boost_ticks;
    int16_t tap_age;             // ticks since the last throttle press
    bool locked;                 // vented, boost out until the heat drops

    uint16_t node;               // nearest centreline node
    uint8_t lap;
    int32_t clearance;           // above the surface, fp16
    int32_t lateral;             // signed offset from the centreline, fp16
    bool grounded;
    bool scraping;
    bool on_road;
    bool over_water;             // the surface under the field is the sea
    int16_t wreck_ticks;
    int16_t flash_ticks;
    int16_t blast[2];
    uint8_t racer_index;
};

// A rival is NOT running the flight model. A rival that can crash can be
// lapped by its own physics, and none of that is visible from behind; what a
// rival has to do is be somewhere plausible at a plausible speed so the
// position on the HUD means something.
struct Rival {
    int32_t distance;            // fp16 along the centreline
    int32_t x, y, z;
    int32_t yaw, roll;
    int32_t pace;
    uint8_t racer_index;
    uint8_t phase;
};

constexpr int k_rival_count = k_racer_count - 1;

struct Race {
    Pod pod;
    Rival rivals[k_rival_count];
    uint8_t track_index;
    uint8_t place;
    uint32_t ticks;
    uint32_t lap_tick;           // tick the current lap started on
    uint32_t best_lap;           // ticks, 0 when none yet
    uint32_t last_lap;
    bool finished;
};

void race_init(Race& race, int track_index, int racer_index);
void race_tick(Race& race, const Input& in);

// The surface under a point: where the road is, how far off its middle, and
// whether there is any road there at all. One function, so the hover field,
// the wall test and the fall all read the same ground.
struct Surface {
    int32_t y;
    int32_t lateral;
    uint16_t node;
    bool road;
    bool wall;
    bool water;     // the sea won: the field is pushing off water, not rock
};
Surface surface_at(const Track& t, uint16_t near_node, int32_t x, int32_t z);

// Nearest centreline node, searched outward from a hint. On the device this is
// the difference between scanning 345 nodes every tick and about twenty.
uint16_t nearest_node(const Track& t, uint16_t hint, int32_t x, int32_t z);

int32_t pod_speed(const Pod& pod);            // fp16 per tick
int32_t pod_top_speed(const Pod& pod);        // fp16 per tick
bool boost_armed(const Pod& pod);
inline bool engine_dead(const Pod& pod, int i) { return (pod.dead >> i) & 1; }

}  // namespace twinflare
