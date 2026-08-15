#include <cstdio>

#include "32blit.hpp"

#include "pse/blit_target.hpp"
#include "pse/game.hpp"

#include "render.hpp"
#include "menu.hpp"
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
uint8_t g_title_item = 0;
uint32_t g_best_fuel = 0;      // fp8 fuel left on the best landing
bool g_new_record = false;
bool g_save_pending = false;
bool g_sound_on = true;
// Frames left before the fuel chime's second note. See where it is fired.
uint8_t g_chime_ticks = 0;
// The furthest mission reached, and the one START flies. Saved, so the game
// comes back where it was left rather than making the delivery earned again.
uint8_t g_progress = 1;

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
    uint8_t sound_on;
    uint8_t progress;
    uint8_t reserved[6];
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

// ---- sound ----
//
// An engine and three cues, synthesised rather than sampled: the 32blit
// channels can make all of this from waveforms, so it costs a few hundred
// bytes of code and no asset at all. The alternative was a wave file per cue,
// which is flash spent on something a square wave says just as well at 120
// pixels.
//
// The thruster is noise, because that is what a rocket is. Its volume and its
// filter frequency both follow total throttle, so a single pod mutters and
// four of them roar, and the player can hear how hard they are burning without
// looking at the fuel bar.
constexpr uint8_t k_ch_thrust = 0;
constexpr uint8_t k_ch_cue = 1;
bool g_thrust_sounding = false;

void sound_init() {
    channels[k_ch_thrust].waveforms = Waveform::NOISE;
    channels[k_ch_thrust].attack_ms = 15;
    channels[k_ch_thrust].decay_ms = 30;
    channels[k_ch_thrust].sustain = 0xffff;
    channels[k_ch_thrust].release_ms = 150;
    channels[k_ch_thrust].volume = 0;

    channels[k_ch_cue].waveforms = Waveform::TRIANGLE | Waveform::SQUARE;
    channels[k_ch_cue].attack_ms = 4;
    channels[k_ch_cue].sustain = 0;
    channels[k_ch_cue].release_ms = 80;
}

void sound_stop() {
    channels[k_ch_thrust].volume = 0;
    if (g_thrust_sounding) {
        channels[k_ch_thrust].trigger_release();
        g_thrust_sounding = false;
    }
}

void sound_cue(uint16_t frequency, uint16_t decay_ms, uint16_t volume) {
    if (!g_sound_on) return;
    channels[k_ch_cue].frequency = frequency;
    channels[k_ch_cue].decay_ms = decay_ms;
    channels[k_ch_cue].volume = volume;
    channels[k_ch_cue].trigger_attack();
}

// Called every frame while flying, from the total the pods are actually
// pulling, so it follows the auto leveller's partial throttles too and not
// just whether a button is down.
void sound_thrust(int total) {
    if (!g_sound_on || total <= 8) {
        sound_stop();
        return;
    }
    // total is 0..1020. Frequency climbs with it so the noise brightens as
    // well as loudens, which is what stops four pods sounding like one loud
    // pod.
    channels[k_ch_thrust].frequency =
        static_cast<uint16_t>(420 + (total * 500) / 1020);
    channels[k_ch_thrust].volume =
        static_cast<uint16_t>(1200 + (total * 4200) / 1020);
    if (!g_thrust_sounding) {
        channels[k_ch_thrust].trigger_attack();
        g_thrust_sounding = true;
    }
}

void save_if_needed() {
    // update() runs outside run_split, so core 1 is parked in its RAM idle
    // loop and a flash write is safe here.
    if (!g_save_pending) return;
    SaveData data{};
    data.magic = k_save_magic;
    data.best_fuel = g_best_fuel;
    data.sound_on = g_sound_on ? 1 : 0;
    data.progress = g_progress;
    write_save(data);
    g_save_pending = false;
}

