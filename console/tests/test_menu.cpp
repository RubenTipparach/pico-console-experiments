// Host tests for the console's menu.
//
// The menu is the one screen with no gameplay to cover a mistake in it, and
// on a device the only way to find a mistake is to flash and look. So the
// parts that can be wrong quietly are asserted here: the cursor stepping over
// headings, wrapping, the list following the cursor, and every name in the
// real generated library fitting the row it is drawn in.

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

// Every name in the library console.yaml actually produced fits the row it is
// drawn in, and is drawable at all. tools/gen_library.py refuses both at build
// time; this is what proves its numbers are the menu's numbers, since a name
// that overflows still compiles and still runs.
void test_real_library_fits() {
    // Mirrors menu.cpp: k_scrollbar_x - 6 - k_name_x, name drawn at scale 2.
    constexpr int k_name_room = 233 - 6 - 38;

    check(console::k_library_count > 0, "the generated library has rows in it");
    for (int i = 0; i < console::k_library_count; i++) {
        const console::Entry& e = console::k_library[i];
        check(pse::text_is_drawable(e.name), e.name);
        const int scale = e.game == nullptr ? 1 : 2;
        if (pse::text_width(e.name, scale) > k_name_room) {
            std::printf("FAIL %s is %d pixels wide, the row has %d\n", e.name,
                        pse::text_width(e.name, scale), k_name_room);
            g_failures++;
        }
    }
    check(pse::text_is_drawable(console::k_console_title),
          "the console title is drawable");
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
    menu.draw(target);

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
    test_real_library_fits();
    test_draw_clips();

    if (g_failures == 0) {
        std::printf("console menu tests passed\n");
        return 0;
    }
    std::printf("%d console menu checks failed\n", g_failures);
    return 1;
}
