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
enum class Shell : uint8_t { Title, Options, Play };

kf::World g_world;
Shell g_shell = Shell::Title;
int g_cursor = 0;
bool g_sound_off = false;
bool g_show_records = false;

// The sim's RAM footprint is a hard promise, checked at compile time. If this
// fails, something grew without its cost being paid attention to.
static_assert(sizeof(kf::World) <= 768, "sim state grew past its RAM budget");
static_assert(sizeof(kf::SaveData) <= 64, "save record grew");

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

void draw_card() {
    if (g_world.mode != kf::Mode::Landed || g_world.card_species < 0) return;
    const kf::Species& s = kf::k_species[g_world.card_species];

    screen.pen = Pen(8, 6, 20, 200);
    screen.rectangle(Rect(0, 96, 120, 24));

    char line[28];
    snprintf(line, sizeof(line), "%s %dcm", s.name, g_world.card_size);
    screen.pen = Pen(255, 255, 238);
    screen.text(line, minimal_font, Point(6, 100));
    if (g_world.card_record) {
        screen.pen = Pen(255, 220, 90);
        screen.text("RECORD", minimal_font, Point(6, 109));
    }
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
    screen.rectangle(Rect(14, 30, 92, 52));
    screen.pen = Pen(120, 200, 255);
    screen.text("KINGFISHER", minimal_font, Point(36, 38));

    draw_menu_item("START", 56, g_cursor == 0);
    draw_menu_item("OPTIONS", 66, g_cursor == 1);
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
    if (up || down) g_cursor ^= 1;

    if (g_shell == Shell::Title) {
        if (pick && g_cursor == 0) g_shell = Shell::Play;
        else if (pick && g_cursor == 1) { g_shell = Shell::Options; g_cursor = 0; }
    } else {
        if (g_cursor == 0 && (pick || flip)) toggle_sound();
        else if (pick && g_cursor == 1) { g_shell = Shell::Title; g_cursor = 0; }
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

    kf::world_tick(g_world, input);
    kfs::sfx_handle(g_world.ev);
    kfs::sfx_tick();
    save_if_safe();
}

void render(uint32_t time) {
    kfr::render_scene(g_world, pse::target_from_screen(), time);

    if (g_shell == Shell::Title) { draw_title(); return; }
    if (g_shell == Shell::Options) { draw_options(); return; }

    draw_card();
    if (g_show_records) draw_records_overlay();
}
