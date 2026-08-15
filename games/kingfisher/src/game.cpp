#include <cstdio>

#include "32blit.hpp"

#include "pse/blit_target.hpp"
#include "pse/game.hpp"

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

// The screen is 120x120 (lores) and text is minimal_font, which is as small
// as text goes here: there is no smaller font and the glyph is already 3x5.
//
// Drawing text at the panel's native 240x240 would halve its apparent size,
// and it was tried. The 3D cost is fine, because the scene still rasterizes
// at 120x120 and is stretched over the screen, but two 240x240 pages do not
// fit in 264 KB, so the display loses its second buffer and every frame also
// pays a 57,600 pixel expand. Not worth it for smaller text.
//
// Panels and menus are built outwards from the centre and sized from
// measure_text, never from a hand picked x: a tuned x is only correct for the
// exact string it was tuned against, and the first wording change prints
// through the edge of its own panel, which is how "FREE FISHING" came to run
// out through the side of the title menu.
constexpr int k_screen_w = 120;
constexpr int k_screen_h = 120;
constexpr int k_screen_cx = k_screen_w / 2;

// One row of minimal_font plus breathing room, and the glyph height itself.
constexpr int k_row = 10;
constexpr int k_text_h = 6;
// The cursor hangs in a gutter left of the item, and the panel holds both.
constexpr int k_cursor_gutter = 8;
constexpr int k_panel_pad = 4;

// The widest of a set of labels, for sizing a panel from what goes in it.
int widest_text(const char* const* labels, int count) {
    int widest = 0;
    for (int i = 0; i < count; i++) {
        const int w = screen.measure_text(labels[i], minimal_font).w;
        if (w > widest) widest = w;
    }
    return widest;
}

// A centred panel wide enough for `labels`, drawn at `y` for `h` rows.
void draw_panel(const char* const* labels, int count, int y, int h) {
    const int w = widest_text(labels, count) +
                  2 * (k_cursor_gutter + k_panel_pad);
    screen.pen = Pen(8, 6, 20, 190);
    screen.rectangle(Rect(k_screen_cx - w / 2, y, w, h));
}

void draw_centred(const char* text, int y) {
    const int w = screen.measure_text(text, minimal_font).w;
    screen.text(text, minimal_font, Point(k_screen_cx - w / 2, y));
}

