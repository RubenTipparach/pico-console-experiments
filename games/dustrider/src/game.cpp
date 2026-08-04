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
bool g_new_record = false;

// Any button acts. With nothing on screen telling the player which one to
// press, no press can be the wrong guess.
constexpr uint32_t k_any_button =
    Button::A | Button::B | Button::X | Button::Y |
    Button::DPAD_UP | Button::DPAD_DOWN | Button::DPAD_LEFT |
    Button::DPAD_RIGHT;

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

// Centre one line on the screen. Every string here is measured rather than
// placed by eye: a hand picked x is only correct for the exact string it
// was tuned against, which is how the old title ended up printing its
// control hints straight through the edges of their own panel.
void text_centered(const char* line, int y, Pen pen) {
    const Size size = screen.measure_text(line, minimal_font);
    screen.pen = pen;
    screen.text(line, minimal_font, Point((screen.bounds.w - size.w) / 2, y));
}

// A panel sized to its widest measured line, centred, with a small margin.
void panel(int y, int height, int content_w, Pen pen) {
    const int w = content_w + 12;
    screen.pen = pen;
    screen.rectangle(Rect((screen.bounds.w - w) / 2, y, w, height));
}

int widest(const char* const* lines, int count) {
    int w = 0;
    for (int i = 0; i < count; i++) {
        const int lw = screen.measure_text(lines[i], minimal_font).w;
        if (lw > w) w = lw;
    }
    return w;
}

void draw_title() {
    char best[20];
    const char* lines[2] = {"DUST RIDER", nullptr};
    int count = 1;
    if (g_world.best_m > 0) {
        snprintf(best, sizeof(best), "best %um", g_world.best_m);
        lines[1] = best;
        count = 2;
    }

    // No control prompts. Any button rides, so there is nothing a prompt
    // could usefully say, and the gallery card already lists the controls.
    const int height = count == 2 ? 30 : 20;
    panel(44, height, widest(lines, count), Pen(20, 10, 8, 190));
    text_centered("DUST RIDER", 50, Pen(255, 196, 90));
    if (count == 2) text_centered(best, 62, Pen(210, 210, 220));
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

// State, score, and a record tag when there is one. No retry prompt: any
// button rides again, and a line telling the player that is a line they
// have to read every single death.
void draw_wreck() {
    char dist[16];
    snprintf(dist, sizeof(dist), "%dm", dr::distance_m(g_world));
    const char* cause = death_word(g_world.death);
    const char* lines[3] = {cause, dist, "RECORD"};
    const int count = g_new_record ? 3 : 2;

    panel(46, count == 3 ? 32 : 22, widest(lines, count), Pen(20, 10, 8, 200));
    text_centered(cause, 51, Pen(255, 90, 70));
    text_centered(dist, 62, Pen(255, 255, 238));
    if (g_new_record) text_centered("RECORD", 73, Pen(255, 196, 90));
}

// The only thing on screen while riding: how far. Sized to the text so a
// four digit distance cannot run out of its own backing.
void draw_hud() {
    char line[16];
    snprintf(line, sizeof(line), "%dm", dr::distance_m(g_world));
    const Size size = screen.measure_text(line, minimal_font);
    const int x = screen.bounds.w - size.w - 4;
    screen.pen = Pen(40, 24, 16, 160);
    screen.rectangle(Rect(x - 2, 2, size.w + 4, size.h + 2));
    screen.pen = Pen(255, 255, 238);
    screen.text(line, minimal_font, Point(x, 3));
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
        if (buttons.pressed & k_any_button) start_run();
        return;
    }

    dr::Input input;
    input.throttle = (buttons & Button::A) != 0;
    input.brake = (buttons & Button::B) != 0;
    // Steering is held, not tapped: the road curves continuously and the
    // rider holds a line through it. Up is north, away from the camera.
    input.north = (buttons & Button::DPAD_UP) != 0;
    input.south = (buttons & Button::DPAD_DOWN) != 0;

    const uint32_t prev_best = g_world.best_m;
    dr::world_tick(g_world, input);
    if (g_world.ev.died) g_new_record = g_world.best_m > prev_best;

    if (!g_world.alive) {
        g_dead_ticks++;
        save_if_needed();
        // The grace period is so a wreck is not skipped by the button that
        // was already held when it happened.
        if (g_dead_ticks > 40 && (buttons.pressed & k_any_button)) start_run();
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
