// Joker Reels: the SDK facing half, which is as small as it can be.
//
// Input, the screen mode, and one call. Every pixel including every character
// of text is drawn in render.cpp against a pse::RenderTarget, so the host
// preview harness renders the complete game and nothing in it is unverified
// until somebody runs it on hardware.
//
// That is deliberate and it is not what this game did first. The HUD started
// out here with screen.text, the way the other games draw theirs, and the
// preview harness could not see any of it: the screenshots were a slot machine
// with no score, no ante, and no labels on the speed dial. pse::draw_text is
// the engine's own 5x7 font drawn straight into a target, and its header
// already makes this argument for the console's menu. A HUD has the same
// problem and gets the same answer.

#include "32blit.hpp"

#include "pse/blit_target.hpp"
#include "pse/game.hpp"

#include "render.hpp"
#include "sim.hpp"

using namespace blit;

namespace {

jr::World g_world;

// The run's RAM footprint is a promise, checked by the compiler.
static_assert(sizeof(jr::World) <= 768, "the run state grew past its budget");

// Any button acts. With nothing on screen naming one, no press can be the
// wrong guess, which is rule 9's reason for having no button prompts at all.
constexpr uint32_t k_any_button =
    Button::A | Button::B | Button::X | Button::Y |
    Button::DPAD_UP | Button::DPAD_DOWN | Button::DPAD_LEFT |
    Button::DPAD_RIGHT;

void game_init() {
    // Hires. This is the one 3D game here that is not lores, and it can be
    // because the 3D covers the top 112 rows rather than the whole screen.
    // render.hpp carries the arithmetic.
    set_screen_mode(ScreenMode::hires);
    jr::world_init(g_world, 0x5EED5EEDu);
}

void game_update(uint32_t time) {
    (void)time;
    jr::Buttons btn{};
    btn.a = buttons.pressed & Button::A;
    btn.b = buttons.pressed & Button::B;
    btn.up = buttons.pressed & Button::DPAD_UP;
    btn.down = buttons.pressed & Button::DPAD_DOWN;
    btn.left = buttons.pressed & Button::DPAD_LEFT;
    btn.right = buttons.pressed & Button::DPAD_RIGHT;
    btn.any = (buttons.pressed & k_any_button) != 0;
    jr::world_tick(g_world, btn);
}

void game_render(uint32_t time) {
    (void)time;
    jrr::render_frame(g_world, pse::target_from_screen());
}

}  // namespace

PSE_GAME(jokerreels, game_init, game_update, game_render);
