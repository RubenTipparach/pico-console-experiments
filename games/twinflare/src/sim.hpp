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
    // Which pod. Every racer has its own engine and its own cockpit now, and
    // the shapes read off the stats: the fast one is a spike, the tough one is
    // a drum, the nimble one is a flat blade. It used to be a single bit
    // choosing between two engine meshes, so the stat bars said the six pods
    // differed and the only thing a player could see was paint.
    uint8_t mesh;
    uint8_t colour[3][3];       // hull, trim, intake
    uint8_t arc[3];             // the binder between the engines
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

// What happened this tick, for the sound to read. Cleared and refilled by
// race_tick, so the audio layer holds no state about the race and the race
// holds no opinion about the audio: exactly the arrangement kingfisher uses,
// and the reason its sfx.cpp compiles against a sim that has never heard of
// the SDK.
struct Events {
    bool count;        // one of 3, 2, 1 came up
    bool go;           // green
    bool lap;          // the player finished a lap
    bool finish;       // and the last one
    bool boost;        // boost lit, however it was lit
    bool launch;       // the charge cashed in on green
    bool flood;        // or blew on the line
    bool bump;         // touched a rival
    bool scrape;       // first tick against a wall
    bool slam;         // landed hard
    bool engine_out;   // an engine reached zero
    bool wreck;

    // Held states rather than edges, because these are sounds that do not
    // start and stop, they rise and fall. A jingle cannot be an engine.
    bool grinding;     // still against the rock
    uint8_t rev;       // the engine note, 0..255, off speed and throttle
};

// Fold one tick's events into a frame's worth.
//
// A frame steps the sim up to eight times and the sound layer is called once,
// so something has to carry the seven ticks that would otherwise be thrown
// away: a lap completed on the first of eight sim ticks in a frame is a lap
// nobody hears. The edges accumulate and the LEVELS take the latest value,
// which is the difference between "did this happen during the frame" and "what
// is it doing now", and getting that backwards is a rev that flickers to
// whatever the first tick of the frame happened to be.
void merge_events(Events& into, const Events& from);

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
    bool roofed;                 // under a tunnel, so there is a ceiling
    int8_t scrape;               // which side is grinding: -1 left, +1 right
    int16_t wreck_ticks;
    int16_t flash_ticks;
    int16_t blast[2];
    // Ticks of sparks left on each engine, set by whatever last hit it. The
    // renderer used to strike sparks off a wall alone, because a wall was the
    // only damage source that said WHERE it landed; every other one took the
    // health off both engines and showed nothing at all. This is the seam that
    // lets a hit of any kind be seen on the side it happened.
    int16_t hit[2];
    int16_t bump_ticks;          // cooldown, so one touch is one hit
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
    // A rival's own race, which it did not have. It was a shape moving past the
    // camera at a plausible speed, and that is all the HUD's position number
    // ever needed. A results table needs it to have FINISHED something: which
    // lap it is on, and the tick it crossed the line for the last time.
    uint32_t finish_tick;        // 0 until it crosses
    uint8_t lap;
    uint8_t racer_index;
    uint8_t phase;
};

constexpr int k_rival_count = k_racer_count - 1;

// Where the race is. A race used to begin the instant the screen appeared,
// which is fine for a time trial and wrong for a grid: the pod was already
// moving before the player had looked at it.
enum class Phase : uint8_t {
    Countdown,   // held on the line, winding the engines up
    Racing,
    Finished,    // the player is over the line and the sim is driving
};

struct Race {
    Pod pod;
    Rival rivals[k_rival_count];
    uint8_t track_index;
    uint8_t place;
    uint32_t ticks;
    uint32_t lap_tick;           // tick the current lap started on
    uint32_t best_lap;           // ticks, 0 when none yet
    uint32_t last_lap;
    bool finished;               // the player is over the line
    Phase phase;
    int16_t countdown;           // ticks to green, then down through GO
    int16_t charge;              // the launch charge, 0..k_charge_one
    bool flooded;                // and it blew, so it cannot be rebuilt
    uint32_t finish_tick;        // the player's total, in ticks
    int16_t after_ticks;         // ticks since the player crossed
    uint8_t cam_mode;
    bool done;                   // the whole finish sequence is over
    Events ev;
};

