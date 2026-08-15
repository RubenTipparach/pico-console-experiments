#include "32blit.hpp"

#include "pse/blit_target.hpp"
#include "pse/game.hpp"
#include "render.hpp"
#include "sfx.hpp"
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
    // Repair on Y and the air brake on X, which is the swap of what they were.
    // Repair is the one held under pressure, with an engine going and a corner
    // coming, so it gets the button that is easier to reach for.
    in.brake = buttons & Button::X;
    in.repair = buttons & Button::Y;
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
    // Past the flag the race runs itself: the sim flies the pod, the camera
    // cuts around it, and the finishing times come up over the top. The only
    // thing a button does here is cut it short, because a player who has seen
    // enough should not have to wait out the rest of the field.
    if (g_race.phase == twinflare::Phase::Finished) {
        if (buttons.pressed) {
            g_chrome.screen = twinflare::Screen::Results;
            return;
        }
    } else if (k_boost_on_a) {
        // Pause. In the default scheme A is only ever the pause button; in the
        // other one it is a hold, so a boost tap does not open a menu.
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
    // Every tick's events folded into one frame's worth. The sound layer is
    // called once a frame and the sim can step eight times inside it, so
    // without this a lap completed on the first of those eight ticks is a lap
    // nobody hears.
    twinflare::Events heard{};
    while (g_accumulator >= twinflare::k_tick_ms) {
        g_accumulator -= twinflare::k_tick_ms;
        twinflare::race_tick(g_race, in);
        twinflare::merge_events(heard, g_race.ev);
    }
    tfs::sfx_handle(heard);
    // `done`, not `finished`. Finished is the moment the player crosses, and
    // this used to switch to the results panel on it, which threw away the
    // whole of the run in: the flat panel appeared the instant the pod passed
    // the line and nobody else's time was ever on it. Done is when the field is
    // in, or thirty seconds later, whichever comes first.
    if (g_race.done) g_chrome.screen = twinflare::Screen::Results;
}

// The pause rows, in the order draw_pause draws them. Named rather than
// numbered because the sound row went in between resume and restart, and a
// chain of bare 0/1/2 comparisons is exactly the sort of thing that silently
// starts restarting the race when a row is inserted above it.
enum PauseRow : uint8_t { kResume, kSound, kRestart, kQuit };

void update_paused() {
    const int n = twinflare::k_pause_rows;
    if (buttons.pressed & Button::DPAD_UP)
        g_chrome.menu_item = static_cast<uint8_t>((g_chrome.menu_item + n - 1) % n);
    if (buttons.pressed & Button::DPAD_DOWN)
        g_chrome.menu_item = static_cast<uint8_t>((g_chrome.menu_item + 1) % n);
    if (buttons.pressed & (Button::A | Button::B)) {
        switch (g_chrome.menu_item) {
            case kResume: g_chrome.screen = twinflare::Screen::Race; break;
            case kSound:
                // Stays on the menu, because a toggle you have to reopen the
                // menu to check is a toggle nobody trusts.
                g_chrome.sound_on = !g_chrome.sound_on;
                tfs::sfx_set_enabled(g_chrome.sound_on);
                break;
            case kRestart: start_race(); break;
            default: g_chrome.screen = twinflare::Screen::Title; break;
        }
    }
}

void game_init() {
    pse::set_screen_mode(pse::ScreenMode::lores);
    g_chrome = twinflare::Chrome{};
    g_chrome.boost_on_a = k_boost_on_a;
    twinflare::race_init(g_race, 0, 0);
    tfs::sfx_init();
    g_accumulator = 0;
    g_last_time = 0;
}

// Rule 8 asks for the budget to be stated; this is the budget being kept. The
// whole race is one static instance and nothing in it is allocated, so its size
// is the entire sim RAM cost of the game and a compiler is a better place to
// notice it growing than a test is.
static_assert(sizeof(twinflare::Race) <= 512,
              "the race state has grown past its RAM budget");

void game_update(uint32_t time) {
    const uint32_t elapsed = g_last_time == 0 ? 0 : time - g_last_time;
    g_last_time = time;
    g_chrome.time_ms = time;

    // Off everywhere but the race. The engine is a held note, so a menu that
    // does not silence it is a menu with a pod idling under it.
    if (g_chrome.screen != twinflare::Screen::Race) tfs::sfx_silence();

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

    // EVERY update, including the menus and the pause screen. The sequencer
    // steps here, so a screen that skips it freezes whatever cue was playing
    // when it opened, mid note, holding the channel.
    tfs::sfx_tick();
}

void game_render(uint32_t) {
    twinflare::render_frame(g_race, g_chrome, pse::target_from_screen());
}

}  // namespace

PSE_GAME(twinflare, game_init, game_update, game_render);
