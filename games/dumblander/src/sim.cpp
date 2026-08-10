#include "sim.hpp"

#include <cmath>

namespace dl {
namespace {

// Integer square root, for the speed readout and the landing test. No floats
// in a per tick path on a core with no FPU.
uint32_t isqrt(uint64_t value) {
    if (value == 0) return 0;
    uint64_t guess = value;
    uint64_t next = (guess + 1) / 2;
    while (next < guess) {
        guess = next;
        next = (guess + value / guess) / 2;
    }
    return static_cast<uint32_t>(guess);
}

int32_t clamp32(int32_t v, int32_t lo, int32_t hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

bool near_pad(const World& world, int x) {
    const Pad* pads[2] = {&world.start, &world.goal};
    for (const Pad* pad : pads) {
        if (x >= pad->x - 8 && x <= pad->x + pad->w + 8) return true;
    }
    return false;
}

void flatten(World& world, Pad& pad) {
    const uint16_t height = world.terrain[clamp32(pad.x, 0, k_screen_w - 1)];
    pad.y = height;
    for (int i = pad.x; i < pad.x + pad.w; i++) {
        if (i >= 0 && i < k_screen_w) world.terrain[i] = height;
    }
}

// Terrain generation, which is the one place floats are allowed.
//
// It runs once a leg over 240 columns, not per tick and not per pixel, so the
// software float cost is a fraction of a millisecond in a place nothing is
// waiting on. Rule 8 asks for fixed point in what runs per pixel or per
// vertex; this is neither, and a sine table accurate enough for the cart's two
// bands would be more code than the thing it saves.
void generate(World& world) {
    // The cart's three phase offsets, scaled with the screen so the ridge
    // keeps its shape rather than repeating 1.875 times as often.
    const float x1 = static_cast<float>(rand_below(world, 300 * 1875)) / 1000.0f;
    const float x2 = static_cast<float>(rand_below(world, 200 * 1875)) / 1000.0f;
    constexpr float k_tau = 6.28318530718f;

    for (int x = 0; x < k_screen_w; x++) {
        // PICO-8's cos takes turns, not radians, which is why these divisors
        // look like periods: (x + phase) / period is a fraction of a turn.
        const float big = std::cos(k_tau * (x + x1) / (200.0f * 1.875f)) * (30.0f * 1.875f);
        const float med = std::sin(k_tau * (x + x2) / (50.0f * 1.875f)) * (4.0f * 1.875f);
        const float jitter = static_cast<float>(rand_below(world, 3 * 1875)) / 1000.0f;
        float h = 95.0f * 1.875f + big;
        const float floor_h = 110.0f * 1.875f;
        if (h > floor_h) h = floor_h;            // the cart's min(95 + big, 110)
        h += med + jitter;
        if (h < 20.0f) h = 20.0f;
        if (h > k_screen_h - 2.0f) h = k_screen_h - 2.0f;
        world.terrain[x] = static_cast<uint16_t>(h * 256.0f);
    }

    // Pads. The goal walks further out and narrows as the run goes on, which
    // is the only difficulty curve this game has.
    const int step = world.leg - 1 > 6 ? 6 : world.leg - 1;
    world.start.x = static_cast<int16_t>(rand_below(world, 56) + 19);
    world.start.w = k_pad_w;
    world.goal.w = static_cast<int16_t>(k_pad_w - step * 2 < 18 ? 18 : k_pad_w - step * 2);
    world.goal.x = static_cast<int16_t>(rand_below(world, 56 - step * 4 > 8 ? 56 - step * 4 : 8) +
                                        150 + step * 2);
    if (world.goal.x + world.goal.w > k_screen_w - 4) {
        world.goal.x = static_cast<int16_t>(k_screen_w - 4 - world.goal.w);
    }
    flatten(world, world.start);
    flatten(world, world.goal);

    // Rocks. The cart picked each rock's colour with rnd inside _draw, so all
    // thirty strobed every frame; the kind is chosen once, here.
    world.rock_count = 0;
    for (int i = 0; i < k_max_rocks; i++) {
        int x = 0;
        int tries = 0;
        do {
            x = static_cast<int>(rand_below(world, k_screen_w));
            tries++;
        } while (tries < 40 && near_pad(world, x));
        if (near_pad(world, x)) continue;
        Rock& rock = world.rocks[world.rock_count++];
        rock.x = static_cast<uint8_t>(x);
        rock.y = static_cast<uint16_t>(world.terrain[x] + (4 + rand_below(world, 7)) * 256);
        rock.kind = static_cast<uint8_t>(rand_below(world, 3));
    }
}

void place_on_start(World& world) {
    world.x = (world.start.x + world.start.w / 2) * k_one;
    world.y = static_cast<int32_t>(world.start.y) << 8;   // Q8.8 into Q16.16
    world.vx = 0;
    world.vy = 0;
    world.speed = 0;
    world.took_off = false;
    world.thrusting = false;
    world.jet = 0;
    world.rest_ticks = 0;
    world.flame_count = 0;
    world.debris_count = 0;
}

void begin_run(World& world) {
    world.leg = 1;
    world.fuel = k_tank;
    world.ending = Ending::none;
    generate(world);
    place_on_start(world);
    world.state = State::fly;
    world.hold = 0;
}

void wreck(World& world) {
    world.state = State::over;
    world.ending = Ending::crashed;
    world.hold = 0;
    world.shake = 14;
    world.debris_count = 0;
    for (int i = 0; i < k_max_debris; i++) {
        Particle& p = world.debris[world.debris_count++];
        p.x = world.x;
        p.y = world.y - 6 * k_one;
        // A cheap spread: the generator is good enough for debris and a sine
        // table for 26 particles would not earn its flash.
        p.vx = static_cast<int32_t>(rand_below(world, 2 * k_one)) - k_one;
        p.vy = static_cast<int32_t>(rand_below(world, 2 * k_one)) - k_one - k_one / 2;
        p.life = static_cast<uint8_t>(30 + rand_below(world, 40));
        static const uint8_t colours[5] = {0, 1, 2, 3, 4};
        p.colour = colours[rand_below(world, 5)];
    }
}

void step_particles(Particle* list, uint8_t& count, int32_t gravity) {
    for (int i = static_cast<int>(count) - 1; i >= 0; i--) {
        Particle& p = list[i];
        p.x += p.vx;
        p.y += p.vy;
        p.vy += gravity;
        if (p.life > 0) p.life--;
        if (p.life == 0) {
            list[i] = list[count - 1];
            count--;
        }
    }
}

}  // namespace

uint32_t rand_next(World& world) {
    world.rng = world.rng * 1664525u + 1013904223u;
    return world.rng;
}

uint32_t rand_below(World& world, uint32_t bound) {
    if (bound == 0) return 0;
    return (rand_next(world) >> 8) % bound;
}

uint16_t ground_at(const World& world, int column) {
    if (column < 0) column = 0;
    if (column >= k_screen_w) column = k_screen_w - 1;
    return world.terrain[column];
}

bool on_goal(const World& world) {
    const int32_t left = world.x - (k_hull_w / 2) * k_one;
    const int32_t right = world.x + (k_hull_w / 2) * k_one;
    return left >= world.goal.x * k_one &&
           right <= (world.goal.x + world.goal.w) * k_one;
}

void world_init(World& world, uint32_t seed) {
    world.rng = seed ? seed : 1u;
    world.ticks = 0;
    world.leg = 1;
    world.fuel = k_tank;
    world.shake = 0;
    world.hold = 0;
    world.ending = Ending::none;
    world.flame_count = 0;
    world.debris_count = 0;

    for (int i = 0; i < k_max_stars; i++) {
        world.stars[i].x = static_cast<uint8_t>(rand_below(world, k_screen_w));
        world.stars[i].y = static_cast<uint8_t>(rand_below(world, 150));
        world.stars[i].bright = static_cast<uint8_t>(rand_below(world, 4) == 0 ? 1 : 0);
    }
    generate(world);
    place_on_start(world);
    world.state = State::title;
}

void world_tick(World& world, const Input& input) {
    world.ticks++;
    if (world.shake > 0) world.shake--;

    step_particles(world.flame, world.flame_count, 0);
    step_particles(world.debris, world.debris_count, k_gravity * 3);

    if (world.state == State::title) {
        if (input.any_pressed) begin_run(world);
        return;
    }

    if (world.state == State::landed) {
        // A beat on the pad, then the next leg.
        if (world.hold > 0) world.hold--;
        if (world.hold == 0) {
            world.leg++;
            world.fuel += k_refuel;
            if (world.fuel > k_tank) world.fuel = k_tank;
            generate(world);
            place_on_start(world);
            world.state = State::fly;
        }
        return;
    }

    if (world.state == State::over) {
        if (world.hold < 0xFFFF) world.hold++;
        if (world.hold > k_restart_hold && input.any_pressed) begin_run(world);
        return;
    }

    // --- flying --------------------------------------------------------
    world.thrusting = false;
    world.jet = 0;

    world.vy += k_gravity;
    if (input.thrust && world.fuel > 0) {
        world.vy += k_thrust;
        world.fuel -= k_burn;
        if (world.fuel < 0) world.fuel = 0;
        world.thrusting = true;
        // The cart's latch: without it the opening frame on the pad counts as
        // an impact and the run ends before it starts.
        if (!world.took_off && (world.vy < -k_one / 200 ||
                                world.vx > k_one / 200 || world.vx < -k_one / 200)) {
            world.took_off = true;
        }
    }
    if (input.left) {
        world.vx -= k_side;
        world.jet = -1;
    }
    if (input.right) {
        world.vx += k_side;
        world.jet = 1;
    }

    world.x += world.vx;
    world.y += world.vy;

    // The cart clamped x to 0..120 of 128, which is the hull's own width off
    // the right edge. The same fraction here.
    const int32_t x_min = (k_hull_w / 2) * k_one;
    const int32_t x_max = (k_screen_w - k_hull_w / 2) * k_one;
    if (world.x < x_min) {
        world.x = x_min;
        world.vx = 0;
    }
    if (world.x > x_max) {
        world.x = x_max;
        world.vx = 0;
    }
    // The cart let the lander leave the top of the screen and come back. A
    // lander you cannot see is a lander you cannot fly.
    if (world.y < k_ceiling * k_one) {
        world.y = k_ceiling * k_one;
        if (world.vy < 0) world.vy = 0;
    }

    {
        const int64_t vx = world.vx;
        const int64_t vy = world.vy;
        world.speed = static_cast<int32_t>(isqrt(static_cast<uint64_t>(vx * vx + vy * vy)));
    }

    if (world.thrusting && (world.ticks % 2) == 0 && world.flame_count < k_max_flame) {
        Particle& p = world.flame[world.flame_count++];
        p.x = world.x + static_cast<int32_t>(rand_below(world, 4 * k_one)) - 2 * k_one;
        p.y = world.y - 2 * k_one;
        p.vx = (static_cast<int32_t>(rand_below(world, k_one)) - k_one / 2) / 2 - world.vx / 2;
        p.vy = k_one + static_cast<int32_t>(rand_below(world, k_one)) / 2;
        p.life = static_cast<uint8_t>(7 + rand_below(world, 6));
        p.colour = 0;
    }

    const int32_t ground = static_cast<int32_t>(ground_at(world, world.x >> k_fp)) << 8;
    if (world.y >= ground) {
        world.y = ground;
        if (world.speed <= k_safe && on_goal(world)) {
            world.state = State::landed;
            world.hold = k_landed_hold;
        } else if (world.took_off && world.speed > k_safe) {
            wreck(world);
        } else {
            // Set down softly somewhere that is not the deck. Fine, and you
            // can take off again, unless there is nothing left to take off
            // with: the cart let you sit there for ever.
            if (world.rest_ticks < 0xFFFF) world.rest_ticks++;
            if (world.fuel <= 0 && world.rest_ticks > k_strand_hold) {
                world.state = State::over;
                world.ending = Ending::stranded;
                world.hold = 0;
            }
        }
        world.vx = 0;
        world.vy = 0;
    } else {
        world.rest_ticks = 0;
    }
}

}  // namespace dl