void race_init(Race& race, int track_index, int racer_index);
void race_tick(Race& race, const Input& in);

// One racer's standing, and the whole of what the results table draws. Built
// rather than drawn straight off the arrays because the player is a Pod and the
// other five are Rivals, and a results table that reaches into both is a table
// that has to know which is which on every row.
struct Standing {
    uint8_t racer_index;
    uint32_t ticks;              // its finishing time, 0 while still out there
    uint8_t lap;
    // How far round, fp16 along the centreline from the line, which is what
    // orders the racers still out there. Lap alone ties five ways down the
    // last lap, which is the lap the table is being looked at on.
    int32_t progress;
    bool player;
    bool finished;
};

// Every racer, in finishing order: those that have crossed by their time, then
// those still running by how far round they are.
void standings(const Race& race, Standing out[k_racer_count]);

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
    bool roofed;    // a tunnel: there is a ceiling as well as a floor
    int32_t roof;   // and this is where it is
    // How far from the centreline the pod is allowed to get: the road edge on
    // a canyon, the railing everywhere else. The push in race_tick reads this
    // rather than the half width, because those are not the same line.
    int32_t limit;
};

// How high the rock stands at the road edge at a node: nothing on open desert,
// a canyon wall on a walled one, a tunnel's own height on a roofed one.
int32_t node_wall(const TrackNode& n);

// How high the ground is `over` units past the road edge, relative to the
// road's own height at that point, given the rock standing at that edge.
//
// THE ONE DESCRIPTION OF THE WORLD'S CROSS SECTION, and it is a public
// function rather than four lines inside surface_at because the renderer has
// to draw the same shape. There used to be two descriptions: the sim fell
// three units over twelve of width, the renderer fell the same three over
// three and a bit, and on a walled stretch the renderer drew a four unit kerb
// where the sim held the pod at road level. A pod off the racing line was
// drawn inside the scenery at one sample in twelve. Two answers for the same
// piece of ground, and the eye believes the one it can see.
//
// A wall is TERRAIN here, not a special case: past the road edge the ground is
// simply the top of the rock, so the same function describes a canyon rim and
// a desert shoulder. That is what lets a canyon end without a seam. `wall_h`
// is interpolated along the segment, so a wall rises out of the desert and
// sinks back into it over eight units, and the pod can drive over the low ends
// of that taper because the surface out there really is that low.
int32_t ground_offset(int32_t wall_h, int32_t over);
Surface surface_at(const Track& t, uint16_t near_node, int32_t x, int32_t z);

// Nearest centreline node, searched outward from a hint. On the device this is
// the difference between scanning 345 nodes every tick and about twenty.
uint16_t nearest_node(const Track& t, uint16_t hint, int32_t x, int32_t z);

int32_t pod_speed(const Pod& pod);            // fp16 per tick
int32_t pod_top_speed(const Pod& pod);        // fp16 per tick
bool boost_armed(const Pod& pod);
inline bool engine_dead(const Pod& pod, int i) { return (pod.dead >> i) & 1; }

// An engine low enough to smoke. Derived rather than stored, because a flag
// and a health bar are two answers to one question and only one of them is
// the one the thrust is calculated from.
inline bool engine_critical(const Pod& pod, int i) {
    if (engine_dead(pod, i)) return true;
    return pod.engine_max > 0
        && pod.engine[i] * 1000 / pod.engine_max < k_engine_critical;
}

// Which of the countdown's numbers is on screen: 3, 2, 1, or 0 for GO. Asked
// by the renderer, so the number the player sees and the number the charge is
// racing against cannot drift apart.
int countdown_number(const Race& race);

}  // namespace twinflare
