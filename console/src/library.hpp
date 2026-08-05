#pragma once

#include <cstdint>

#include "pse/game.hpp"

namespace console {

// Menu icons. 24x24 is what fits a row that also has a readable name in it.
// The source picture is the game's committed 120x120 thumbnail, box averaged
// down by tools/gen_library.py at build time.
constexpr int k_icon_w = 24;
constexpr int k_icon_h = 24;

// Icon pixels are CONVENTIONAL RGB565: red in bits 11..15, green 5..10, blue
// 0..4. Deliberately not the SDK's layout, which puts red in the low bits to
// cancel out the PicoSystem panel being wired BGR (see pse/pixel.hpp). The
// menu decodes to eight bit channels and hands those to pse::plot_pixel,
// which knows what the target it was given wants. That is what lets the same
// icon draw correctly on the device and in a host screenshot.
constexpr uint16_t icon_red(uint16_t px) {
    return static_cast<uint16_t>((px >> 11) & 0x1F);
}
constexpr uint16_t icon_green(uint16_t px) {
    return static_cast<uint16_t>((px >> 5) & 0x3F);
}
constexpr uint16_t icon_blue(uint16_t px) { return static_cast<uint16_t>(px & 0x1F); }

// One row of the menu.
//
// A heading is an entry with no game: it is drawn, it cannot be selected, and
// the cursor steps over it. crisp-game-lib does the same thing with a null
// update pointer, which is what lets one flat table carry both.
struct Entry {
    const char* name;      // what the menu draws. Font charset only.
    const char* slug;      // stable id, used as the last played key
    const pse::Game* game; // nullptr for a heading
    const uint16_t* icon;  // k_icon_w * k_icon_h pixels, or nullptr
};

// Defined by the generated console_library.cpp. Declared here so the menu,
// the tests, and the preview harness all agree on the shape without any of
// them needing the generator to have run.
extern const Entry k_library[];
extern const int k_library_count;

// The name across the top of the menu, from console.yaml.
extern const char* const k_console_title;

}  // namespace console
