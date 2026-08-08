#include "32blit.hpp"

#include "pse/blit_target.hpp"
#include "pse/game.hpp"

#include "render.hpp"
#include "sim.hpp"

using namespace blit;

namespace {

// This file is thin on purpose.
//
// Everything Starlance draws, including the menus and the HUD, is drawn in
// render.cpp through pse::draw_text into a RenderTarget. What is left here is
// only the things that genuinely need the SDK: reading buttons, making noise,
// writing a save, and handing the frame's surface over. The consequence is
// that the preview harness renders every screen this game has, so a layout
// mistake is a picture on a laptop rather than a report from a device.

sl::World g_world;
slr::Chrome g_chrome{};

// The sim runs on ticks, not on wall clock, so a slow frame costs frames and
// never changes the physics.
constexpr uint32_t k_tick_ms = 10;
uint32_t g_tick_accumulator = 0;
uint32_t g_last_time = 0;

bool g_save_pending = false;
uint32_t g_best_score = 0;

struct SaveData {
    uint32_t magic;
    uint32_t best_score;
    uint8_t sound_on;
    uint8_t invert_pitch;
    uint8_t reserved[6];
};
constexpr uint32_t k_save_magic = 0x314C5453u;   // 'S','T','L','1'

static_assert(sizeof(SaveData) <= 16, "save record grew");

// ---- the control scheme ----
//
// The d-pad flies the ship and X is a modifier on the whole of it: held, left
// and right roll instead of yawing, and up and down work the throttle instead
// of the nose. That is five axes of control on a four way pad and one shoulder
// of a button, without spending a face button on any of them, and it is why X
// does nothing at all on its own.
//
// Y is a press and a hold and they do different things. HELD, the camera
// swings onto whatever is targeted and stays there. RELEASED, the target steps
// to the next contact. Acting on the release rather than the press is what
// makes both possible on one button: a press cannot know yet whether it is
// going to be a hold.
//
// X and Y together open the pause menu. That press is marked as spent, so
// letting go of Y afterwards does not also change target on the way into the
// menu.
constexpr uint32_t k_any_face = Button::A | Button::B | Button::X | Button::Y;

// Y's state across frames, for the press/hold/release split above.
bool g_target_down = false;
bool g_target_spent = false;

// `tapped`, not `pressed`, and the name is load bearing.
//
// The SDK has a `blit::pressed(uint32_t)` of its own, and this file says
// `using namespace blit;`. A helper called `pressed` in here is found first by
// ordinary lookup and hides it, which is fine right up until the argument is a
// `blit::Button`: that pulls `blit` in as an associated namespace, argument
// dependent lookup adds the SDK's overload back, and both are then exact
// matches for a uint32_t. Every call passing a Button is ambiguous and every
// call passing a plain uint32_t compiles, which is why it looked like three
// unrelated errors rather than one shadowed name.
//
// The host tests cannot catch this: they never compile a game's SDK facing
// file, so the first thing to notice was the build on main.
bool held(uint32_t button) { return (buttons & button) != 0; }
bool tapped(uint32_t button) { return (buttons.pressed & button) != 0; }

sl::Input read_flight() {
    sl::Input in{};

    const bool modifier = held(Button::X);
    const int8_t across = static_cast<int8_t>(
        (held(Button::DPAD_RIGHT) ? 1 : 0) - (held(Button::DPAD_LEFT) ? 1 : 0));

    const int8_t vertical = static_cast<int8_t>((held(Button::DPAD_UP) ? 1 : 0) -
                                                (held(Button::DPAD_DOWN) ? 1 : 0));

    if (modifier) {
        in.roll = across;
        in.throttle = vertical;
    } else {
        in.yaw = across;
        // Up is nose up unless the pilot says otherwise. Both readings are
        // defensible and neither is wrong, which is exactly why it is a
        // setting rather than a decision: a stick pushed forward pitches down
        // in every simulator, and a d-pad pressed up moves the crosshair up in
        // every arcade game, and this is both at once.
        //
        // The throttle is deliberately NOT inverted with it. Pulling back on a
        // stick to climb is a convention about a nose; no throttle anywhere
        // goes backwards for more power.
        in.pitch = g_chrome.invert_pitch ? static_cast<int8_t>(-vertical)
                                         : vertical;
    }

    in.fire = held(Button::A);
    in.launch = held(Button::B);

    // The target button, on release. See k_any_face above for why.
    const bool target_now = held(Button::Y);
    in.cycle_target = g_target_down && !target_now && !g_target_spent;
    g_chrome.look_at_target = target_now && !g_target_spent;
    if (!target_now) g_target_spent = false;
    g_target_down = target_now;

    return in;
}

// ---- sound ----
//
// Four cues and an engine, synthesised rather than sampled: the 32blit
// channels make all of this from waveforms, so it costs a few hundred bytes of
// code and no asset at all.
constexpr uint8_t k_ch_engine = 0;
constexpr uint8_t k_ch_gun = 1;
constexpr uint8_t k_ch_hit = 2;
bool g_engine_sounding = false;

void sound_init() {
    channels[k_ch_engine].waveforms = Waveform::NOISE;
    channels[k_ch_engine].attack_ms = 200;
    channels[k_ch_engine].decay_ms = 100;
    channels[k_ch_engine].sustain = 0xffff;
    channels[k_ch_engine].release_ms = 400;
    channels[k_ch_engine].frequency = 140;
    channels[k_ch_engine].volume = 0;

    channels[k_ch_gun].waveforms = Waveform::SQUARE;
    channels[k_ch_gun].attack_ms = 2;
    channels[k_ch_gun].sustain = 0;
    channels[k_ch_gun].release_ms = 40;

    channels[k_ch_hit].waveforms = Waveform::NOISE;
    channels[k_ch_hit].attack_ms = 2;
    channels[k_ch_hit].sustain = 0;
    channels[k_ch_hit].release_ms = 220;
}

void engine_sound(bool on) {
    if (on && g_chrome.sound_on) {
        if (!g_engine_sounding) {
            channels[k_ch_engine].volume = 1400;
            channels[k_ch_engine].trigger_attack();
            g_engine_sounding = true;
        }
        return;
    }
    if (g_engine_sounding) {
        channels[k_ch_engine].volume = 0;
        channels[k_ch_engine].trigger_release();
        g_engine_sounding = false;
    }
}

void cue(uint8_t channel, uint16_t frequency, uint16_t decay_ms,
         uint16_t volume) {
    if (!g_chrome.sound_on) return;
    channels[channel].frequency = frequency;
    channels[channel].decay_ms = decay_ms;
    channels[channel].volume = volume;
    channels[channel].trigger_attack();
}

void sound_stop() {
    engine_sound(false);
    channels[k_ch_gun].volume = 0;
    channels[k_ch_hit].volume = 0;
}

// ---- persistence ----

void save_if_needed() {
    // update() runs outside run_split, so core 1 is parked in its RAM idle
    // loop and a flash write is safe here. Never move this into render().
    if (!g_save_pending) return;
    SaveData data{};
    data.magic = k_save_magic;
    data.best_score = g_best_score;
    data.sound_on = g_chrome.sound_on ? 1 : 0;
    data.invert_pitch = g_chrome.invert_pitch ? 1 : 0;
    write_save(data);
    g_save_pending = false;
}

void load_save() {
    SaveData data{};
    if (read_save(data) && data.magic == k_save_magic) {
        g_best_score = data.best_score;
        g_chrome.sound_on = data.sound_on != 0;
        g_chrome.invert_pitch = data.invert_pitch != 0;
    } else {
        g_best_score = 0;
        g_chrome.sound_on = true;
        g_chrome.invert_pitch = false;
    }
    g_chrome.best_score = g_best_score;
}

// ---- the shell ----

void launch() {
    sl::world_init(g_world, 0x5A1CE001u ^ (now() * 2654435761u));
    g_chrome.screen = slr::Screen::Play;
    g_tick_accumulator = 0;
    sound_stop();
}

void finish_sortie() {
    if (g_world.score > g_best_score) {
        g_best_score = g_world.score;
        g_chrome.best_score = g_best_score;
        g_save_pending = true;
    }
    g_chrome.screen = slr::Screen::Debrief;
    sound_stop();
}

// One menu, driven the same way in both places it appears: up and down move,
// any face button picks. Nothing on screen names a button, per rule 9, and
// with nothing naming one no press can be the wrong guess.
void menu_move(uint8_t& item, uint8_t count) {
    if (tapped(Button::DPAD_UP)) {
        item = item == 0 ? static_cast<uint8_t>(count - 1)
                         : static_cast<uint8_t>(item - 1);
    }
    if (tapped(Button::DPAD_DOWN)) {
        item = static_cast<uint8_t>((item + 1) % count);
    }
}

void toggle_sound() {
    g_chrome.sound_on = !g_chrome.sound_on;
    if (!g_chrome.sound_on) sound_stop();
    g_save_pending = true;
}

void toggle_pitch() {
    g_chrome.invert_pitch = !g_chrome.invert_pitch;
    g_save_pending = true;
}

void update_title() {
    menu_move(g_chrome.item, slr::kTitleItemCount);
    if (!tapped(k_any_face)) return;
    switch (g_chrome.item) {
        case slr::kLaunch:      launch(); break;
        case slr::kTitleSound:  toggle_sound(); break;
        case slr::kTitleInvert: toggle_pitch(); break;
        default: break;
    }
}

void update_paused() {
    menu_move(g_chrome.item, slr::kPauseItemCount);
    if (!tapped(k_any_face)) return;
    switch (g_chrome.item) {
        case slr::kResume:
            g_chrome.screen = slr::Screen::Play;
            break;
        case slr::kPauseSound:
            toggle_sound();
            break;
        case slr::kPauseInvert:
            toggle_pitch();
            break;
        case slr::kAbort:
            finish_sortie();
            break;
        default:
            break;
    }
}

// What the guns and the hull were doing last frame, so a cue fires on the
// change rather than every frame the trigger is down.
uint8_t g_last_missiles = 0;
int16_t g_last_hull = 0;
uint16_t g_last_kills = 0;
uint16_t g_last_gun_reload = 0;

void voice_the_frame() {
    if (g_world.gun_reload > g_last_gun_reload) {
        cue(k_ch_gun, 900, 30, 1600);
    }
    if (g_world.missiles < g_last_missiles) {
        cue(k_ch_gun, 220, 220, 2600);
    }
    if (g_world.hull < g_last_hull) {
        cue(k_ch_hit, 90, 200, 4200);
    }
    if (g_world.kills > g_last_kills) {
        cue(k_ch_hit, 60, 320, 4800);
    }
    g_last_gun_reload = g_world.gun_reload;
    g_last_missiles = g_world.missiles;
    g_last_hull = g_world.hull;
    g_last_kills = g_world.kills;
}

void update_play(uint32_t elapsed) {
    // X and Y together pauses. The press is marked spent BEFORE the flight
    // controls are read, so the same press cannot also padlock the camera, and
    // letting go of Y on the way into the menu cannot step the target.
    const bool chord = held(Button::X) && tapped(Button::Y);
    if (chord) g_target_spent = true;

    // Read regardless of the chord, so the target button's press and release
    // stay in step across the frame that opens the menu. Skipping it here left
    // the button looking held, and the first release after resuming stepped
    // the target for no reason the player could see.
    const sl::Input in = read_flight();

    if (chord) {
        g_chrome.screen = slr::Screen::Paused;
        g_chrome.item = slr::kResume;
        g_chrome.look_at_target = false;
        sound_stop();
        return;
    }

    g_tick_accumulator += elapsed;
    // A cap, so a long stall (a flash write, a first frame) does not run the
    // battle forward in one go with a single frame's input held down.
    if (g_tick_accumulator > k_tick_ms * 8) g_tick_accumulator = k_tick_ms * 8;

    while (g_tick_accumulator >= k_tick_ms) {
        g_tick_accumulator -= k_tick_ms;
        sl::world_tick(g_world, in);
    }

    voice_the_frame();
    engine_sound(true);

    if (!sl::in_flight(g_world)) finish_sortie();
}

void update_debrief() {
    if (tapped(k_any_face)) {
        g_chrome.screen = slr::Screen::Title;
        g_chrome.item = slr::kLaunch;
    }
}

// ---- the three the SDK wants, via pse::Game ----

void game_init() {
    set_screen_mode(ScreenMode::lores);

    g_chrome = slr::Chrome{};
    g_chrome.screen = slr::Screen::Title;
    g_chrome.item = slr::kLaunch;
    load_save();

    sound_init();
    sl::world_init(g_world);
    g_tick_accumulator = 0;
    g_last_time = 0;
}

void game_update(uint32_t time) {
    const uint32_t elapsed = g_last_time == 0 ? 0 : time - g_last_time;
    g_last_time = time;

    switch (g_chrome.screen) {
        case slr::Screen::Title:   update_title(); break;
        case slr::Screen::Play:    update_play(elapsed); break;
        case slr::Screen::Paused:  update_paused(); break;
        case slr::Screen::Debrief: update_debrief(); break;
    }

    if (g_chrome.screen != slr::Screen::Play) engine_sound(false);

    save_if_needed();
}

void game_render(uint32_t time) {
    slr::render_scene(g_world, g_chrome, pse::target_from_screen(), time);
}

}  // namespace

PSE_GAME(starlance, game_init, game_update, game_render);