void draw_records_overlay() {
    const int panel_x = 6;
    const int panel_w = k_screen_w - 2 * panel_x;
    const int rows_top = 12;
    const int row_h = 8;   // tighter than k_row: this list has to fit 120 rows

    screen.pen = Pen(8, 6, 20, 220);
    screen.rectangle(Rect(panel_x, 4, panel_w, 112));
    screen.pen = Pen(255, 255, 238);
    screen.text("RECORDS", minimal_font, Point(panel_x + 4, 7));

    const int name_x = panel_x + 4;
    const int stat_x = panel_x + 60;
    char line[24];
    for (int i = 0; i < kf::k_species_count; i++) {
        const kf::Species& s = kf::k_species[i];
        const int y = 4 + rows_top + i * row_h;
        if (g_world.records.caught[i] == 0) {
            screen.pen = Pen(90, 85, 120);
            screen.text("--------", minimal_font, Point(name_x, y));
            continue;
        }
        screen.pen = Pen(s.r, s.g, s.b);
        screen.text(s.name, minimal_font, Point(name_x, y));
        snprintf(line, sizeof(line), "%dcm x%d",
                 g_world.records.best_cm[i], g_world.records.caught[i]);
        screen.pen = Pen(210, 210, 220);
        screen.text(line, minimal_font, Point(stat_x, y));
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
    // Pinned to the bottom right corner and sized to the reading, which grows
    // a digit past 9.9m.
    const int text_w = screen.measure_text(line, minimal_font).w;
    const int box_w = text_w + 2 * k_panel_pad;
    const int box_x = k_screen_w - box_w - 2;
    const int box_y = 101;
    screen.pen = Pen(8, 6, 20, 170);
    screen.rectangle(Rect(box_x, box_y, box_w, k_text_h + 4));
    screen.pen = Pen(255, 255, 238);
    screen.text(line, minimal_font, Point(box_x + k_panel_pad, box_y + 2));
}

void draw_card() {
    if (g_world.mode != kf::Mode::Landed || g_world.card_species < 0) return;
    const kf::Species& s = kf::k_species[g_world.card_species];

    // A card floating over the scene, like the menus, not a bar living inside
    // the lower viewport. It used to be a full width strip pinned under the
    // water line, which read as text jammed into the bottom half rather than
    // as a card, and left its first line tight against that line.
    //
    // It still hangs low enough to leave the trophy fish alone: the fish is
    // held up in the middle of the top viewport, and a card centred on the
    // screen would cover the belly of exactly the biggest ones worth showing.
    // Crossing the split by a couple of rows is what makes it read as an
    // overlay on both viewports rather than as furniture in one.
    char line[28];
    snprintf(line, sizeof(line), "%s %dcm", s.name, g_world.card_size);

    char weight[12];
    weight[0] = '\0';
    // In a tournament the weight is the point: a card that only said 30cm
    // would not tell a player whether that fish moved them any closer to the
    // day's quota.
    if (g_world.tour_state == kf::TourState::Running) {
        const uint32_t grams = kf::fish_weight_g(g_world.card_species,
                                                 g_world.card_size);
        snprintf(weight, sizeof(weight), "%u.%02ukg",
                 static_cast<unsigned>(grams / 1000),
                 static_cast<unsigned>((grams % 1000) / 10));
    }

    // The second row carries the record flag and the weight side by side, so
    // its width is both of them plus the gap between, and the card is sized
    // from whichever row is wider.
    constexpr int k_gap = 4;
    const int record_w = g_world.card_record
        ? screen.measure_text("RECORD", minimal_font).w : 0;
    const int weight_w = weight[0]
        ? screen.measure_text(weight, minimal_font).w : 0;
    int second_w = record_w + weight_w;
    if (record_w && weight_w) second_w += k_gap;

    const int name_w = screen.measure_text(line, minimal_font).w;
    const int content_w = name_w > second_w ? name_w : second_w;
    const int card_w = content_w + 2 * k_panel_pad + 2;
    const int card_h = second_w > 0 ? 23 : 14;
    // Just above the water line, which is halfway down the screen.
    constexpr int k_card_y = 58;

    screen.pen = Pen(8, 6, 20, 200);
    screen.rectangle(Rect(k_screen_cx - card_w / 2, k_card_y, card_w, card_h));

    screen.pen = Pen(255, 255, 238);
    draw_centred(line, k_card_y + 4);

    if (second_w > 0) {
        int x = k_screen_cx - second_w / 2;
        const int y = k_card_y + 4 + k_row;
        if (record_w) {
            screen.pen = Pen(255, 220, 90);
            screen.text("RECORD", minimal_font, Point(x, y));
            x += record_w + k_gap;
        }
        if (weight_w) {
            screen.pen = Pen(150, 220, 255);
            screen.text(weight, minimal_font, Point(x, y));
        }
    }
}

// A legibility patch sized to one string, drawn under it.
//
// The tournament readout used to sit on a panel across the full 120 pixels and
// 17 rows deep, which is over a quarter of the 60 row sky viewport. At 2pm that
// turned a (95,155,225) daylight blue into (37,55,88) and read as dusk: the sky
// palette was right and the HUD was lying about it. Something still has to sit
// behind the glyphs, because a (120,200,255) clock on that same blue is nearly
// invisible, so each string now carries its own patch a pixel proud of itself
// and nothing at all covers the gaps between them.
void hud_text(const char* s, int x, int y, Pen ink) {
    const int w = screen.measure_text(s, minimal_font).w;
    screen.pen = Pen(8, 6, 20, 150);
    screen.rectangle(Rect(x - 1, y - 1, w + 2, k_text_h + 2));
    screen.pen = ink;
    screen.text(s, minimal_font, Point(x, y));
}

// The tournament readout: which day, what it wants, what is in the boat, and
// the time. It runs along the very top of the sky viewport and never meets the
// catch card down in the water.
//
// The clock is here because the day ends at midnight: a quota with no time
// against it says how far there is to go and nothing about how long there is
// to do it in.
void draw_tour_hud() {
    if (g_world.tour_state != kf::TourState::Running) return;
    char line[28];
    // Two lines: which day it is and what time, then what the day wants and
    // what is in the boat. One line held all four readings across 120 pixels
    // with about 16 spare, which left no room for a target that reaches 10kg
    // and read as a wall of digits.
    constexpr int k_line1 = 1;
    constexpr int k_line2 = k_line1 + k_text_h + 2;

    // 12 hour clock, hours only. The hour is the unit the day is worth
    // reading in: 18 of them, each a few seconds of play, and a minute field
    // would be four more characters of a 120 pixel line spent on noise.
    const uint16_t mins = kf::clock_minutes(g_world);
    const int hour24 = mins / 60;
    const int hour12 = (hour24 % 12) == 0 ? 12 : (hour24 % 12);
    char clock[8];
    snprintf(clock, sizeof(clock), "%d%s", hour12, hour24 >= 12 ? "pm" : "am");

    // Laid out from the right edge inwards, measured rather than guessed: the
    // quota string grows a digit the moment a target reaches 10kg, and a
    // hand placed x would print that through the edge of the panel.
    constexpr int k_left = 3;
    const int k_right = k_screen_w - 3;

    // Line one: the day, and the clock hard against the right edge.
    snprintf(line, sizeof(line), "DAY %d/%d", g_world.tour_day,
             kf::k_tour_days);
    hud_text(line, k_left, k_line1, Pen(255, 220, 90));

    const int clock_w = screen.measure_text(clock, minimal_font).w;
    hud_text(clock, k_right - clock_w, k_line1, Pen(120, 200, 255));

    // Line two: the day's target and what is against it, under the day it
    // belongs to. Left justified so the leading digit sits at a fixed x and
    // the number does not walk sideways as it climbs.
    const bool made = g_world.tour_today_g >= g_world.tour_target_g;
    snprintf(line, sizeof(line), "%u.%02u/%u.%02ukg",
             static_cast<unsigned>(g_world.tour_today_g / 1000),
             static_cast<unsigned>((g_world.tour_today_g % 1000) / 10),
             static_cast<unsigned>(g_world.tour_target_g / 1000),
             static_cast<unsigned>((g_world.tour_target_g % 1000) / 10));
    hud_text(line, k_left, k_line2,
             made ? Pen(120, 255, 170) : Pen(230, 230, 240));
}

// The day result, and the end of a run. One panel, because all three say the
// same kind of thing: here is where the tournament stands.
void draw_tour_card() {
    const kf::TourState state = g_world.tour_state;
    if (state == kf::TourState::Idle || state == kf::TourState::Running) return;

    // Every line is centred by measurement. These x positions used to be hand
    // picked per string (42 for one heading, 20 for another, 30 for a third),
    // which is only ever correct for the exact wording it was tuned against.
    const int panel_h = 48;
    const int panel_y = 34;
    const int top = panel_y + 6;
    screen.pen = Pen(8, 6, 20, 215);
    screen.rectangle(Rect(10, panel_y, k_screen_w - 20, panel_h));

    char line[28];
    if (state == kf::TourState::DayPassed) {
        screen.pen = Pen(120, 255, 170);
        draw_centred("DAY MADE", top);
        snprintf(line, sizeof(line), "DAY %d WANTS %u.%02ukg",
                 g_world.tour_day,
                 static_cast<unsigned>(g_world.tour_target_g / 1000),
                 static_cast<unsigned>((g_world.tour_target_g % 1000) / 10));
        screen.pen = Pen(230, 230, 240);
        draw_centred(line, top + 2 * k_row);
        return;
    }

    if (state == kf::TourState::Won) {
        screen.pen = Pen(255, 220, 90);
        draw_centred("TOURNAMENT WON", top);
    } else {
        screen.pen = Pen(255, 120, 120);
        draw_centred("SHORT OF QUOTA", top);
        const int lasted = g_world.tour_day > 0 ? g_world.tour_day - 1 : 0;
        snprintf(line, sizeof(line), "LASTED %d DAY%s", lasted,
                 lasted == 1 ? "" : "S");
        screen.pen = Pen(200, 200, 215);
        draw_centred(line, top + k_row);
    }
    snprintf(line, sizeof(line), "SCORE %u",
             static_cast<unsigned>(g_world.tour_score));
    screen.pen = Pen(255, 255, 238);
    draw_centred(line, top + 2 * k_row);
    screen.pen = Pen(160, 155, 190);
    draw_centred("A: TITLE", top + 3 * k_row);
}

void draw_scores() {
    const int panel_x = 6;
    const int panel_w = k_screen_w - 2 * panel_x;
    const int rows_top = 16;
    const int row_h = 9;

    screen.pen = Pen(8, 6, 20, 220);
    screen.rectangle(Rect(panel_x, 4, panel_w, 112));
    screen.pen = Pen(255, 220, 90);
    draw_centred("BEST TOURNAMENTS", 7);

    const int rank_x = panel_x + 8;
    const int score_x = panel_x + 36;
    char line[24];
    for (int i = 0; i < kf::k_high_scores; i++) {
        const int y = 4 + rows_top + i * row_h;
        const uint32_t score = g_world.records.high[i];
        screen.pen = score ? Pen(230, 230, 240) : Pen(90, 85, 120);
        snprintf(line, sizeof(line), "%2d.", i + 1);
        screen.text(line, minimal_font, Point(rank_x, y));
        if (score) {
            snprintf(line, sizeof(line), "%u", static_cast<unsigned>(score));
            screen.text(line, minimal_font, Point(score_x, y));
        } else {
            screen.text("----", minimal_font, Point(score_x, y));
        }
    }
    screen.pen = Pen(160, 155, 190);
    screen.text("A: BACK", minimal_font, Point(rank_x, 108));
}

void draw_menu_item(const char* label, int y, bool selected) {
    const int w = screen.measure_text(label, minimal_font).w;
    const int x = k_screen_cx - w / 2;
    if (selected) {
        screen.pen = Pen(255, 220, 90);
        screen.text(">", minimal_font, Point(x - k_cursor_gutter, y));
    }
    screen.pen = selected ? Pen(255, 255, 238) : Pen(160, 155, 190);
    screen.text(label, minimal_font, Point(x, y));
}

// Everything here is centred on k_screen_cx and the panel is measured from
// its own contents. It used to be a 92 pixel panel with items at a fixed
// x = 46, and "FREE FISHING" printed straight out through the right edge:
// the width was tuned for wording that had since grown.
const char* const k_title_items[] = {
    "KINGFISHER", "FREE FISHING", "TOURNAMENT", "SCORES", "OPTIONS",
};

// A menu panel: a heading, then `count` rows, sized to hold exactly that and
// centred on the screen. Returns the y of the first row.
int draw_menu_panel(const char* const* labels, int label_count, int rows) {
    const int head_h = 22;
    const int panel_h = head_h + rows * k_row + 8;
    const int panel_y = (k_screen_h - panel_h) / 2;
    draw_panel(labels, label_count, panel_y, panel_h);
    screen.pen = Pen(120, 200, 255);
    draw_centred(labels[0], panel_y + 6);
    return panel_y + head_h;
}

void draw_title() {
    const int row = draw_menu_panel(k_title_items, 5, 4);
    draw_menu_item("FREE FISHING", row, g_cursor == 0);
    draw_menu_item("TOURNAMENT", row + k_row, g_cursor == 1);
    draw_menu_item("SCORES", row + 2 * k_row, g_cursor == 2);
    draw_menu_item("OPTIONS", row + 3 * k_row, g_cursor == 3);
}

void draw_options() {
    // Both sound wordings, so the panel does not resize as it is toggled.
    const char* const items[] = {
        "OPTIONS", "SOUND: OFF", "SOUND: ON", "BACK",
    };
    const int row = draw_menu_panel(items, 4, 2);
    draw_menu_item(g_sound_off ? "SOUND: OFF" : "SOUND: ON", row,
                   g_cursor == 0);
    draw_menu_item("BACK", row + k_row, g_cursor == 1);
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

void game_init() {
    pse::set_screen_mode(pse::ScreenMode::lores);

    // Every entry, not once per boot: the console calls this each time the
    // game is picked, so the shell has to be put back to its title screen
    // rather than resuming wherever the last session was left.
    g_shell = Shell::Title;
    g_cursor = 0;
    g_show_records = false;

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

void game_update(uint32_t time) {
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
    input.up = (buttons & Button::DPAD_UP) != 0;
    input.down = (buttons & Button::DPAD_DOWN) != 0;

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

void game_render(uint32_t time) {
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

}  // namespace

// The one symbol this game exports. Everything above is internal linkage, so
// the console can link it beside three other games that also have a g_world.
PSE_GAME(kingfisher, game_init, game_update, game_render);
