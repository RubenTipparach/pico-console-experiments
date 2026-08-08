// Renders the console's real menu on a host and writes the frames out as
// pictures.
//
// This is the whole reason the menu draws through a pse::RenderTarget instead
// of calling the SDK: the first screen a player meets can be looked at, and
// looked at again after a change, without a device in hand and without
// flashing anything. Same code, same font, same icons, same layout numbers as
// the PicoSystem runs.
//
// Usage: console_menu_preview [out_dir]

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "library.hpp"
#include "menu.hpp"
#include "pse/pixel.hpp"

namespace {

constexpr int k_w = 240;
constexpr int k_h = 240;

void write_ppm(const std::string& path, const uint8_t* rgb) {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (f == nullptr) {
        std::printf("could not write %s\n", path.c_str());
        return;
    }
    std::fprintf(f, "P6\n%d %d\n255\n", k_w, k_h);
    std::fwrite(rgb, 1, static_cast<size_t>(k_w) * k_h * 3, f);
    std::fclose(f);
    std::printf("wrote %s\n", path.c_str());
}

void capture(const console::Menu& menu, const std::string& path,
             uint32_t time_ms = 0) {
    static std::vector<uint8_t> buffer(static_cast<size_t>(k_w) * k_h * 3);
    pse::RenderTarget target{buffer.data(), k_w, k_h, k_w * 3,
                             pse::PixelFormat::rgb888};
    menu.draw(target, time_ms);
    write_ppm(path, buffer.data());
}

console::Battery battery(int percent, bool charging = false) {
    console::Battery state;
    state.percent = percent;
    state.charging = charging;
    return state;
}

// A library longer than the screen, to prove the scrolling and the scrollbar.
// Names only: an entry with no icon draws the "no art yet" tile, which is
// worth seeing too.
console::Entry make_entry(const char* name, const char* slug,
                          const pse::Game* game) {
    console::Entry entry{};
    entry.name = name;
    entry.slug = slug;
    entry.game = game;
    entry.icon = nullptr;
    return entry;
}

}  // namespace

int main(int argc, char** argv) {
    const std::string out = argc > 1 ? argv[1] : ".";

    // 1: the console as it opens, on the real library from console.yaml,
    // about two thirds charged.
    {
        console::Menu menu(console::k_library, console::k_library_count);
        menu.reset();
        menu.set_battery(battery(68));
        capture(menu, out + "/menu_1_open.ppm");
    }

    // 2: opened again after playing Picomon. The cursor is where it was left
    // and the dot says so, and this one is on the charger.
    {
        console::Menu menu(console::k_library, console::k_library_count);
        menu.reset("picomon");
        menu.set_battery(battery(23, true));
        capture(menu, out + "/menu_2_resumed.ppm");
    }

    // 3: the cursor stepping over a heading onto the game below it.
    {
        console::Menu menu(console::k_library, console::k_library_count);
        menu.reset("picomon");
        console::Menu::Input down;
        down.down = true;
        menu.update(down);
        capture(menu, out + "/menu_3_stepped.ppm");
    }

    // 4: a longer library than the screen holds, scrolled to the bottom.
    {
        static const pse::Game dummy{nullptr, nullptr, nullptr};
        static std::vector<console::Entry> many;
        static const char* names[] = {
            "ARCADE",    "DUST RIDER", "KINGFISHER", "PICO SANTA",
            "CHICKEN",   "PUZZLE",     "HEXMIN",     "COLOR ROLL",
            "BALL TOUR", "PAKU PAKU",  "SHOOTERS",   "B CANNON",
            "THUNDER",   "SURVIVOR",   "REFLECTOR",  "LADDER DROP",
        };
        for (const char* name : names) {
            many.push_back(make_entry(name, name, &dummy));
        }
        // Rows 0, 5 and 10 are the group titles: no game, so the cursor steps
        // over them.
        many[0].game = nullptr;
        many[5].game = nullptr;
        many[10].game = nullptr;

        console::Menu menu(many.data(), static_cast<int>(many.size()));
        menu.reset();
        menu.set_battery(battery(100));
        console::Menu::Input down;
        down.down = true;
        for (int i = 0; i < 9; i++) menu.update(down);
        capture(menu, out + "/menu_4_scrolled.ppm");
    }

    // 5 and 6: a name too wide for its row, at the start of its slide and part
    // way along it. Nothing is renamed to fit any more, so this is the pair of
    // pictures that says what a long name actually looks like: the row under
    // the cursor slides, the rows either side of it sit at their start.
    {
        static const pse::Game dummy{nullptr, nullptr, nullptr};
        static console::Entry rows[] = {
            make_entry("SHORT ONE", "short", &dummy),
            make_entry("PICO SPACE PROGRAM DELUXE", "long", &dummy),
            make_entry("ANOTHER RATHER LONG NAME HERE", "long2", &dummy),
        };

        console::Menu menu(rows, 3);
        menu.reset();
        menu.set_battery(battery(41));
        console::Menu::Input down;
        down.down = true;
        menu.update(down, 0);
        capture(menu, out + "/menu_5_long_name.ppm", 0);
        capture(menu, out + "/menu_6_long_name_sliding.ppm", 2600);
    }

    return 0;
}
