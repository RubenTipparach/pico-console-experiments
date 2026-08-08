// Host tests for the console's menu.
//
// The menu is the one screen with no gameplay to cover a mistake in it, and
// on a device the only way to find a mistake is to flash and look. So the
// parts that can be wrong quietly are asserted here: the cursor stepping over
// headings, wrapping, the list following the cursor, every name in the real
// generated library being drawable at all, and the two things that are pixel
// facts rather than arithmetic, a long name sliding inside its row and the
// battery sitting inside its header.

#include <cstdio>
#include <cstring>
#include <vector>

#include "library.hpp"
#include "menu.hpp"
#include "pse/text.hpp"

namespace {

int g_failures = 0;

void check(bool condition, const char* what) {
    if (!condition) {
        std::printf("FAIL %s\n", what);
        g_failures++;
    }
}

const pse::Game k_dummy{nullptr, nullptr, nullptr};

console::Entry entry(const char* name, const pse::Game* game) {
    console::Entry e{};
    e.name = name;
    e.slug = name;
    e.game = game;
    e.icon = nullptr;
    return e;
}

console::Menu::Input down() {
    console::Menu::Input input;
    input.down = true;
    return input;
}

console::Menu::Input up() {
    console::Menu::Input input;
    input.up = true;
    return input;
}

// A frame of the real 240x240 layout. The sliding name and the battery are
// drawn rather than computed, so the only honest test of either is what came
// out of the drawing.
struct Frame {
    static constexpr int k_w = 240;
    static constexpr int k_h = 240;

    std::vector<uint8_t> rgb =
        std::vector<uint8_t>(static_cast<size_t>(k_w) * k_h * 3, 0);

    pse::RenderTarget target() {
        return pse::RenderTarget{rgb.data(), k_w, k_h, k_w * 3,
                                 pse::PixelFormat::rgb888};
    }

    bool white(int x, int y) const {
        const size_t at = (static_cast<size_t>(y) * k_w + x) * 3;
        return rgb[at] == 255 && rgb[at + 1] == 255 && rgb[at + 2] == 255;
    }

