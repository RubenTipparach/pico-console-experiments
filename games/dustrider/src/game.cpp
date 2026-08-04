#include <cstdio>

#include "32blit.hpp"

#include "pse/blit_target.hpp"

#include "bot.hpp"
#include "render.hpp"
#include "sim.hpp"

using namespace blit;

namespace {

// The shell around the sim: title over an attract run, then the run itself,
// with the wreck report drawn over the frozen world.
enum class Shell : uint8_t { Title, Play };

dr::World g_world;
Shell g_shell = Shell::Title;
uint32_t g_dead_ticks = 0;
uint32_t g_attract_dead = 0;

// The sim's RAM footprint is a promise, checked at compile time.
static_assert(sizeof(dr::World) <= 1024, "sim state grew past its RAM budget");
static_assert(sizeof(dr::SaveData) <= 16, "save record grew");

uint32_t fresh_seed() {
    return blit::now() ^ (g_world.rng * 2654435761u) ^ 0xD057D057u;
}

void save_if_needed() {
    // update() runs outside run_split, so core 1 is parked in its RAM idle
    // loop and a flash write is safe here.
    if (!g_world.save_pending) return;
    dr::SaveData data;
    dr::world_make_save(g_world, data);
    blit::write_save(data);
    g_world.save_pending = false;
}

void draw_title() {
    screen.pen = Pen(20, 10, 8, 190);
    screen.rectangle(Rect(12, 30, 96, 50));
    screen.pen = Pen(255, 196, 90);
    screen.text("DUST RIDER", minimal_font, Point(34, 38));
    screen.pen = Pen(255, 255, 238);
    screen.text("A: ride", minimal_font, Point(46, 54));
    screen.pen = Pen(190, 170, 150);
    screen.text("B brake  pad: steer", minimal_font, Point(22, 66));

    if (g_world.best_m > 0) {
        char line[20];
        snprintf(line, sizeof(line), "best %um", g_world.best_m);
        screen.pen = Pen(210, 210, 220);
        screen.text(line, minimal_font, Point(40, 108));
    }
}

const char* death_word(dr::Death death) {
    switch (death) {
        case dr::Death::Cactus: return "CACTUS";
        case dr::Death::Rail: return "RAIL";
        case dr::Death::Behind: return "TOO SLOW";
        case dr::Death::Ahead: return "TOO FAST";
        default: return "";
    }
}

void draw_wreck() {
    char line[24];
    screen.pen = Pen(20, 10, 8, 200);
    screen.rectangle(Rect(14, 34, 92, 46));
    screen.pen = Pen(255, 90, 70);
    screen.text(death_word(g_world.death), minimal_font, Point(42, 40));
    snprintf(line, sizeof(line), "%dm", dr::distance_m(g_world));
    screen.pen = Pen(255, 255, 238);
    screen.text(line, minimal_font, Point(52, 52));
    snprintf(line, sizeof(line), "best %um", g_world.best_m);
    screen.pen = Pen(210, 210, 220);
    screen.text(line, minimal_font, Point(40, 62));
    if (g_dead_ticks > 40) {
        screen.pen = Pen(255, 196, 90);
        screen.text("A: again", minimal_font, Point(44, 71));
    }
}

void draw_hud() {
    char line[16];
    snprintf(line, sizeof(line), "%dm", dr::distance_m(g_world));
    screen.pen = Pen(40, 24, 16, 160);
    screen.rectangle(Rect(86, 2, 32, 9));
    screen.pen = Pen(255, 255, 238);
    screen.text(line, minimal_font, Point(88, 3));
}

void start_run() {
    const uint32_t best = g_world.best_m;
    dr::world_init(g_world, fresh_seed());
    g_world.best_m = best;
    g_dead_ticks = 0;
    g_shell = Shell::Play;
}

}  // namespace

void init() {
    set_screen_mode(ScreenMode::lores);

    dr::SaveData data;
    uint32_t seed = blit::now() ^ 0xD057D057u;
    const bool have_save = blit::read_save(data);
    if (have_save) seed ^= data.best_m * 2654435761u;
    dr::world_init(g_world, seed);
    if (have_save) dr::world_load(g_world, data);
}

void update(uint32_t time) {
    if (g_shell == Shell::Title) {
        // The desert rides itself behind the title.
        dr::world_tick(g_world, dr::bot_input(g_world));
        if (!g_world.alive && ++g_attract_dead > 90) {
            const uint32_t best = g_world.best_m;
            dr::world_init(g_world, fresh_seed());
            g_world.best_m = best;
            g_attract_dead = 0;
        }
        if (buttons.pressed & Button::A) start_run();
        return;
    }

    dr::Input input;
    input.throttle = (buttons & Button::A) != 0;
    input.brake = (buttons & Button::B) != 0;
    // Steering is held, not tapped: the road curves continuously and the
    // rider holds a line through it. Up is north, away from the camera.
    input.north = (buttons & Button::DPAD_UP) != 0;
    input.south = (buttons & Button::DPAD_DOWN) != 0;

    dr::world_tick(g_world, input);

    if (!g_world.alive) {
        g_dead_ticks++;
        save_if_needed();
        if (g_dead_ticks > 40 && (buttons.pressed & Button::A)) start_run();
    }
}

void render(uint32_t time) {
    drr::render_scene(g_world, pse::target_from_screen(), time);

    if (g_shell == Shell::Title) {
        draw_title();
        return;
    }
    if (!g_world.alive) {
        draw_wreck();
        return;
    }
    draw_hud();
}