void start_flight(uint8_t mission) {
    // tl::mission_for, not a comparison written here. This line used to read
    // `mission >= 2 ? Delivery : Hop`, which made mission three unreachable:
    // picking it flew the delivery, finishing that set progress to three, and
    // the game never changed again.
    tl::world_init(g_world, tl::mission_for(mission));
    g_cam_yaw = 0.0f;
    g_new_record = false;
    g_shell = Shell::Play;
    sound_stop();
}

// What the SOUND row reads, which is the whole of how the toggle reports
// itself. Nothing has to say it worked: the word changes.
const char* sound_word() { return g_sound_on ? "SOUND ON" : "SOUND OFF"; }

void toggle_sound() {
    g_sound_on = !g_sound_on;
    if (!g_sound_on) sound_stop();
    g_save_pending = true;
}

// One menu, driven the same way in both places it appears: up and down move,
// any face button picks. No prompt says so, per rule 9, and with nothing on
// screen naming a button no press can be the wrong guess.
void menu_move(uint8_t& item, uint8_t count) {
    if (buttons.pressed & Button::DPAD_UP) {
        item = item == 0 ? static_cast<uint8_t>(count - 1)
                         : static_cast<uint8_t>(item - 1);
    }
    if (buttons.pressed & Button::DPAD_DOWN) {
        item = static_cast<uint8_t>((item + 1) % count);
    }
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
//   dent  one red mark per hard landing already taken, and nothing at all
//         while the hull is clean, so a tidy flight costs the HUD nothing
//   ALT   how far above whatever is underneath
//   SPD   descent rate, in the colour of what a touchdown at it would do
//
// ALT and SPD are stacked in the top right corner, labelled, ALT over SPD.
// They used to sit in opposite corners with no labels at all, which made them
// two unrelated numbers to hunt for rather than one instrument to read: the
// question a lander pilot asks is "how far down and how fast", and that is one
// glance or it is nothing.
//   B nn  which deck, and how far, in the colour of the arrow that points at
//         it. The decks no longer mark themselves, so this and the arrow are
//         between them the whole of how the target is named.
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

    // One mark per point of hull already spent, just past the end of the fuel
    // bar and on its row, so nothing else on screen has to move to make room.
    // Nothing is drawn while the hull is clean: damage announces itself by
    // appearing, which is louder than a gauge that is usually full.
    for (int i = 0; i < g_world.damage; i++) {
        screen.pen = Pen(255, 0, 77);
        screen.rectangle(Rect(3 + bar_w + 5 + i * 5, 4, 3, 4));
    }

    // ALT over SPD, both hard against the right edge. Every string is
    // measured, never placed by eye, so the pair stays flush whatever number
    // of digits either of them happens to be showing.
    char line[16];
    std::snprintf(line, sizeof(line), "ALT %d", tl::altitude(g_world) >> 16);
    Size size = screen.measure_text(line, minimal_font);
    const int alt_h = size.h;
    screen.pen = Pen(200, 200, 214);
    screen.text(line, minimal_font, Point(w - size.w - 3, 3));

    const int32_t fall_cm = (tl::descent(g_world) * 100) >> 16;
    std::snprintf(line, sizeof(line), "SPD %d", fall_cm > 0 ? fall_cm : 0);
    size = screen.measure_text(line, minimal_font);
    // Green, amber, red, straight off the same function the touchdown is
    // judged by. The readout is whole units per second and both band edges sit
    // on exact printed values, so the colour and the number always agree: at
    // 16 it is amber and it lands, at 17 it is red and it does not.
    switch (tl::descent_band(tl::descent(g_world))) {
        case tl::Touchdown::Fatal: screen.pen = Pen(255, 0, 77); break;
        case tl::Touchdown::Hard:  screen.pen = Pen(255, 163, 0); break;
        default:                   screen.pen = Pen(0, 228, 54); break;
    }
    screen.text(line, minimal_font, Point(w - size.w - 3, 3 + alt_h + 2));

    // The deck letter follows the target rather than being spelled B, because
    // the delivery retargets mid flight and a fixed letter would then name the
    // wrong deck at exactly the moment the player is looking for the right one.
    std::snprintf(line, sizeof(line), "%c %d",
                  static_cast<char>('A' + g_world.target),
                  tl::range_to_target(g_world));
    size = screen.measure_text(line, minimal_font);
    screen.pen = Pen(255, 163, 0);
    screen.text(line, minimal_font, Point(w - size.w - 3, h - size.h - 3));

    // One word for which leg of the delivery this is, and only on the delivery.
    // Mission one has nothing to say here and says nothing.
    if (g_world.mission != tl::Mission::Hop &&
        g_world.cargo != tl::kCargoDone) {
        const bool loaded = tl::carrying(g_world);
        // Below the fuel bar, not beside it. The bar is 6 tall at y = 3 and
        // reaches a third of the way across, and a centred "GET CARGO" is wide
        // enough to reach back into it, so the two would have overlapped on
        // exactly the leg the word matters on.
        text_centered(loaded ? "CARGO" : "GET CARGO", 11,
                      loaded ? Pen(194, 112, 58) : Pen(255, 163, 0));
    }
}