    bool differs(const Frame& other, int x, int y) const {
        const size_t at = (static_cast<size_t>(y) * k_w + x) * 3;
        return rgb[at] != other.rgb[at] || rgb[at + 1] != other.rgb[at + 1] ||
               rgb[at + 2] != other.rgb[at + 2];
    }
};

// The cursor never lands on a heading, in either direction, however many
// headings are stacked together.
void test_headings_are_skipped() {
    std::vector<console::Entry> rows{
        entry("GROUP ONE", nullptr),
        entry("ALPHA", &k_dummy),
        entry("GROUP TWO", nullptr),
        entry("GROUP THREE", nullptr),
        entry("BETA", &k_dummy),
    };
    console::Menu menu(rows.data(), static_cast<int>(rows.size()));
    menu.reset();
    check(menu.cursor() == 1, "opens on the first game, not the heading above it");

    menu.update(down());
    check(menu.cursor() == 4, "steps over two headings in a row");

    menu.update(down());
    check(menu.cursor() == 1, "wraps past the end back to the first game");

    menu.update(up());
    check(menu.cursor() == 4, "wraps backwards over the headings too");
}

// A library with nothing selectable must not spin or run off the end.
void test_all_headings() {
    std::vector<console::Entry> rows{
        entry("ONE", nullptr),
        entry("TWO", nullptr),
    };
    console::Menu menu(rows.data(), static_cast<int>(rows.size()));
    menu.reset();
    menu.update(down());
    check(menu.cursor() >= 0 && menu.cursor() < 2, "cursor stays in range");
    check(menu.update(down()) == -1, "nothing is selectable");
}

// Selecting returns the row, and only when the row is a game.
void test_select() {
    std::vector<console::Entry> rows{
        entry("HEADING", nullptr),
        entry("GAME", &k_dummy),
    };
    console::Menu menu(rows.data(), static_cast<int>(rows.size()));
    menu.reset();

    console::Menu::Input select;
    select.select = true;
    check(menu.update(select) == 1, "selecting returns the row index");

    console::Menu::Input nothing;
    check(menu.update(nothing) == -1, "no button, no game");
}

// The window follows the cursor and stops at both ends.
void test_scrolling() {
    std::vector<console::Entry> rows;
    static char names[40][8];
    for (int i = 0; i < 40; i++) {
        std::snprintf(names[i], sizeof(names[i]), "G%d", i);
        rows.push_back(entry(names[i], &k_dummy));
    }
    console::Menu menu(rows.data(), static_cast<int>(rows.size()));
    menu.reset();
    check(menu.scroll() == 0, "opens at the top");

    for (int i = 0; i < console::Menu::k_rows_visible; i++) menu.update(down());
    check(menu.cursor() == console::Menu::k_rows_visible,
          "cursor moved past the last visible row");
    check(menu.scroll() == 1, "the list scrolled by exactly one row");
    check(menu.cursor() >= menu.scroll(), "cursor is not above the window");
    check(menu.cursor() < menu.scroll() + console::Menu::k_rows_visible,
          "cursor is not below the window");

    // Wrapping from the last row to the first pulls the window back with it.
    menu.reset();
    menu.update(up());
    check(menu.cursor() == 39, "up from the first row wraps to the last");
    check(menu.scroll() == 40 - console::Menu::k_rows_visible,
          "the window followed to the bottom");
}

// Reopening on the last game played.
void test_resume() {
    std::vector<console::Entry> rows{
        entry("ALPHA", &k_dummy),
        entry("BETA", &k_dummy),
    };
    console::Menu menu(rows.data(), static_cast<int>(rows.size()));
    menu.reset("BETA");
    check(menu.cursor() == 1, "resumes on the remembered slug");

    menu.reset("NOT INSTALLED");
    check(menu.cursor() == 0, "a slug that is no longer here starts at the top");

    menu.reset(nullptr);
    check(menu.cursor() == 0, "no memory starts at the top");
}

// Every name in the library console.yaml actually produced can be drawn at
// all, and the title fits the header it does not scroll in.
// tools/gen_library.py refuses both at build time; this is what proves its
// numbers are the menu's numbers, since a title that overflows still compiles
// and still runs.
void test_real_library_draws() {
    check(console::k_library_count > 0, "the generated library has rows in it");
    for (int i = 0; i < console::k_library_count; i++) {
        const console::Entry& e = console::k_library[i];
        check(pse::text_is_drawable(e.name), e.name);
    }
    check(pse::text_is_drawable(console::k_console_title),
          "the console title is drawable");
    if (pse::text_width(console::k_console_title, 2) >
        console::Menu::k_title_room) {
        std::printf("FAIL the title %s is %d pixels wide, the header has %d\n",
                    console::k_console_title,
                    pse::text_width(console::k_console_title, 2),
                    console::Menu::k_title_room);
        g_failures++;
    }
}

// A name wider than its row slides, and never outside the row.
//
// This is what replaced refusing to build such a name, so it is the check
// that has to hold: white is the selected row's text colour and nothing else
// in the menu draws pure white, so a white pixel outside the name's own
// columns is a name printing over the icon or through the scrollbar.
void test_a_long_name_slides_inside_its_row() {
    // Mirrors menu.cpp's k_name_x. The width it has from there is published
    // by the menu itself.
    constexpr int k_name_x = 38;
    static const char* k_long = "A VERY LONG GAME NAME THAT KEEPS ON GOING";

    check(pse::text_width(k_long, 2) > console::Menu::k_name_room,
          "the fixture is wider than a row, or this test proves nothing");

    std::vector<console::Entry> rows{entry(k_long, &k_dummy)};
    console::Menu menu(rows.data(), 1);
    menu.reset();

    Frame first;
    Frame later;
    menu.draw(first.target(), 0);
    menu.draw(later.target(), 2500);
    check(first.rgb != later.rgb, "a name too wide for its row moves with time");

    const uint32_t times[] = {0, 800, 1500, 2500, 4000, 7000, 30000};
    for (uint32_t time_ms : times) {
        Frame frame;
        menu.draw(frame.target(), time_ms);
        int inside = 0;
        int outside = 0;
        for (int y = 0; y < Frame::k_h; y++) {
            for (int x = 0; x < Frame::k_w; x++) {
                if (!frame.white(x, y)) continue;
                if (x >= k_name_x && x < k_name_x + console::Menu::k_name_room) {
                    inside++;
                } else {
                    outside++;
                }
            }
        }
        if (outside != 0) {
            std::printf("FAIL at %u ms the name put %d pixels outside its row\n",
                        time_ms, outside);
            g_failures++;
        }
        check(inside > 0, "the name is drawn at all");
    }

    // A name that fits stays put, so a short menu is a still one.
    std::vector<console::Entry> shorts{entry("GAME", &k_dummy)};
    console::Menu still(shorts.data(), 1);
    still.reset();
    Frame a;
    Frame b;
    still.draw(a.target(), 0);
    still.draw(b.target(), 5000);
    check(a.rgb == b.rgb, "a name that fits does not slide");
}

// The battery draws only when there is a level to draw, and stays in the
// header. A percent of -1 is the desktop build, where there is no cell.
void test_the_battery_stays_in_the_header() {
    // Mirrors menu.cpp: 20 wide, right edge on the scrollbar's, in the 22px
    // header band.
    constexpr int k_batt_x = 216;
    constexpr int k_batt_right = 236;
    constexpr int k_header_h = 22;

    std::vector<console::Entry> rows{entry("GAME", &k_dummy)};
    console::Menu menu(rows.data(), 1);
    menu.reset();

    Frame unknown;
    menu.draw(unknown.target(), 0);

    console::Battery charging;
    charging.percent = 64;
    charging.charging = true;
    menu.set_battery(charging);
    Frame shown;
    menu.draw(shown.target(), 0);

    int changed = 0;
    int stray = 0;
    for (int y = 0; y < Frame::k_h; y++) {
        for (int x = 0; x < Frame::k_w; x++) {
            if (!shown.differs(unknown, x, y)) continue;
            changed++;
            if (y >= k_header_h || x < k_batt_x || x >= k_batt_right) stray++;
        }
    }
    check(changed > 0, "a known level draws an icon where -1 drew nothing");
    check(stray == 0, "the battery stays inside the header, right of the title");

    console::Battery flat;
    flat.percent = 5;
    menu.set_battery(flat);
    Frame low;
    menu.draw(low.target(), 0);
    check(low.rgb != shown.rgb, "the fill and its colour follow the level");
}

// Drawing must stay inside the buffer it was given, at any size. The device
// hands it 240x240; a mistake here is memory corruption, not a wrong pixel.
void test_draw_clips() {
    constexpr int k_w = 64;
    constexpr int k_h = 48;
    constexpr int k_guard = 32;
    std::vector<uint8_t> buffer(static_cast<size_t>(k_w) * k_h * 3 + k_guard,
                                0xAB);
    pse::RenderTarget target{buffer.data(), k_w, k_h, k_w * 3,
                             pse::PixelFormat::rgb888};

    console::Menu menu(console::k_library, console::k_library_count);
    menu.reset();
    // With a battery to draw, since the icon is laid out from the right edge
    // of a 240 wide screen and this target has no such edge.
    console::Battery battery;
    battery.percent = 50;
    battery.charging = true;
    menu.set_battery(battery);
    menu.draw(target, 3000);

    bool intact = true;
    for (size_t i = buffer.size() - k_guard; i < buffer.size(); i++) {
        if (buffer[i] != 0xAB) intact = false;
    }
    check(intact, "drawing a 240 wide menu into a 64 wide target stays inside it");
}

}  // namespace

int main() {
    test_headings_are_skipped();
    test_all_headings();
    test_select();
    test_scrolling();
    test_resume();
    test_real_library_draws();
    test_a_long_name_slides_inside_its_row();
    test_the_battery_stays_in_the_header();
    test_draw_clips();

    if (g_failures == 0) {
        std::printf("console menu tests passed\n");
        return 0;
    }
    std::printf("%d console menu checks failed\n", g_failures);
    return 1;
}
