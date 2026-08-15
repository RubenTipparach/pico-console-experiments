// The only file in this game that knows the 32blit SDK exists.
//
// Rule 6: game code depends on engine headers, not on the SDK, wherever the
// engine already abstracts it. So this reads the buttons, hands the screen
// over as a pse::RenderTarget, and does nothing else. The rules are in
// sim.cpp and the drawing is in render.cpp, and neither includes 32blit.hpp,
// which is why the host tests and the preview harness can run both.

#include "32blit.hpp"

#include "pse/blit_target.hpp"
#include "pse/game.hpp"

#include "render.hpp"
#include "sim.hpp"

using namespace blit;

namespace {

dl::World g_world;

// One tick is 10 ms, which is what the SDK's update() runs at, and what every
// constant in sim.hpp was converted for. The sim is stepped once per update
// and never per frame: render() can be called at whatever rate the display
// manages without changing how the lander flies.
void game_init() {
    // 240x240. The cart was 128x128 and this is the demake's whole point, so
    // hires rather than the lores every 3D game in this repo uses. 115 KB of
    // the 264 KB goes to the framebuffer; the game's own state is under 2 KB.
    pse::set_screen_mode(pse::ScreenMode::hires);
    dl::world_init(g_world, static_cast<uint32_t>(now()) | 1u);
}

void game_update(uint32_t time) {
    (void)time;
    dl::Input input;
    input.thrust = buttons & Button::A;
    input.left = buttons & Button::DPAD_LEFT;
    input.right = buttons & Button::DPAD_RIGHT;
    // Any button starts a run and any button starts another after a wreck.
    // Rule 9: with nothing on screen naming one, no press can be the wrong
    // guess, so every press has to be accepted.
    input.any_pressed =
        (buttons.pressed & (Button::A | Button::B | Button::X | Button::Y |
                            Button::DPAD_UP | Button::DPAD_DOWN |
                            Button::DPAD_LEFT | Button::DPAD_RIGHT)) != 0;
    dl::world_tick(g_world, input);
}

void game_render(uint32_t time) {
    (void)time;
    dl::render_world(g_world, pse::target_from_screen());
}

}  // namespace

// The one symbol this game exports.
PSE_GAME(dumblander, game_init, game_update, game_render);