const char* fault_word(tl::Fault fault) {
    switch (fault) {
        case tl::Fault::TooFast: return "TOO FAST";
        // Not "TOO STEEP". That named the ground, and the ground has nothing
        // to do with it: this is the hull lying over on its side. The old word
        // was read as a comment about a slope, over an ocean that is flat.
        case tl::Fault::Tipped: return "TIPPED OVER";
        case tl::Fault::Scraped: return "SCRAPED";
        case tl::Fault::Ditched: return "DITCHED";
        // The longest word on this card by a distance. minimal_font is the
        // 3x5 built in, 4 px an advance, so 23 characters is 91 px of text
        // and a 103 px panel on a 120 px screen. It fits, with 17 to spare.
        case tl::Fault::Struck: return "CRASHED INTO A BUILDING";
        case tl::Fault::Broke: return "BROKE UP";
        case tl::Fault::Dry: return "NO FUEL";
        default: return "CRASHED";
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
        lines[0] = g_world.cargo == tl::kCargoDone
                       ? (g_world.mission == tl::Mission::Salvage ? "RECOVERED"
                                                                  : "DELIVERED")
                       : "DOWN SAFE";
        lines[1] = fuel_line;
        lines[2] = "RECORD";
        pens[0] = Pen(0, 228, 54);
        pens[1] = Pen(255, 241, 232);
        pens[2] = Pen(255, 163, 0);
        count = g_new_record ? 3 : 2;
    } else {
        // The altitude goes on the card too. Without it a verdict handed down
        // in mid air reads as a lie: the player knows they never reached the
        // ground and the card says nothing about where they were.
        //
        // A tip over gets the angle instead, for the same reason. That verdict
        // is the only one whose measure is nowhere on the HUD, so the card is
        // the one place it can be shown, and a number beside the limit is what
        // turns "the game decided I failed" into "I was 58 over, the limit is
        // 45".
        if (g_world.fault == tl::Fault::Tipped) {
            std::snprintf(fuel_line, sizeof(fuel_line), "TILT %d, MAX %d",
                          tl::tilt_degrees(tl::tilt(g_world)),
                          tl::tilt_degrees(tl::k_safe_tilt));
        } else {
            std::snprintf(fuel_line, sizeof(fuel_line), "ALT %d",
                          tl::altitude(g_world) >> 16);
        }
        lines[0] = fault_word(g_world.fault);
        lines[1] = fuel_line;
        pens[0] = Pen(255, 0, 77);
        pens[1] = Pen(200, 200, 214);
        count = 2;
    }
    // No retry prompt: any button flies again, and a line saying so is a line
    // the player has to read after every single attempt.
    panel_lines(screen.bounds.h / 2 - 12, lines, pens, count,
                Pen(12, 14, 28, 210));
}

