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

void capture(const console::Menu& menu, const std::string& path) {
    static std::vector<uint8_t> buffer(static_cast<size_t>(k_w) * k_h * 3);
    pse::RenderTarget target{buffer.data(), k_w, k_h, k_w * 3,
                             pse::PixelFormat::rgb888};
    menu.draw(target);
    write_ppm(path, buffer.data());
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

    // 1: the console as it opens, on the real library from console.yaml.
    {
        console::Menu menu(console::k_library, console::k_library_count);
        menu.reset();
        capture(menu, out + "/menu_1_open.ppm");
    }

    // 2: opened again after playing Pico Santa. The cursor is where it was
    // left and the dot says so.
    {
        console::Menu menu(console::k_library, console::k_library_count);
        menu.reset("pico-santa");
        capture(menu, out + "/menu_2_resumed.ppm");
    }

    // 3: the cursor stepping over a heading onto the game below it.
    {
        console::Menu menu(console::k_library, console::k_library_count);
        menu.reset("pico-santa");
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
        console::Menu::Input down;
        down.down = true;
        for (int i = 0; i < 9; i++) menu.update(down);
        capture(menu, out + "/menu_4_scrolled.ppm");
    }

    return 0;
}
