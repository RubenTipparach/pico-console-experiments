#pragma once

#include <cstdint>

namespace pse {

// What a game is, to anything that runs one.
//
// Three function pointers, which is the whole interface. The console's menu
// holds a table of these and calls one of them per frame; a standalone build
// holds exactly one and forwards the SDK's entry points to it. Neither knows
// anything else about the game.
//
// This is the seam that makes a multi game console possible at all. The
// RP2040 executes in place from flash at a fixed address, so two native
// images cannot both be at the address they were linked for. Rather than
// relocate code, every game is linked into one binary and the menu picks
// which update runs, which is what crisp-game-lib-portable-32blit does with
// its `Game` table and what PicoCrystal does with its ROM catalog. Nothing is
// copied, nothing is relocated, and switching games costs one indirect call.
struct Game {
    // Put the game back to its opening state. Called every time the game is
    // entered from the menu, not once per boot, so it must be safe to call
    // again: seed the world here, not in a file scope initialiser.
    void (*init)();

    // One simulation step. `time` is milliseconds since boot, as the SDK
    // hands it over.
    void (*update)(uint32_t time);

    // Draw one frame.
    void (*render)(uint32_t time);
};

}  // namespace pse

// The one symbol a game exports.
//
// Everything else in a game's translation units should be in an anonymous
// namespace or a namespace of the game's own: several games are linked into
// the console together, and two file scope `g_world`s are a duplicate symbol,
// not a warning. `extern "C"` keeps the name unmangled and predictable so the
// generated library table can declare it without knowing the game's C++
// namespaces.
#define PSE_GAME(slug, init_fn, update_fn, render_fn) \
    extern "C" const ::pse::Game pse_game_##slug = {init_fn, update_fn, render_fn}

// The matching declaration, for the console's generated table and for the
// standalone shim.
#define PSE_GAME_DECL(slug) extern "C" const ::pse::Game pse_game_##slug
