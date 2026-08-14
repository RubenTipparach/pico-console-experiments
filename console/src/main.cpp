// The console: one binary, every game in it, a menu that picks one.
//
// There is no relocation here and no flash writing, because there is nothing
// to relocate: the games are linked in alongside the menu and switching one
// for another is an indirect call through pse::Game. That is the design both
// working references use. crisp-game-lib-portable-32blit keeps a table of
// games and swaps which update() runs; PicoCrystal-GBC builds every ROM into
// its own firmware and boots the one you pick. Neither of them moves native
// code around at runtime, which is the thing that has never worked here.
//
// This file is the only part of the console that knows the SDK exists. The
// menu is plain C++ against a pse::RenderTarget, which is what lets
// console/tests render the real menu on a host and write it out as pictures.

#include <cstring>

#include "32blit.hpp"

#include "battery.hpp"
#include "library.hpp"
#include "menu.hpp"
#include "pse/blit_target.hpp"

using namespace blit;

namespace {

console::Menu g_menu(console::k_library, console::k_library_count);

// -1 is the menu. Anything else indexes k_library.
int g_running = -1;

// Save slot 0 of the console's own save file. Games keep their own saves in
// their own directories (blit::write_save keys off the metadata title), so
// this cannot collide with a game's records.
constexpr uint32_t k_save_magic = 0x31435350;  // "PSC1"

struct ConsoleSave {
    uint32_t magic;
    char slug[24];
};

// Menu key repeat: first repeat after this many frames, then every few.
constexpr uint32_t k_repeat_delay = 22;
constexpr uint32_t k_repeat_every = 6;
uint32_t g_held_ticks = 0;

// Any button starts a game. Rule 9: with nothing on screen naming one, no
// press can be the wrong guess.
constexpr uint32_t k_any_button = Button::A | Button::B | Button::X | Button::Y;

void remember(const char* slug) {
    ConsoleSave save{};
    save.magic = k_save_magic;
    std::strncpy(save.slug, slug, sizeof(save.slug) - 1);
    // Outside render by construction: this runs from update(), never from
    // inside pse::run_split, which is the contract that keeps core 1 alive
    // while XIP is off (STORAGE.md, rule 8).
    write_save(save);
}

const char* last_played() {
    static ConsoleSave save{};
    if (!read_save(save)) return nullptr;
    if (save.magic != k_save_magic) return nullptr;
    save.slug[sizeof(save.slug) - 1] = '\0';
    return save.slug[0] == '\0' ? nullptr : save.slug;
}

void enter(int index) {
    const console::Entry& entry = console::k_library[index];
    if (entry.game == nullptr) return;
    g_running = index;
    g_menu.set_last_played(index);
    remember(entry.slug);
    // init() runs on every entry, not once per boot. That is the whole
    // contract pse::Game asks of a game: be restartable.
    entry.game->init();
}

// Named `held` rather than `pressed`: the SDK has a blit::pressed and
// `using namespace blit` would make the call ambiguous.
//
// Edges come from the SDK's own buttons.pressed rather than a copy of last
// frame's state kept here. Same answer, one fewer thing to keep in step, and
// it is what every game in this repo already reads.
bool held(uint32_t button) { return (buttons.state & button) != 0; }

bool just_pressed(uint32_t button) { return (buttons.pressed & button) != 0; }

}  // namespace

void init() {
    pse::set_screen_mode(pse::ScreenMode::hires);
    g_menu.reset(last_played());
    g_running = -1;
}

void update(uint32_t time) {
    // The console is its own SDK entry point rather than a generated
    // standalone_main, so it puts the board's button mapping in place itself.
    // The menu needs it as much as any game does: on a board with no dpad the
    // list would not scroll either.
    pse::MappedButtons buttons;

    if (g_running < 0) {
        // Read every frame while the menu is up: the ADC is only actually
        // touched once a second, and the charge pins have to be watched at
        // frame rate to catch a charger that pulses them (see battery.hpp).
        g_menu.set_battery(console::battery_sample(time));

        console::Menu::Input input;
        input.select = just_pressed(k_any_button);

        // Up and down move, and repeat when held so a long list does not need
        // forty presses.
        const bool up = held(Button::DPAD_UP);
        const bool down = held(Button::DPAD_DOWN);
        if (up || down) {
            g_held_ticks++;
        } else {
            g_held_ticks = 0;
        }
        const bool repeat = g_held_ticks > k_repeat_delay &&
                            ((g_held_ticks - k_repeat_delay) % k_repeat_every) == 0;
        input.up = just_pressed(Button::DPAD_UP) || (up && repeat);
        input.down = just_pressed(Button::DPAD_DOWN) || (down && repeat);

        const int chosen = g_menu.update(input, time);
        if (chosen >= 0) enter(chosen);
        return;
    }

    // A game runs until the console is restarted. There is no gesture out of
    // one: the power switch is the way back to the menu, it is a control the
    // device already has, and every combination that could have meant "leave"
    // is a combination some game gets to use.
    console::k_library[g_running].game->update(time);
}

void render(uint32_t time) {
    pse::MappedButtons buttons;

    if (g_running < 0) {
        const pse::RenderTarget target = pse::target_from_screen();
        g_menu.draw(target, time);
        return;
    }
    console::k_library[g_running].game->render(time);
}