const Pen k_pick = Pen(255, 163, 0);
const Pen k_rest = Pen(150, 150, 170);

Pen row_pen(uint8_t item, uint8_t self) {
    return item == self ? k_pick : k_rest;
}

// The title, which is a level select.
//
// One row per mission unlocked, then SOUND. A first boot is two rows and the
// list grows as the player earns it: no locked rows, no greyed out teases,
// nothing on screen that cannot be pressed. Rule 9's sparse UI falls out of the
// progression rather than being imposed on it.
//
// It used to be a single START row that flew the furthest mission reached,
// which meant a player who had opened three missions had no way to fly the
// first two ever again.
void draw_title() {
    char best[20];
    // Room for the title, every mission, SOUND, and the best fuel line.
    //
    // Asserted rather than trusted: this file does not compile in a host
    // checkout, so a fourth mission would overrun these arrays on the device
    // and nowhere before it. The assert fires at the first build that has one.
    constexpr int k_title_lines = tl::k_mission_count + 3;
    static_assert(k_title_lines >= 1 + (tl::k_mission_count + 1) + 1,
                  "the title menu needs a row for the name, every mission, "
                  "SOUND, and the best fuel line");
    const char* lines[k_title_lines];
    Pen pens[k_title_lines];
    int count = 0;

    lines[count] = "TOM LANDER";
    pens[count++] = k_pick;

    const uint8_t rows = tl::title_row_count(g_progress);
    for (uint8_t row = 0; row < rows; row++) {
        const uint8_t mission = tl::title_row_mission(g_progress, row);
        lines[count] = mission ? tl::mission_name(mission) : sound_word();
        pens[count++] = row_pen(g_title_item, row);
    }

    if (g_best_fuel > 0) {
        std::snprintf(best, sizeof(best), "best fuel %d",
                      static_cast<int>((g_best_fuel * 100) / tl::k_fuel_full));
        lines[count] = best;
        pens[count++] = Pen(180, 180, 195);
    }
    panel_lines(screen.bounds.h / 2 - 8 - count * 4, lines, pens, count,
                Pen(12, 14, 28, 200));
}

// Pause gains the same toggle, because sound is the one setting worth changing
// without quitting the flight to do it, and a MENU row, because without one the
// only ways out of a flight were to finish it or wreck it. On the web that
// meant reloading the page to pick a different mission.
void draw_pause() {
    const char* items[tl::kPauseRowCount] = {"RESUME", "RESTART", "MENU",
                                             sound_word()};
    Pen pens[tl::kPauseRowCount];
    for (uint8_t i = 0; i < tl::kPauseRowCount; i++) {
        pens[i] = row_pen(g_pause_item, i);
    }
    panel_lines(screen.bounds.h / 2 - 22, items, pens, tl::kPauseRowCount,
                Pen(12, 14, 28, 220));
}

