#include <cstdio>

#include "32blit.hpp"

#include "pse/blit_target.hpp"

#include "render.hpp"
#include "sfx.hpp"
#include "sim.hpp"

using namespace blit;

namespace {

// The shell around the sim: menu in front of it, records book over it. The
// pond ticks with empty input behind the menus, which makes the title screen
// its own attract mode for free.
enum class Shell : uint8_t { Title, Options, Scores, Play };

kf::World g_world;
Shell g_shell = Shell::Title;
int g_cursor = 0;
bool g_sound_off = false;
bool g_show_records = false;

// The sim's RAM footprint is a hard promise, checked at compile time. If this
// fails, something grew without its cost being paid attention to.
static_assert(sizeof(kf::World) <= 768, "sim state grew past its RAM budget");
// The board of ten tournament scores took this past 64. Still one flash
// write of one small struct, and the budget exists to make growth a decision
// rather than a drift, so it moves once and deliberately.
static_assert(sizeof(kf::SaveData) <= 128, "save record grew");

void save_if_safe() {
    // Flash writes disable XIP, and core 1 survives that only while parked in
    // its RAM idle loop, which is any time outside run_split. update() is
    // outside run_split by construction; the extra mode check just keeps a
    // mid fight frame hitch from ever being possible.
    if (!g_world.save_pending || g_world.mode == kf::Mode::Fight) return;
    kf::SaveData data;
    kf::world_make_save(g_world, data);
    data.sound_off = g_sound_off ? 1 : 0;
    blit::write_save(data);
    g_world.save_pending = false;
}

void draw_records_overlay() {
    screen.pen = Pen(8, 6, 20, 220);
    screen.rectangle(Rect(6, 4, 108, 112));
    screen.pen = Pen(255, 255, 238);
    screen.text("RECORDS", minimal_font, Point(10, 7));

    char line[24];
    for (int i = 0; i < kf::k_species_count; i++) {
        const kf::Species& s = kf::k_species[i];
        const int y = 16 + i * 8;
        if (g_world.records.caught[i] == 0) {
            screen.pen = Pen(90, 85, 120);
            screen.text("--------", minimal_font, Point(10, y));
            continue;
        }
        screen.pen = Pen(s.r, s.g, s.b);
        screen.text(s.name, minimal_font, Point(10, y));
        snprintf(line, sizeof(line), "%dcm x%d",
                 g_world.records.best_cm[i], g_world.records.caught[i]);
        screen.pen = Pen(210, 210, 220);
        screen.text(line, minimal_font, Point(66, y));
    }
}

// The hook's distance readout, in meters, for the hook's whole life: it
// rises with the cast, holds while the lure sinks, and counts down through
// the fight. Zero is never shown while a fish is on; the moment it would
// read zero is the moment the card appears.
void draw_fight_distance() {
    const kf::Mode m = g_world.mode;
    if (m != kf::Mode::Fight && m != kf::Mode::Sinking &&
        m != kf::Mode::Flying) return;
    const int dm = kf::hook_distance_dm(g_world);
    char line[12];
    snprintf(line, sizeof(line), "%d.%dm", dm / 10, dm % 10);
    screen.pen = Pen(8, 6, 20, 170);
    screen.rectangle(Rect(84, 101, 34, 9));
    screen.pen = Pen(255, 255, 238);
    screen.text(line, minimal_font, Point(87, 103));
}

void draw_card() {
    if (g_world.mode != kf::Mode::Landed || g_world.card_species < 0) return;
    const kf::Species& s = kf::k_species[g_world.card_species];

    // The card sits in the bottom viewport, under the water line. The trophy
    // fish is held up in the top one, and the card used to be drawn across it:
    // the bigger the fish, the more of it its own name covered.
    screen.pen = Pen(8, 6, 20, 200);
    screen.rectangle(Rect(0, 64, 120, 24));

    char line[28];
    snprintf(line, sizeof(line), "%s %dcm", s.name, g_world.card_size);
    screen.pen = Pen(255, 255, 238);
    screen.text(line, minimal_font, Point(6, 68));

    if (g_world.card_record) {
        screen.pen = Pen(255, 220, 90);
        screen.text("RECORD", minimal_font, Point(6, 77));
    }
    // In a tournament the weight is the point, so it goes on the card beside
    // the length: a card that only said 30cm would not tell a player whether
    // that fish moved them any closer to the day's quota.
    if (g_world.tour_state == kf::TourState::Running) {
        const uint32_t grams = kf::fish_weight_g(g_world.card_species,
                                                 g_world.card_size);
        snprintf(line, sizeof(line), "%u.%02ukg",
                 static_cast<unsigned>(grams / 1000),
                 static_cast<unsigned>((grams % 1000) / 10));
        screen.pen = Pen(150, 220, 255);
        screen.text(line, minimal_font,
                    Point(g_world.card_record ? 62 : 6, 77));
    }
}

// The tournament readout: which day, what it wants, what is in the boat. It
// sits along the top of the water, so it covers neither the sky scene nor the
// fight meters down the left edge.
void draw_tour_hud() {
    if (g_world.tour_state != kf::TourState::Running) return;
    char line[28];
    screen.pen = Pen(8, 6, 20, 170);
    screen.rectangle(Rect(0, 61, 120, 9));

    snprintf(line, sizeof(line), "DAY %d/%d", g_world.tour_day,
             kf::k_tour_days);
    screen.pen = Pen(255, 220, 90);
    screen.text(line, minimal_font, Point(3, 62));

    const bool made = g_world.tour_today_g >= g_world.tour_target_g;
    snprintf(line, sizeof(line), "%u.%02u/%u.%02ukg",
             static_cast<unsigned>(g_world.tour_today_g / 1000),
             static_cast<unsigned>((g_world.tour_today_g % 1000) / 10),
             static_cast<unsigned>(g_world.tour_target_g / 1000),
             static_cast<unsigned>((g_world.tour_target_g % 1000) / 10));
    screen.pen = made ? Pen(120, 255, 170) : Pen(230, 230, 240);
    screen.text(line, minimal_font, Point(44, 62));
}

// The day result, and the end of a run. One panel, because all three say the
// same kind of thing: here is where the tournament stands.
void draw_tour_card() {
    const kf::TourState state = g_world.tour_state;
    if (state == kf::TourState::Idle || state == kf::TourState::Running) return;

    screen.pen = Pen(8, 6, 20, 215);
    screen.rectangle(Rect(10, 34, 100, 48));

    char line[28];
    if (state == kf::TourState::DayPassed) {
        screen.pen = Pen(120, 255, 170);
        screen.text("DAY MADE", minimal_font, Point(42, 40));
        snprintf(line, sizeof(line), "DAY %d WANTS %u.%02ukg",
                 g_world.tour_day,
                 static_cast<unsigned>(g_world.tour_target_g / 1000),
                 static_cast<unsigned>((g_world.tour_target_g % 1000) / 10));
        screen.pen = Pen(230, 230, 240);
        screen.text(line, minimal_font, Point(14, 56));
        return;
    }

    if (state == kf::TourState::Won) {
        screen.pen = Pen(255, 220, 90);
        screen.text("TOURNAMENT WON", minimal_font, Point(20, 40));
    } else {
        screen.pen = Pen(255, 120, 120);
        screen.text("SHORT OF QUOTA", minimal_font, Point(20, 40));
        const int lasted = g_world.tour_day > 0 ? g_world.tour_day - 1 : 0;
        snprintf(line, sizeof(line), "LASTED %d DAY%s", lasted,
                 lasted == 1 ? "" : "S");
        screen.pen = Pen(200, 200, 215);
        screen.text(line, minimal_font, Point(30, 50));
    }
    snprintf(line, sizeof(line), "SCORE %u",
             static_cast<unsigned>(g_world.tour_score));
    screen.pen = Pen(255, 255, 238);
    screen.text(line, minimal_font, Point(36, 62));
    screen.pen = Pen(160, 155, 190);
    screen.text("A: TITLE", minimal_font, Point(38, 72));
}

void draw_scores() {
    screen.pen = Pen(8, 6, 20, 220);
    screen.rectangle(Rect(6, 4, 108, 112));
    screen.pen = Pen(255, 220, 90);
    screen.text("BEST TOURNAMENTS", minimal_font, Point(10, 7));

    char line[24];
    for (int i = 0; i < kf::k_high_scores; i++) {
        const int y = 20 + i * 9;
        const uint32_t score = g_world.records.high[i];
        screen.pen = score ? Pen(230, 230, 240) : Pen(90, 85, 120);
        snprintf(line, sizeof(line), "%2d.", i + 1);
        screen.text(line, minimal_font, Point(14, y));
        if (score) {
            snprintf(line, sizeof(line), "%u", static_cast<unsigned>(score));
            screen.text(line, minimal_font, Point(42, y));
        } else {
            screen.text("----", minimal_font, Point(42, y));
        }
    }
    screen.pen = Pen(160, 155, 190);
    screen.text("A: BACK", minimal_font, Point(14, 108));
}

void draw_menu_item(const char* label, int y, bool selected) {
    if (selected) {
        screen.pen = Pen(255, 220, 90);
        screen.text(">", minimal_font, Point(38, y));
    }
    screen.pen = selected ? Pen(255, 255, 238) : Pen(160, 155, 190);
    screen.text(label, minimal_font, Point(46, y));
}

void draw_title() {
    screen.pen = Pen(8, 6, 20, 190);
    screen.rectangle(Rect(14, 26, 92, 72));
    screen.pen = Pen(120, 200, 255);
    screen.text("KINGFISHER", minimal_font, Point(36, 32));

    draw_menu_item("FREE FISHING", 48, g_cursor == 0);
    draw_menu_item("TOURNAMENT", 58, g_cursor == 1);
    draw_menu_item("SCORES", 68, g_cursor == 2);
    draw_menu_item("OPTIONS", 78, g_cursor == 3);
}

void draw_options() {
    screen.pen = Pen(8, 6, 20, 190);
    screen.rectangle(Rect(14, 30, 92, 52));
    screen.pen = Pen(120, 200, 255);
    screen.text("OPTIONS", minimal_font, Point(42, 38));

    draw_menu_item(g_sound_off ? "SOUND: OFF" : "SOUND: ON", 56,
                   g_cursor == 0);
    draw_menu_item("BACK", 66, g_cursor == 1);
}

void toggle_sound() {
    g_sound_off = !g_sound_off;
    kfs::sfx_set_enabled(!g_sound_off);
    // The preference rides the same save as the records.
    g_world.save_pending = true;
}

// Menu navigation. Returns true when the shell consumed this tick's input.
bool update_shell() {
    if (g_shell == Shell::Play) return false;

    const bool up = (buttons.pressed & Button::DPAD_UP) != 0;
    const bool down = (buttons.pressed & Button::DPAD_DOWN) != 0;
    const bool pick = (buttons.pressed & Button::A) != 0;
    const bool flip = (buttons.pressed & (Button::DPAD_LEFT |
                                          Button::DPAD_RIGHT)) != 0;

    const int items = g_shell == Shell::Title ? 4
                    : g_shell == Shell::Options ? 2 : 1;
    if (up) g_cursor = (g_cursor + items - 1) % items;
    if (down) g_cursor = (g_cursor + 1) % items;

    if (g_shell == Shell::Scores) {
        if (pick) { g_shell = Shell::Title; g_cursor = 0; }
        return true;
    }

    if (g_shell == Shell::Title) {
        if (!pick) return true;
        switch (g_cursor) {
            case 0:
                kf::world_start(g_world, kf::GameMode::Free);
                g_shell = Shell::Play;
                break;
            case 1:
                kf::world_start(g_world, kf::GameMode::Tournament);
                g_shell = Shell::Play;
                break;
            case 2: g_shell = Shell::Scores; g_cursor = 0; break;
            default: g_shell = Shell::Options; g_cursor = 0; break;
        }
        return true;
    }

    if (g_cursor == 0 && (pick || flip)) toggle_sound();
    else if (pick && g_cursor == 1) { g_shell = Shell::Title; g_cursor = 0; }
    return true;
}

// A finished run goes on the board once, and the only way out of the result
// card is back to the title. Returns true when the tournament owns this
// tick's input, which is what stops A from also casting a line into a pond
// that is no longer being fished.
bool update_tournament() {
    if (g_world.ev.tour_won || g_world.ev.tour_lost) {
        kf::records_add_score(g_world.records, g_world.tour_score);
        g_world.save_pending = true;
        // Consumed here on purpose. A finished run stops the pond, so nothing
        // clears these for us, and a flag left standing would put the same
        // score on the board again on every frame the card is up until it had
        // pushed everything else off.
        g_world.ev.tour_won = false;
        g_world.ev.tour_lost = false;
    }
    const kf::TourState state = g_world.tour_state;
    if (state != kf::TourState::Lost && state != kf::TourState::Won) {
        return false;
    }
    if (buttons.pressed & Button::A) {
        kf::world_start(g_world, kf::GameMode::Free);
        g_shell = Shell::Title;
        g_cursor = 0;
    }
    return true;
}

}  // namespace

