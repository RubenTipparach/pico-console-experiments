#include "32blit.hpp"

#include "pse/blit_target.hpp"
#include "pse/game.hpp"
#include "render.hpp"
#include "sim.hpp"

// The only file here that touches the SDK, and it is thin on purpose: reading
// buttons, keeping the tick accumulator, and handing the frame's surface over.
// It draws nothing at all. Everything on screen, including the HUD and every
// menu, is in render.cpp so the preview harness renders it on a host and the
// thumbnail comes out of the same code the device runs.

using namespace blit;

namespace {

twinflare::Race g_race;
twinflare::Chrome g_chrome;
uint32_t g_accumulator = 0;
uint32_t g_last_time = 0;

// The one open question in the whole design, and it is a button.
//
// The brief puts the pause menu and the double tap boost both on A. They
// cannot both be there: a double tap ends with a finger resting on the button,
// so a pause menu on A opens on the second tap of every boost.
//
// Boost lives on the throttle instead, and the reason is not the clash. In
// Episode I Racer the Thrust Meter is a readout of your current SPEED rather
// than a bar you charge, and at the top of it you arm the boost by briefly
// releasing the accelerator and pressing it again. The resource you spend is
// heat. Double tapping the throttle is the mechanic, not a workaround.
//
// Flipping this to true moves boost to A and pause to holding A, which is the
// brief as literally written. It is one constant because it should be one
// decision, not a rewrite.
constexpr bool k_boost_on_a = false;
constexpr uint32_t k_hold_pause_ms = 340;

uint32_t g_a_held_since = 0;

twinflare::Input read_input() {
    twinflare::Input in{};
    in.throttle = buttons & Button::B;
    in.brake = buttons & Button::Y;
    in.repair = buttons & Button::X;
    in.left = buttons & Button::DPAD_LEFT;
    in.right = buttons & Button::DPAD_RIGHT;
    in.up = buttons & Button::DPAD_UP;
    in.down = buttons & Button::DPAD_DOWN;
    in.boost_press = k_boost_on_a ? (buttons.pressed & Button::A)
                                  : (buttons.pressed & Button::B);
    return in;
}

void start_race() {
    twinflare::race_init(g_race, g_chrome.track, g_chrome.pod);
    g_chrome.screen = twinflare::Screen::Race;
}

void update_menu(twinflare::Screen next, uint8_t& value, int count) {
    if (buttons.pressed & Button::DPAD_LEFT)
        value = static_cast<uint8_t>((value + count - 1) % count);
    if (buttons.pressed & Button::DPAD_RIGHT)
        value = static_cast<uint8_t>((value + 1) % count);
    if (buttons.pressed & (Button::A | Button::B)) g_chrome.screen = next;
}

void update_play(uint32_t elapsed) {
    // Pause. In the default scheme A is only ever the pause button; in the
    // other one it is a hold, so a boost tap does not open a menu.
    if (k_boost_on_a) {
        if (buttons & Button::A) {
            if (g_a_held_since == 0) g_a_held_since = 1;
            else if (g_a_held_since < k_hold_pause_ms) g_a_held_since += elapsed;
            else { g_chrome.screen = twinflare::Screen::Paused; g_chrome.menu_item = 0; }
        } else {
            g_a_held_since = 0;
        }
    } else if (buttons.pressed & Button::A) {
        g_chrome.screen = twinflare::Screen::Paused;
        g_chrome.menu_item = 0;
        return;
    }

    const twinflare::Input in = read_input();

    g_accumulator += elapsed;
    // A cap, so a long stall (a flash write, a first frame) does not run the
    // race forward in one go with a single frame's input held down.
    if (g_accumulator > twinflare::k_tick_ms * 8)
        g_accumulator = twinflare::k_tick_ms * 8;
    while (g_accumulator >= twinflare::k_tick_ms) {
        g_accumulator -= twinflare::k_tick_ms;
        twinflare::race_tick(g_race, in);
    }
    if (g_race.finished) g_chrome.screen = twinflare::Screen::Results;
}

void update_paused() {
    if (buttons.pressed & Button::DPAD_UP)
        g_chrome.menu_item = static_cast<uint8_t>((g_chrome.menu_item + 2) % 3);
    if (buttons.pressed & Button::DPAD_DOWN)
        g_chrome.menu_item = static_cast<uint8_t>((g_chrome.menu_item + 1) % 3);
    if (buttons.pressed & (Button::A | Button::B)) {
        if (g_chrome.menu_item == 0) g_chrome.screen = twinflare::Screen::Race;
        else if (g_chrome.menu_item == 1) start_race();
        else g_chrome.screen = twinflare::Screen::Title;
    }
}

void game_init() {
    set_screen_mode(ScreenMode::lores);
    g_chrome = twinflare::Chrome{};
    g_chrome.boost_on_a = k_boost_on_a;
    twinflare::race_init(g_race, 0, 0);
    g_accumulator = 0;
    g_last_time = 0;
}

void game_update(uint32_t time) {
    const uint32_t elapsed = g_last_time == 0 ? 0 : time - g_last_time;
    g_last_time = time;
    g_chrome.time_ms = time;

    switch (g_chrome.screen) {
        case twinflare::Screen::Title:
            if (buttons.pressed) g_chrome.screen = twinflare::Screen::PodSelect;
            break;
        case twinflare::Screen::PodSelect:
            update_menu(twinflare::Screen::TrackSelect, g_chrome.pod,
                        twinflare::k_racer_count);
            break;
        case twinflare::Screen::TrackSelect:
            if (buttons.pressed & (Button::A | Button::B)) { start_race(); break; }
            update_menu(twinflare::Screen::TrackSelect, g_chrome.track,
                        twinflare::k_track_count);
            break;
        case twinflare::Screen::Race:    update_play(elapsed); break;
        case twinflare::Screen::Paused:  update_paused(); break;
        case twinflare::Screen::Results:
            if (buttons.pressed) g_chrome.screen = twinflare::Screen::Title;
            break;
    }
}

void game_render(uint32_t) {
    twinflare::render_frame(g_race, g_chrome, pse::target_from_screen());
}

}  // namespace

PSE_GAME(twinflare, game_init, game_update, game_render);
