#include <cstdio>

#include "32blit.hpp"

#include "pse/blit_target.hpp"
#include "pse/game.hpp"

#include "bot.hpp"
#include "render.hpp"
#include "sfx.hpp"
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
bool g_sound_on = true;
// Which title row the cursor is on. Zero is RIDE, which is where it starts and
// where it returns, so a player who never touches the pad still rides with any
// button and this title keeps the property its comments defend, that no press
// can be the wrong guess. Only a player who has deliberately moved down can
// toggle anything.
uint8_t g_title_row = 0;
constexpr uint8_t k_title_rows = 2;

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

void save_now() {
    // update() runs outside run_split, so core 1 is parked in its RAM idle
    // loop and a flash write is safe here.
    dr::SaveData data;
    dr::world_make_save(g_world, data);
    data.sound_off = g_sound_on ? 0 : 1;
    blit::write_save(data);
    g_world.save_pending = false;
}

void save_if_needed() {
    if (!g_world.save_pending) return;
    save_now();
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

// A centred panel of centred lines, with BOTH dimensions measured.
//
// Sizing the width from the text and then hardcoding the height is only
// half a fix: the wreck card's third line printed straight through the
// bottom of its own panel the first time a run set a record. Measuring one
// axis and guessing the other is the same bug as guessing both.
void panel_lines(int top, const char* const* lines, const Pen* pens,
                 int count, Pen background) {
    constexpr int k_pad_x = 6, k_pad_y = 4, k_gap = 3;

    int content_w = 0, line_h = 0;
    for (int i = 0; i < count; i++) {
        const Size size = screen.measure_text(lines[i], minimal_font);
        if (size.w > content_w) content_w = size.w;
        if (size.h > line_h) line_h = size.h;
    }

    const int w = content_w + k_pad_x * 2;
    const int h = count * line_h + (count - 1) * k_gap + k_pad_y * 2;
    screen.pen = background;
    screen.rectangle(Rect((screen.bounds.w - w) / 2, top, w, h));

    for (int i = 0; i < count; i++) {
        text_centered(lines[i], top + k_pad_y + i * (line_h + k_gap), pens[i]);
    }
}

void draw_title() {
    char best[20];
    char ride[16];
    char sound[20];
    const char* lines[4];
    Pen pens[4];
    int count = 0;

    lines[count] = "DUST RIDER";
    pens[count++] = Pen(255, 196, 90);
    if (g_world.best_m > 0) {
        snprintf(best, sizeof(best), "best %um", g_world.best_m);
        lines[count] = best;
        pens[count++] = Pen(210, 210, 220);
    }

    // Still no control prompt: neither row says which button presses it,
    // because any of them does. What the rows carry is STATE, which a prompt
    // never had to: a setting that does not show whether it is on is a
    // setting nobody can use.
    snprintf(ride, sizeof(ride), "%sRIDE", g_title_row == 0 ? "> " : "  ");
    snprintf(sound, sizeof(sound), "%s%s", g_title_row == 1 ? "> " : "  ",
             g_sound_on ? "SOUND ON" : "SOUND OFF");
    lines[count] = ride;
    pens[count++] = g_title_row == 0 ? Pen(255, 255, 238) : Pen(150, 150, 160);
    lines[count] = sound;
    pens[count++] = g_title_row == 1 ? Pen(255, 255, 238) : Pen(150, 150, 160);

    panel_lines(38, lines, pens, count, Pen(20, 10, 8, 190));
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
    const char* lines[3] = {death_word(g_world.death), dist, "RECORD"};
    const Pen pens[3] = {Pen(255, 90, 70), Pen(255, 255, 238),
                         Pen(255, 196, 90)};
    panel_lines(48, lines, pens, g_new_record ? 3 : 2, Pen(20, 10, 8, 200));
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
    drs::sfx_silence();
    const uint32_t best = g_world.best_m;
    dr::world_init(g_world, fresh_seed());
    g_world.best_m = best;
    g_dead_ticks = 0;
    g_shell = Shell::Play;
}

void game_init() {
    pse::set_screen_mode(pse::ScreenMode::lores);

    // Every entry, not once per boot: the console calls this each time the
    // game is picked, so the shell goes back to its title and attract run
    // rather than resuming a wreck from a previous session.
    g_shell = Shell::Title;
    g_dead_ticks = 0;
    g_attract_dead = 0;
    g_new_record = false;

    dr::SaveData data;
    uint32_t seed = blit::now() ^ 0xD057D057u;
    const bool have_save = blit::read_save(data);
    if (have_save) seed ^= data.best_m * 2654435761u;
    dr::world_init(g_world, seed);
    drs::sfx_init();
    if (have_save && dr::world_load(g_world, data))
        g_sound_on = dr::save_sound_on(data);
    drs::sfx_set_enabled(g_sound_on);
    // The title runs an attract bike, and an attract bike revving at the
    // player from the menu would be the loudest thing in the room. The engine
    // stays quiet until a run actually starts: game_update never hands the
    // title's ticks to sfx_handle.
    drs::sfx_silence();
    g_title_row = 0;
}

void game_update(uint32_t time) {
    if (g_shell == Shell::Title) {
        // The desert rides itself behind the title.
        dr::world_tick(g_world, dr::bot_input(g_world));
        if (!g_world.alive && ++g_attract_dead > 90) {
            const uint32_t best = g_world.best_m;
            dr::world_init(g_world, fresh_seed());
            g_world.best_m = best;
            g_attract_dead = 0;
        }
        if (buttons.pressed & Button::DPAD_UP)
            g_title_row = static_cast<uint8_t>((g_title_row + k_title_rows - 1)
                                               % k_title_rows);
        if (buttons.pressed & Button::DPAD_DOWN)
            g_title_row = static_cast<uint8_t>((g_title_row + 1) % k_title_rows);
        // Up and down are the cursor, so they are the only two presses that do
        // not act. Everything else acts on the row the cursor is on, and the
        // cursor starts on RIDE.
        constexpr uint32_t k_act =
            k_any_button & ~(Button::DPAD_UP | Button::DPAD_DOWN);
        if (buttons.pressed & k_act) {
            if (g_title_row == 1) {
                g_sound_on = !g_sound_on;
                drs::sfx_set_enabled(g_sound_on);
                save_now();
            } else {
                start_run();
            }
        }
        drs::sfx_tick();
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

    // One frame is one sound. This shell steps the sim exactly once per
    // update, so the merge is a copy today, but it goes through the same door
    // the multi step games use: a shell that later catches up several ticks
    // would otherwise silently drop every cue but the last.
    dr::Events heard{};
    dr::merge_events(heard, g_world.ev);
    drs::sfx_handle(heard);
    drs::sfx_tick();

    if (!g_world.alive) {
        g_dead_ticks++;
        save_if_needed();
        // The grace period is so a wreck is not skipped by the button that
        // was already held when it happened.
        if (g_dead_ticks > 40 && (buttons.pressed & k_any_button)) start_run();
    }
}

void game_render(uint32_t time) {
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

}  // namespace

// The one symbol this game exports. Everything above is internal linkage, so
// the console can link it beside three other games that also have a g_world.
PSE_GAME(dustrider, game_init, game_update, game_render);