void game_init() {
    set_screen_mode(ScreenMode::lores);

    // Every entry, not once per boot: the console calls this each time the
    // game is picked, so it goes back to its title rather than resuming a
    // wreck from a previous session.
    g_shell = Shell::Title;
    g_pause_item = 0;
    g_title_item = 0;
    g_new_record = false;
    g_save_pending = false;
    g_tick_accumulator = 0;
    g_last_time = 0;

    SaveData data;
    if (read_save(data) && data.magic == k_save_magic) {
        g_best_fuel = data.best_fuel;
        g_sound_on = data.sound_on != 0;
        g_progress = data.progress >= 1 && data.progress <= tl::k_mission_count
                         ? data.progress : 1;
    } else {
        g_best_fuel = 0;
        g_sound_on = true;         // sound on until someone turns it off
        g_progress = 1;
    }

    sound_init();
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
    const bool had_cargo = tl::carrying(g_world);
    const uint8_t crates_before = g_world.crates_taken;
    tl::world_tick(g_world, input);

    int total = 0;
    for (int i = 0; i < tl::kPodCount; i++) total += g_world.throttle[i];
    sound_thrust(total);

    // The crate coming aboard, which is the one event in the flight with no
    // panel and no state change to announce it.
    if (!had_cargo && tl::carrying(g_world)) sound_cue(900, 160, 5000);

    // A fuel crate. Two notes rather than one, a fifth apart and rising, which
    // is what makes it read as a chime instead of the blip the cargo gets.
    // sound_cue retriggers ONE channel, so the second note cannot be asked for
    // in the same frame: it is queued and fired a few frames later, and the
    // first note's own decay carries the gap.
    if (g_world.crates_taken != crates_before) {
        sound_cue(1046, 130, 5200);        // C
        g_chime_ticks = 7;
    } else if (g_chime_ticks > 0 && --g_chime_ticks == 0) {
        sound_cue(1568, 300, 4800);        // the G above it
    }

    if (before == tl::Flight::Flying && g_world.state != tl::Flight::Flying) {
        sound_stop();
        if (g_world.state == tl::Flight::Landed) {
            sound_cue(520, 220, 5200);
            if (static_cast<uint32_t>(g_world.fuel) > g_best_fuel) {
                g_best_fuel = static_cast<uint32_t>(g_world.fuel);
                g_new_record = true;
                g_save_pending = true;
            }
            // The delivery is unlocked by finishing the hop, and stays
            // unlocked. progress_after never counts down, so replaying an
            // early mission cannot take a later one away.
            const uint8_t opened = tl::progress_after(g_progress, g_world.mission);
            if (opened != g_progress) {
                g_progress = opened;
                g_save_pending = true;
            }
        } else {
            // A crash: the lowest and longest cue this game has, but no
            // lower than 400 Hz. It was 110, which is inaudible on the
            // device's piezo and fine on the web, which is why it survived.
            // See tools/tests/test_audio_range.py.
            sound_cue(400, 420, 6000);
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
        sound_stop();
        menu_move(g_title_item, tl::title_row_count(g_progress));
        if (buttons.pressed & k_any_face) {
            const uint8_t mission =
                tl::title_row_mission(g_progress, g_title_item);
            if (mission) start_flight(mission);
            else toggle_sound();
        }
        save_if_needed();
        return;
    }

    if (g_shell == Shell::Paused) {
        g_tick_accumulator = 0;
        sound_stop();
        menu_move(g_pause_item, tl::kPauseRowCount);
        if (buttons.pressed & k_any_face) {
            if (g_pause_item == tl::kPauseResume) {
                g_shell = Shell::Play;
            } else if (g_pause_item == tl::kPauseRestart) {
                start_flight(tl::number_of(g_world.mission));
            } else if (g_pause_item == tl::kPauseMenu) {
                // Back to the title, with the cursor on the mission that was
                // just abandoned rather than at the top of the list, so
                // stepping out to try a different one lands where you were.
                g_shell = Shell::Title;
                g_title_item = static_cast<uint8_t>(
                    tl::number_of(g_world.mission) - 1);
                sound_stop();
            } else {
                toggle_sound();
            }
        }
        save_if_needed();
        return;
    }

    if (g_world.state != tl::Flight::Flying) {
        g_tick_accumulator = 0;
        save_if_needed();
        // A grace period so the button that was already held when the ship
        // touched down does not skip the card it produced.
        if (g_world.ticks_in_state > 40 &&
            (buttons.pressed & (k_any_face | Button::DPAD_DOWN))) {
            // Landing rolls straight on into the NEXT mission, not the
            // furthest one unlocked: replaying the hop with everything open
            // should lead to the delivery rather than skip to the salvage.
            // A crash flies the same mission again, because the thing that
            // just went wrong is the thing worth another go.
            start_flight(g_world.state == tl::Flight::Landed
                             ? tl::next_mission(g_world.mission)
                             : tl::number_of(g_world.mission));
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
