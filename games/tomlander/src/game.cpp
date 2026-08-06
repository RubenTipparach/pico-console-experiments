#include <cstdio>

#include "32blit.hpp"

#include "pse/blit_target.hpp"
#include "pse/game.hpp"

#include "render.hpp"
#include "sim.hpp"

using namespace blit;

namespace {

// The shell around the flight: a title over the ship sitting on its pad, the
// flight itself, and the card that closes it.
enum class Shell : uint8_t { Title, Play, Paused };

tl::World g_world;
Shell g_shell = Shell::Title;
float g_cam_yaw = 0.0f;
uint8_t g_pause_item = 0;
uint32_t g_best_fuel = 0;      // fp8 fuel left on the best landing
bool g_new_record = false;
bool g_save_pending = false;

constexpr uint32_t k_any_face =
    Button::A | Button::B | Button::X | Button::Y;

// Radians of camera per tick. Left and right turn the view, never the hull.
constexpr float k_cam_rate = 0.028f;

// The sim runs on ticks, not on wall clock, so a slow frame costs frames and
// never changes the physics.
constexpr uint32_t k_tick_ms = 10;
uint32_t g_tick_accumulator = 0;
uint32_t g_last_time = 0;

struct SaveData {
    uint32_t magic;
    uint32_t best_fuel;
    uint8_t reserved[8];
};
constexpr uint32_t k_save_magic = 0x314C4D54u;   // 'T','M','L','1'

static_assert(sizeof(tl::World) <= 256, "sim state grew past its RAM budget");
static_assert(sizeof(SaveData) <= 16, "save record grew");

// Which face button works which pod.
//
// The diamond IS the ship seen from above: X is the top button and works the
// front pod, B the bottom one and the back pod, Y the left, A the right. A
// pod lifts its own corner, so the ship always travels away from the one that
// is lit. One rule, four buttons, no per axis exception to remember.
//
// This is a deliberate divergence from tom-lander, where keyboard A works the
// RIGHT thruster so that pressing left takes you left. Both readings are
// defensible, which is why the original ships an invert setting for the pair.
// With four digital buttons and no stick, the pad doubling as a picture of
// the hull is worth more than matching travel direction on one axis.
constexpr uint32_t k_pod_button[tl::kPodCount] = {
    Button::A,   // right pod
    Button::Y,   // left pod
    Button::X,   // front pod
    Button::B,   // back pod
};

void save_if_needed() {
    // update() runs outside run_split, so core 1 is parked in its RAM idle
    // loop and a flash write is safe here.
    if (!g_save_pending) return;
    SaveData data{};
    data.magic = k_save_magic;
    data.best_fuel = g_best_fuel;
    write_save(data);
    g_save_pending = false;
}

void start_flight() {
    tl::world_init(g_world);
    g_cam_yaw = 0.0f;
    g_new_record = false;
    g_shell = Shell::Play;
}

// Centre one line. Every string is measured rather than placed by eye: a hand
// picked x is only correct for the exact string it was tuned against.
void text_centered(const char* line, int y, Pen pen) {
    const Size size = screen.measure_text(line, minimal_font);
    screen.pen = pen;
    screen.text(line, minimal_font, Point((screen.bounds.w - size.w) / 2, y));
}

// A centred panel of centred lines, with BOTH dimensions measured.
void panel_lines(int top, const char* const* lines, const Pen* pens, int count,
                 Pen background) {
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

// The in flight HUD. Rule 9: the minimum, and no button prompts.
//   fuel  a bar, because a number you have to read is a number you do not
//   fall  descent rate, red once it would break the ship
//   alt   how far above whatever is underneath
//   B nn  which deck, and how far, in the colour the deck itself is
void draw_hud() {
    const int w = screen.bounds.w, h = screen.bounds.h;

    const int bar_w = w / 3;
    screen.pen = Pen(20, 24, 40);
    screen.rectangle(Rect(3, 3, bar_w + 2, 6));
    const int fill = (bar_w * g_world.fuel) / tl::k_fuel_full;
    if (fill > 0) {
        const int32_t pct = (g_world.fuel * 100) / tl::k_fuel_full;
        screen.pen = pct < 25 ? Pen(255, 0, 77)
                              : (pct < 50 ? Pen(255, 163, 0) : Pen(0, 228, 54));
        screen.rectangle(Rect(4, 4, fill, 4));
    }

    char line[16];
    const int32_t fall_cm = (tl::descent(g_world) * 100) >> 16;
    std::snprintf(line, sizeof(line), "%d", fall_cm > 0 ? fall_cm : 0);
    Size size = screen.measure_text(line, minimal_font);
    screen.pen = tl::descent(g_world) > tl::k_safe_descent ? Pen(255, 0, 77)
                                                           : Pen(255, 241, 232);
    screen.text(line, minimal_font, Point(w - size.w - 3, 3));

    std::snprintf(line, sizeof(line), "%d", tl::altitude(g_world) >> 16);
    size = screen.measure_text(line, minimal_font);
    screen.pen = Pen(200, 200, 214);
    screen.text(line, minimal_font, Point(3, h - size.h - 3));

    std::snprintf(line, sizeof(line), "B %d", tl::range_to_target(g_world));
    size = screen.measure_text(line, minimal_font);
    screen.pen = Pen(255, 163, 0);
    screen.text(line, minimal_font, Point(w - size.w - 3, h - size.h - 3));
}

const char* fault_word(tl::Fault fault) {
    switch (fault) {
        case tl::Fault::TooFast: return "TOO FAST";
        case tl::Fault::TooSteep: return "TOO STEEP";
        case tl::Fault::Scraped: return "SCRAPED";
        default: return "TUMBLED";
    }
}

void draw_outcome() {
    char fuel_line[20];
    const char* lines[3];
    Pen pens[3];
    int count;

    if (g_world.state == tl::Flight::Landed) {
        std::snprintf(fuel_line, sizeof(fuel_line), "FUEL %d",
                      (g_world.fuel * 100) / tl::k_fuel_full);
        lines[0] = "DOWN SAFE";
        lines[1] = fuel_line;
        lines[2] = "RECORD";
        pens[0] = Pen(0, 228, 54);
        pens[1] = Pen(255, 241, 232);
        pens[2] = Pen(255, 163, 0);
        count = g_new_record ? 3 : 2;
    } else {
        lines[0] = fault_word(g_world.fault);
        pens[0] = Pen(255, 0, 77);
        count = 1;
    }
    // No retry prompt: any button flies again, and a line saying so is a line
    // the player has to read after every single attempt.
    panel_lines(screen.bounds.h / 2 - 12, lines, pens, count,
                Pen(12, 14, 28, 210));
}

void draw_title() {
    char best[20];
    const char* lines[2] = {"TOM LANDER", best};
    const Pen pens[2] = {Pen(255, 163, 0), Pen(210, 210, 220)};
    int count = 1;
    if (g_best_fuel > 0) {
        std::snprintf(best, sizeof(best), "best fuel %d",
                      static_cast<int>((g_best_fuel * 100) / tl::k_fuel_full));
        count = 2;
    }
    panel_lines(screen.bounds.h / 2 - 14, lines, pens, count,
                Pen(12, 14, 28, 200));
}

void draw_pause() {
    const char* items[2] = {"RESUME", "RESTART"};
    const Pen pens[2] = {
        g_pause_item == 0 ? Pen(255, 163, 0) : Pen(150, 150, 170),
        g_pause_item == 1 ? Pen(255, 163, 0) : Pen(150, 150, 170)};
    panel_lines(screen.bounds.h / 2 - 14, items, pens, 2, Pen(12, 14, 28, 220));
}

void game_init() {
    set_screen_mode(ScreenMode::lores);

    // Every entry, not once per boot: the console calls this each time the
    // game is picked, so it goes back to its title rather than resuming a
    // wreck from a previous session.
    g_shell = Shell::Title;
    g_pause_item = 0;
    g_new_record = false;
    g_save_pending = false;
    g_tick_accumulator = 0;
    g_last_time = 0;

    SaveData data;
    if (read_save(data) && data.magic == k_save_magic) {
        g_best_fuel = data.best_fuel;
    } else {
        g_best_fuel = 0;
    }

    tl::world_init(g_world);
    g_cam_yaw = 0.0f;
}

void step_sim() {
    tl::Input input{};
    for (int i = 0; i < tl::kPodCount; i++) {
        input.pod[i] = (buttons & k_pod_button[i]) != 0;
    }
    input.level = (buttons & Button::DPAD_DOWN) != 0;

    const tl::Flight before = g_world.state;
    tl::world_tick(g_world, input);

    if (before == tl::Flight::Flying && g_world.state == tl::Flight::Landed) {
        if (static_cast<uint32_t>(g_world.fuel) > g_best_fuel) {
            g_best_fuel = static_cast<uint32_t>(g_world.fuel);
            g_new_record = true;
            g_save_pending = true;
        }
    }
}

void game_update(uint32_t time) {
    // Fixed 100 Hz, caught up from the wall clock. A dropped frame costs a
    // frame and never changes what the physics did.
    if (g_last_time == 0) g_last_time = time;
    uint32_t elapsed = time - g_last_time;
    g_last_time = time;
    if (elapsed > 200) elapsed = 200;      // a long stall is not a fast fall
    g_tick_accumulator += elapsed;

    if (buttons & Button::DPAD_LEFT) g_cam_yaw -= k_cam_rate;
    if (buttons & Button::DPAD_RIGHT) g_cam_yaw += k_cam_rate;

    if (g_shell == Shell::Title) {
        g_tick_accumulator = 0;
        if (buttons.pressed & (k_any_face | Button::DPAD_DOWN)) start_flight();
        return;
    }

    if (g_shell == Shell::Paused) {
        g_tick_accumulator = 0;
        if (buttons.pressed & Button::DPAD_UP) g_shell = Shell::Play;
        if (buttons.pressed & Button::DPAD_LEFT) g_pause_item = 0;
        if (buttons.pressed & Button::DPAD_RIGHT) g_pause_item = 1;
        if (buttons.pressed & k_any_face) {
            if (g_pause_item == 0) {
                g_shell = Shell::Play;
            } else {
                start_flight();
            }
        }
        return;
    }

    if (g_world.state != tl::Flight::Flying) {
        g_tick_accumulator = 0;
        save_if_needed();
        // A grace period so the button that was already held when the ship
        // touched down does not skip the card it produced.
        if (g_world.ticks_in_state > 40 &&
            (buttons.pressed & (k_any_face | Button::DPAD_DOWN))) {
            start_flight();
        }
        tl::Input none{};
        tl::world_tick(g_world, none);       // counts out the grace period
        return;
    }

    if (buttons.pressed & Button::DPAD_UP) {
        g_shell = Shell::Paused;
        g_pause_item = 0;
        return;
    }

    while (g_tick_accumulator >= k_tick_ms) {
        g_tick_accumulator -= k_tick_ms;
        step_sim();
        if (g_world.state != tl::Flight::Flying) break;
    }
}

void game_render(uint32_t time) {
    tlr::render_scene(g_world, pse::target_from_screen(), g_cam_yaw, time);

    if (g_shell == Shell::Title) {
        draw_title();
        return;
    }
    if (g_shell == Shell::Paused) {
        draw_pause();
        return;
    }
    if (g_world.state != tl::Flight::Flying) {
        draw_outcome();
        return;
    }
    draw_hud();
}

}  // namespace

// The one symbol this game exports. Everything above is internal linkage, so
// the console can link it beside other games that also have a g_world.
PSE_GAME(tomlander, game_init, game_update, game_render);