void init() {
    set_screen_mode(ScreenMode::lores);

    kfs::sfx_init();

    kf::SaveData data;
    const bool have_save = blit::read_save(data);
    // now() is near constant at boot, so fold the save in for seed variety
    // between sessions. Determinism per run is what matters, not the seed.
    uint32_t seed = blit::now() ^ 0x51F5EEDu;
    if (have_save) seed ^= data.records.score * 2654435761u;
    kf::world_init(g_world, seed);
    if (have_save) {
        kf::world_load(g_world, data);
        g_sound_off = data.sound_off != 0;
        kfs::sfx_set_enabled(!g_sound_off);
    }
}

void update(uint32_t time) {
    if (update_shell()) {
        // The pond idles behind the menu.
        kf::world_tick(g_world, kf::Input{});
        kfs::sfx_tick();
        save_if_safe();
        return;
    }

    if (buttons.pressed & Button::Y) g_show_records = !g_show_records;

    kf::Input input;
    input.a = (buttons & Button::A) != 0;
    input.a_pressed = (buttons.pressed & Button::A) != 0;
    input.a_released = (buttons.released & Button::A) != 0;
    input.b_pressed = (buttons.pressed & Button::B) != 0;
    input.left = (buttons & Button::DPAD_LEFT) != 0;
    input.right = (buttons & Button::DPAD_RIGHT) != 0;
    input.left_pressed = (buttons.pressed & Button::DPAD_LEFT) != 0;
    input.right_pressed = (buttons.pressed & Button::DPAD_RIGHT) != 0;

    if (g_show_records) {
        // The pond pauses while the book is open. Letting it run with the
        // input zeroed would pay out line to a hooked fish and lose it while
        // the player reads their own records, which is not a fair trade.
        kfs::sfx_tick();
        save_if_safe();
        return;
    }

    if (update_tournament()) {
        kfs::sfx_tick();
        save_if_safe();
        return;
    }

    kf::world_tick(g_world, input);
    kfs::sfx_handle(g_world.ev);
    kfs::sfx_tick();
    save_if_safe();
}

void render(uint32_t time) {
    kfr::render_scene(g_world, pse::target_from_screen(), time);

    if (g_shell == Shell::Title) { draw_title(); return; }
    if (g_shell == Shell::Options) { draw_options(); return; }
    if (g_shell == Shell::Scores) { draw_scores(); return; }

    draw_fight_distance();
    draw_tour_hud();
    draw_card();
    draw_tour_card();
    if (g_show_records) draw_records_overlay();
}
