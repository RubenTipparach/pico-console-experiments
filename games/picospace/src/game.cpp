#include <cstdio>

#include "32blit.hpp"

#include "pse/blit_target.hpp"
#include "pse/game.hpp"

#include "render.hpp"
#include "sim.hpp"

using namespace blit;

namespace {

// The shell around the flight: a title over a rocket on its pad, the flight
// itself, and the card that closes it.
enum class Shell : uint8_t { Title, Play, Paused };

ps::World g_world;
Shell g_shell = Shell::Title;
psr::View g_view = psr::View::Flight;
uint8_t g_pause_item = 0;
uint8_t g_title_item = 0;
bool g_save_pending = false;
bool g_sound_on = true;
// The furthest mission reached, and the one START flies. Saved, so the game
// comes back where it was left rather than making the transfer earned again.
uint8_t g_progress = 1;
// Seconds of the best finished flight, per mission, for the debrief line.
uint16_t g_best_s[ps::k_mission_count + 1] = {};
bool g_new_record = false;

constexpr uint32_t k_any_face =
    Button::A | Button::B | Button::X | Button::Y;

// The sim runs on ticks, not on wall clock, so a slow frame costs frames and
// never changes the physics.
constexpr uint32_t k_tick_ms = ps::k_tick_ms;
uint32_t g_tick_accumulator = 0;
uint32_t g_last_time = 0;

// How long DOWN has been held with the engine already shut.
//
// This is the pause gesture, and it exists because the pad has no button left.
// Turning, the throttle, staging, warp, the map and the attitude hold are six
// controls on eight inputs, and the two that are left are the two halves of
// the throttle. Down at zero throttle is the one input in the game that
// already does nothing, so holding it is free: it cannot be pressed by
// accident during a burn, because during a burn it is closing the throttle.
constexpr uint32_t k_pause_hold_ms = 900;
uint32_t g_shut_held_ms = 0;

struct SaveData {
    uint32_t magic;
    uint8_t sound_on;
    uint8_t progress;
    uint16_t best_s[ps::k_mission_count + 1];
};
constexpr uint32_t k_save_magic = 0x31535000u;   // 'P','S',0,'1'

static_assert(sizeof(ps::World) <= 192, "sim state grew past its RAM budget");
static_assert(sizeof(SaveData) <= 16, "save record grew");

// ---- sound ----
//
// An engine and four cues, synthesised rather than sampled: the 32blit
// channels make all of this from waveforms, so it costs a few hundred bytes of
// code and no asset at all.
//
// The engine is noise, because that is what a rocket is. Its volume and its
// pitch both follow the throttle, so a trickle mutters and full power roars,
// and a player can hear how hard they are burning without looking away from
// the horizon.
constexpr uint8_t k_ch_thrust = 0;
constexpr uint8_t k_ch_cue = 1;
bool g_thrust_sounding = false;

void sound_init() {
    channels[k_ch_thrust].waveforms = Waveform::NOISE;
    channels[k_ch_thrust].attack_ms = 20;
    channels[k_ch_thrust].decay_ms = 40;
    channels[k_ch_thrust].sustain = 0xffff;
    channels[k_ch_thrust].release_ms = 220;
    channels[k_ch_thrust].volume = 0;

    channels[k_ch_cue].waveforms = Waveform::TRIANGLE | Waveform::SQUARE;
    channels[k_ch_cue].attack_ms = 4;
    channels[k_ch_cue].sustain = 0;
    channels[k_ch_cue].release_ms = 90;
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

void sound_thrust(int throttle) {
    if (!g_sound_on || throttle <= 6 || g_world.fuel_kg <= 0) {
        sound_stop();
        return;
    }
    channels[k_ch_thrust].frequency =
        static_cast<uint16_t>(240 + (throttle * 420) / 255);
    channels[k_ch_thrust].volume =
        static_cast<uint16_t>(1100 + (throttle * 4400) / 255);
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
    data.sound_on = g_sound_on ? 1 : 0;
    data.progress = g_progress;
    for (int i = 0; i <= ps::k_mission_count; i++) data.best_s[i] = g_best_s[i];
    write_save(data);
    g_save_pending = false;
}

void start_flight(uint8_t mission) {
    if (mission < 1) mission = 1;
    if (mission > ps::k_mission_count) mission = ps::k_mission_count;
    ps::world_init(g_world, static_cast<ps::Mission>(mission));
    g_view = psr::View::Flight;
    g_shell = Shell::Play;
    g_new_record = false;
    g_shut_held_ms = 0;
    sound_stop();
}

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

// ---- drawing the words ----
//
// Every string is measured rather than placed by eye: a hand picked x is only
// correct for the exact string it was tuned against, so the first wording
// change prints through the edge of its own panel.

void text_at(const char* line, int x, int y, Pen pen) {
    screen.pen = pen;
    screen.text(line, minimal_font, Point(x, y));
}

void text_right(const char* line, int right, int y, Pen pen) {
    const Size size = screen.measure_text(line, minimal_font);
    text_at(line, right - size.w, y, pen);
}

void text_centered(const char* line, int y, Pen pen) {
    const Size size = screen.measure_text(line, minimal_font);
    text_at(line, (screen.bounds.w - size.w) / 2, y, pen);
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

const Pen k_pick = Pen(255, 163, 0);
const Pen k_rest = Pen(150, 150, 170);
const Pen k_plain = Pen(216, 222, 234);
const Pen k_good = Pen(0, 228, 54);
const Pen k_warn = Pen(255, 163, 0);
const Pen k_bad = Pen(255, 0, 77);
const Pen k_cold = Pen(127, 210, 255);

Pen row_pen(uint8_t item, uint8_t self) {
    return item == self ? k_pick : k_rest;
}

// A distance, in the unit that fits it. Under ten kilometres a player is
// judging a landing and wants metres; over it they are judging an orbit and a
// five digit number of metres is unreadable at this size.
void write_range(char* out, int size, int32_t metres) {
    // The MAGNITUDE decides the unit, not the value. A periapsis below the
    // surface is negative, and it is negative by tens of kilometres, so a
    // signed comparison sent every one of them down the metres branch: an
    // ascent that is very obviously suborbital reported "PE -45771M", eight
    // characters of false precision in the corner of a 120 pixel screen, and
    // the one number where the sign is the whole message was the hardest one
    // on screen to read.
    const int32_t reach = metres < 0 ? -metres : metres;
    if (reach < 10000) {
        std::snprintf(out, size, "%dm", metres);
    } else {
        std::snprintf(out, size, "%dk", metres / 1000);
    }
}

// ---- the HUD ----
//
// Rule 9: the minimum, and no button prompts.
//   fuel   a bar for the tank that is burning, not the whole rocket, so an
//          empty bar is the cue to stage
//   pips   which stage is lit, one per stage
//   alt    height above the ground under the ship
//   spd    speed relative to whatever it is orbiting, in the colour of what a
//          touchdown at it would do, but only when there is ground close
//          enough for that to be a question
//   AP     the top of the arc the ship is on. This is how orbit is read: raise
//          AP clear of the air, then burn at AP until PE follows it up.
void draw_hud() {
    const int w = screen.bounds.w, h = screen.bounds.h;
    const ps::Elements el = ps::elements(g_world);
    const ps::Body& ref = ps::k_bodies[g_world.ref_body];

    const int bar_w = w / 3;
    screen.pen = Pen(18, 22, 38, 190);
    screen.rectangle(Rect(3, 3, bar_w + 2, 6));
    const int32_t tank = ps::k_stages[g_world.stage].fuel_kg * ps::k_fp8;
    const int fill = static_cast<int>((bar_w * g_world.fuel_kg) / tank);
    if (fill > 0) {
        const int32_t pct = (g_world.fuel_kg * 100) / tank;
        screen.pen = pct < 20 ? k_bad : (pct < 45 ? k_warn : k_good);
        screen.rectangle(Rect(4, 4, fill, 4));
    }
    for (int i = 0; i < ps::k_stage_count; i++) {
        screen.pen = i < g_world.stage ? Pen(80, 84, 100) : k_plain;
        screen.rectangle(Rect(3 + bar_w + 5 + i * 4, 4, 2, 4));
    }

    // The throttle, as a column up the left edge. A bar rather than a number:
    // what a player needs off it is "more than last time", not a percentage.
    screen.pen = Pen(18, 22, 38, 170);
    screen.rectangle(Rect(0, 16, 3, h / 2));
    const int col = (h / 2 - 2) * g_world.throttle / 255;
    if (col > 0) {
        screen.pen = k_good;
        screen.rectangle(Rect(1, 16 + (h / 2 - 2) - col, 2, col));
    }

    char line[16];
    if (ps::warp_factor(g_world) > 1) {
        std::snprintf(line, sizeof(line), "x%d", ps::warp_factor(g_world));
        text_centered(line, 3, k_cold);
    }
    if (g_world.hold != ps::Hold::Off) {
        text_at(g_world.hold == ps::Hold::Prograde ? "PRO" : "RET", 5, 12,
                k_good);
    }

    write_range(line, sizeof(line), ps::clearance_m(g_world));
    text_right(line, w - 3, 3, k_plain);

    const int32_t speed = ps::speed_fp16(g_world);
    std::snprintf(line, sizeof(line), "%d", speed / ps::k_fp16);
    // Coloured by the touchdown the sim would give, and only while there is
    // ground close enough for that to be the question. At orbital speed a red
    // number means nothing, and a HUD that is permanently red is a HUD nobody
    // reads.
    Pen speed_pen = k_plain;
    if (ps::clearance_m(g_world) < 3000) {
        switch (ps::touchdown_band(speed)) {
            case ps::Touchdown::Fatal: speed_pen = k_bad; break;
            case ps::Touchdown::Hard:  speed_pen = k_warn; break;
            default:                   speed_pen = k_good; break;
        }
    }
    const Size size = screen.measure_text(line, minimal_font);
    text_at(line, 5, h - size.h - 3, speed_pen);

    // Apoapsis, once the ship is out of the air. On the pad it means nothing
    // and it says nothing.
    if (ps::clearance_m(g_world) > (ref.atmo_m > 0 ? ref.atmo_m / 3 : 400)) {
        if (el.closed) {
            char range[12];
            write_range(range, sizeof(range),
                        el.apoapsis_m - ref.radius_m);
            std::snprintf(line, sizeof(line), "AP %s", range);
        } else {
            std::snprintf(line, sizeof(line), "ESCAPE");
        }
        text_right(line, w - 3, h - size.h - 3, k_warn);
    }
}

// The map's own two numbers, and the cue that decides a whole mission.
void draw_map_hud() {
    const int w = screen.bounds.w, h = screen.bounds.h;
    const ps::Elements el = ps::elements(g_world);
    const ps::Body& ref = ps::k_bodies[el.ref];
    char line[16], range[12];

    text_centered(ref.name, 3, k_rest);
    if (ps::burn_window(g_world)) text_centered("BURN", 11, k_good);

    const Size size = screen.measure_text("0", minimal_font);
    if (el.closed) {
        write_range(range, sizeof(range), el.periapsis_m - ref.radius_m);
        std::snprintf(line, sizeof(line), "PE %s", range);
        text_at(line, 3, h - size.h - 3, k_cold);
        write_range(range, sizeof(range), el.apoapsis_m - ref.radius_m);
        std::snprintf(line, sizeof(line), "AP %s", range);
        text_right(line, w - 3, h - size.h - 3, k_warn);
    } else {
        text_at("ESCAPE", 3, h - size.h - 3, k_warn);
    }
}

const char* fault_word(ps::Fault fault) {
    switch (fault) {
        case ps::Fault::Impact:  return "TOO FAST";
        case ps::Fault::Toppled: return "TIPPED OVER";
        case ps::Fault::Dry:     return "NO FUEL";
        case ps::Fault::Lost:    return "LOST";
        default:                 return "WRECKED";
    }
}

void draw_outcome() {
    char detail[20];
    char record[20];
    const char* lines[3];
    Pen pens[3];
    int count = 0;

    switch (g_world.state) {
        case ps::Flight::Complete: {
            const uint8_t goal = ps::target_body(g_world);
            lines[count] = goal == ps::kPicopiter
                               ? "IN ORBIT"
                               : ps::k_bodies[goal].name;
            pens[count++] = k_good;
            std::snprintf(detail, sizeof(detail), "%us  %dkg",
                          g_world.mission_ms / 1000,
                          g_world.fuel_kg / ps::k_fp8);
            lines[count] = detail;
            pens[count++] = k_plain;
            if (g_new_record) {
                std::snprintf(record, sizeof(record), "BEST");
                lines[count] = record;
                pens[count++] = k_warn;
            }
            break;
        }
        case ps::Flight::Landed:
            lines[count] = "DOWN SAFE";
            pens[count++] = k_warn;
            std::snprintf(detail, sizeof(detail), "%s",
                          ps::k_bodies[g_world.landed_on].name);
            lines[count] = detail;
            pens[count++] = k_plain;
            break;
        case ps::Flight::Stranded:
            lines[count] = g_world.fault == ps::Fault::Lost ? "LOST" : "ADRIFT";
            pens[count++] = k_bad;
            break;
        default:
            lines[count] = fault_word(g_world.fault);
            pens[count++] = k_bad;
            break;
    }
    // No retry prompt: any button flies again, and a line saying so is a line
    // the player has to read after every single attempt.
    panel_lines(screen.bounds.h / 2 - 14, lines, pens, count,
                Pen(12, 14, 28, 210));
}

// The title, which is a menu. START flies the furthest mission reached.
void draw_title() {
    char start[20];
    char best[20];
    const char* lines[4];
    Pen pens[4];
    int count = 0;

    lines[count] = "PICO SPACE";
    pens[count++] = k_pick;

    // The row names where it is going when there is more than one place to
    // name, so START alone never leaves the player guessing which flight it
    // means.
    if (g_progress > 1) {
        std::snprintf(start, sizeof(start), "FLY TO %s",
                      ps::k_bodies[ps::k_mission_target[g_progress]].name);
        lines[count] = start;
    } else {
        lines[count] = "MAKE ORBIT";
    }
    pens[count++] = row_pen(g_title_item, 0);

    lines[count] = sound_word();
    pens[count++] = row_pen(g_title_item, 1);

    if (g_best_s[g_progress] > 0) {
        std::snprintf(best, sizeof(best), "best %us",
                      static_cast<unsigned>(g_best_s[g_progress]));
        lines[count] = best;
        pens[count++] = Pen(180, 180, 195);
    }
    panel_lines(screen.bounds.h / 2 - 22, lines, pens, count,
                Pen(12, 14, 28, 200));
}

void draw_pause() {
    const char* items[3] = {"RESUME", "RESTART", sound_word()};
    const Pen pens[3] = {row_pen(g_pause_item, 0), row_pen(g_pause_item, 1),
                         row_pen(g_pause_item, 2)};
    panel_lines(screen.bounds.h / 2 - 18, items, pens, 3, Pen(12, 14, 28, 220));
}

void game_init() {
    set_screen_mode(ScreenMode::lores);

    // Every entry, not once per boot: the console calls this each time the
    // game is picked, so it goes back to its title rather than resuming a
    // wreck from a previous session.
    g_shell = Shell::Title;
    g_view = psr::View::Flight;
    g_pause_item = 0;
    g_title_item = 0;
    g_save_pending = false;
    g_new_record = false;
    g_tick_accumulator = 0;
    g_last_time = 0;
    g_shut_held_ms = 0;

    SaveData data;
    if (read_save(data) && data.magic == k_save_magic) {
        g_sound_on = data.sound_on != 0;
        g_progress = data.progress >= 1 && data.progress <= ps::k_mission_count
                         ? data.progress : 1;
        for (int i = 0; i <= ps::k_mission_count; i++) {
            g_best_s[i] = data.best_s[i];
        }
    } else {
        g_sound_on = true;
        g_progress = 1;
        for (int i = 0; i <= ps::k_mission_count; i++) g_best_s[i] = 0;
    }

    sound_init();
    ps::world_init(g_world, static_cast<ps::Mission>(g_progress));
}

// One tick of the sim, from the buttons. `edges` is true only on the first
// tick of a frame: a frame can step the sim several times, and a stage command
// that fired on each of them would drop both stages at once.
void step_sim(bool edges) {
    ps::Input in{};
    in.up = (buttons & Button::DPAD_UP) != 0;
    in.down = (buttons & Button::DPAD_DOWN) != 0;
    in.left = (buttons & Button::DPAD_LEFT) != 0;
    in.right = (buttons & Button::DPAD_RIGHT) != 0;
    if (edges) {
        in.stage = (buttons.pressed & Button::A) != 0;
        in.warp = (buttons.pressed & Button::B) != 0;
        in.hold = (buttons.pressed & Button::Y) != 0;
    }

    const ps::Flight before = g_world.state;
    const uint8_t stage_before = g_world.stage;
    ps::world_tick(g_world, in);

    if (g_world.stage != stage_before) sound_cue(180, 260, 5200);
    sound_thrust(g_world.throttle);

    if (before == ps::Flight::Flying && g_world.state != ps::Flight::Flying) {
        sound_stop();
        if (g_world.state == ps::Flight::Complete) {
            sound_cue(660, 240, 5200);
            const uint8_t m = static_cast<uint8_t>(g_world.mission);
            const uint16_t seconds =
                static_cast<uint16_t>(g_world.mission_ms / 1000);
            if (g_best_s[m] == 0 || seconds < g_best_s[m]) {
                g_best_s[m] = seconds;
                g_new_record = true;
                g_save_pending = true;
            }
            // The transfer is unlocked by reaching orbit, and stays unlocked.
            // Nothing here ever counts progress back down.
            const uint8_t next = static_cast<uint8_t>(m + 1);
            if (next <= ps::k_mission_count && next > g_progress) {
                g_progress = next;
                g_save_pending = true;
            }
        } else if (g_world.state == ps::Flight::Landed) {
            sound_cue(420, 220, 4200);
        } else {
            sound_cue(96, 460, 6000);       // a wreck, low and long
        }
    }
}

void game_update(uint32_t time) {
    // Fixed 100 Hz of MISSION time, caught up from the wall clock. Time warp
    // makes each of those ticks cover more mission time rather than running
    // more of them, so a frame is the same amount of work at x1 and at x200.
    if (g_last_time == 0) g_last_time = time;
    uint32_t elapsed = time - g_last_time;
    g_last_time = time;
    if (elapsed > 200) elapsed = 200;      // a long stall is not a fast fall
    g_tick_accumulator += elapsed;

    if (g_shell == Shell::Title) {
        g_tick_accumulator = 0;
        sound_stop();
        menu_move(g_title_item, 2);
        if (buttons.pressed & k_any_face) {
            if (g_title_item == 0) start_flight(g_progress);
            else toggle_sound();
        }
        save_if_needed();
        return;
    }

    if (g_shell == Shell::Paused) {
        g_tick_accumulator = 0;
        sound_stop();
        menu_move(g_pause_item, 3);
        if (buttons.pressed & k_any_face) {
            if (g_pause_item == 0) {
                g_shell = Shell::Play;
                g_shut_held_ms = 0;
            } else if (g_pause_item == 1) {
                start_flight(static_cast<uint8_t>(g_world.mission));
            } else {
                toggle_sound();
            }
        }
        save_if_needed();
        return;
    }

    if (g_world.state != ps::Flight::Flying) {
        g_tick_accumulator = 0;
        save_if_needed();
        // A grace period, so the button that was already held when the ship
        // touched down does not skip the card it produced.
        if (g_world.ticks_in_state > 40 &&
            (buttons.pressed & (k_any_face | Button::DPAD_UP |
                                Button::DPAD_DOWN))) {
            g_shell = Shell::Title;
            g_title_item = 0;
            ps::world_init(g_world, static_cast<ps::Mission>(g_progress));
        }
        ps::Input none{};
        ps::world_tick(g_world, none);      // counts out the grace period
        return;
    }

    if (buttons.pressed & Button::X) {
        g_view = g_view == psr::View::Flight ? psr::View::Map
                                             : psr::View::Flight;
    }

    // Down, held, with the engine already shut. See k_pause_hold_ms: it is the
    // one input in the game that otherwise does nothing.
    if ((buttons & Button::DPAD_DOWN) && g_world.throttle == 0) {
        g_shut_held_ms += elapsed;
        if (g_shut_held_ms >= k_pause_hold_ms) {
            g_shell = Shell::Paused;
            g_pause_item = 0;
            g_shut_held_ms = 0;
            sound_stop();
            return;
        }
    } else {
        g_shut_held_ms = 0;
    }

    bool edges = true;
    while (g_tick_accumulator >= k_tick_ms) {
        g_tick_accumulator -= k_tick_ms;
        step_sim(edges);
        edges = false;
        if (g_world.state != ps::Flight::Flying) break;
    }
}

void game_render(uint32_t time) {
    psr::render_scene(g_world, pse::target_from_screen(), g_view, time);

    if (g_shell == Shell::Title) {
        draw_title();
        return;
    }
    if (g_shell == Shell::Paused) {
        draw_pause();
        return;
    }
    if (g_world.state != ps::Flight::Flying) {
        draw_outcome();
        return;
    }
    if (g_view == psr::View::Map) {
        draw_map_hud();
    } else {
        draw_hud();
    }
}

}  // namespace

// The one symbol this game exports. Everything above is internal linkage, so
// the console can link it beside other games that also have a g_world.
PSE_GAME(picospace, game_init, game_update, game_render);
