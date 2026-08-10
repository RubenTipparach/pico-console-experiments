// The only file in this game that knows the 32blit SDK exists.
//
// Rule 6: it reads the buttons, hands the screen over as a pse::RenderTarget,
// and does nothing else. The rules are in sim.cpp and the drawing is in
// render.cpp, and neither includes 32blit.hpp, which is why the host tests and
// the preview harness can run both.

#include "32blit.hpp"

#include "pse/blit_target.hpp"
#include "pse/game.hpp"

#include "render.hpp"
#include "sim.hpp"

using namespace blit;

namespace {

cc::World g_world;

void game_init() {
    // 240x240. The cart was 128x128 and this is the demake's whole point.
    // 115 KB of the 264 KB goes to the framebuffer, and the world is 9 KB.
    set_screen_mode(ScreenMode::hires);
    cc::world_init(g_world, static_cast<uint32_t>(now()) | 1u);
}

void game_update(uint32_t time) {
    (void)time;
    cc::Input in;
    // A drops a coin, which is the verb of the game. B uses whatever the row
    // has selected. Nothing on screen names either of them: rule 9.
    in.drop_pressed = (buttons.pressed & Button::A) != 0;
    in.use_pressed = (buttons.pressed & Button::B) != 0;
    in.left_pressed = (buttons.pressed & Button::DPAD_LEFT) != 0;
    in.right_pressed = (buttons.pressed & Button::DPAD_RIGHT) != 0;
    in.up_pressed = (buttons.pressed & Button::DPAD_UP) != 0;
    in.down_pressed = (buttons.pressed & Button::DPAD_DOWN) != 0;
    in.any_pressed =
        (buttons.pressed & (Button::A | Button::B | Button::X | Button::Y |
                            Button::DPAD_UP | Button::DPAD_DOWN |
                            Button::DPAD_LEFT | Button::DPAD_RIGHT)) != 0;
    cc::world_tick(g_world, in);
}

void game_render(uint32_t time) {
    (void)time;
    cc::render_world(g_world, pse::target_from_screen());
}

}  // namespace

// The one symbol this game exports.
PSE_GAME(catcoin, game_init, game_update, game_render);
