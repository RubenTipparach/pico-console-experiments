#include "32blit.hpp"

#include "pse/blit_target.hpp"

#include "render.hpp"
#include "sim.hpp"

using namespace blit;

// The SDK facing shell, and the only file in this game that includes 32blit.
// Everything else is plain C++ against the sim and a pse::RenderTarget, which
// is why the same code compiles for the device, for desktop, for the browser,
// and for the host preview harness.

namespace {

pm::World g_world;

// The RAM footprint is a hard promise, checked at compile time. If either of
// these fails, something grew without its cost being paid attention to.
static_assert(sizeof(pm::World) <= 1024, "sim state grew past its RAM budget");
static_assert(sizeof(pm::SaveData) <= 256, "the save block grew past a flash block");

void save_if_safe() {
    // write_save disables XIP while it programs flash, and core 1 survives
    // that only while parked in its RAM resident idle loop. update() is
    // outside any render call by construction, so this is the safe moment;
    // doing it from render() would be the unsafe one.
    if (!g_world.save_pending) return;
    pm::SaveData data;
    pm::world_make_save(g_world, data);
    blit::write_save(data);
    g_world.save_pending = false;
}

}  // namespace

void init() {
    set_screen_mode(ScreenMode::lores);

    pm::SaveData data;
    const bool have_save = blit::read_save(data);
    // now() is near constant at boot, so fold the save in for seed variety
    // between sessions. Determinism within a run is what matters, not the seed.
    uint32_t seed = blit::now() ^ 0x9E3779B9u;
    if (have_save) seed ^= uint32_t(data.money) * 2654435761u;
    pm::world_init(g_world, seed);
    if (have_save) pm::world_load(g_world, data);
}

void update(uint32_t time) {
    (void)time;
    pm::Input in;
    in.up = (buttons & Button::DPAD_UP) != 0;
    in.down = (buttons & Button::DPAD_DOWN) != 0;
    in.left = (buttons & Button::DPAD_LEFT) != 0;
    in.right = (buttons & Button::DPAD_RIGHT) != 0;
    in.up_pressed = (buttons.pressed & Button::DPAD_UP) != 0;
    in.down_pressed = (buttons.pressed & Button::DPAD_DOWN) != 0;
    in.left_pressed = (buttons.pressed & Button::DPAD_LEFT) != 0;
    in.right_pressed = (buttons.pressed & Button::DPAD_RIGHT) != 0;
    in.a_pressed = (buttons.pressed & Button::A) != 0;
    in.b_pressed = (buttons.pressed & Button::B) != 0;
    in.x_pressed = (buttons.pressed & Button::X) != 0;
    in.y_pressed = (buttons.pressed & Button::Y) != 0;

    pm::world_tick(g_world, in);
    save_if_safe();
}

void render(uint32_t time) {
    pmr::render_scene(g_world, pse::target_from_screen(), time);
}
