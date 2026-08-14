#include "32blit.hpp"

#include <cmath>

#include "pse/blit_target.hpp"
#include "pse/game.hpp"
#include "pse/raster.hpp"
#include "pse/renderer3d.hpp"
#include "pse/shared_render.hpp"

#include "city.hpp"
#include "pico_santa/gem.hpp"
#include "pico_santa/sleigh.hpp"

using namespace blit;

namespace {

// lores on PicoSystem is 120x120, which is what the engine is sized for.
constexpr float k_move_speed = 0.010f;
constexpr float k_reverse_speed = 0.005f;
constexpr float k_turn_speed = 0.030f;
constexpr float k_friction = 0.95f;
constexpr float k_player_radius = 0.5f;
constexpr float k_street_limit = 2.5f;

struct Player {
    float x, y, z;
    float velocity_x, velocity_z;
    float yaw;
};

// The engine's, not ours: on the console every game is in one binary, and a
// 14 KB depth buffer per game is RAM spent on scenes nothing is rendering.
pse::Rasterizer& g_rasterizer = pse::shared_rasterizer();
pse::Renderer3D g_renderer(g_rasterizer);
santa::City g_city;
Player g_player;
int g_score;

// Off by default. A published build should not spend a quarter of a 120x120
// screen telling the player about frame timings.
#ifdef SANTA_SHOW_STATS
constexpr bool k_show_stats = true;
#else
constexpr bool k_show_stats = false;
#endif

uint32_t g_frame_us;

void reset_player() {
    g_player.x = 5.0f;
    g_player.y = 0.0f;
    g_player.z = 0.0f;
    g_player.velocity_x = 0.0f;
    g_player.velocity_z = 0.0f;
    g_player.yaw = 0.0f;
    g_score = 0;
}

void game_init() {
    pse::set_screen_mode(pse::ScreenMode::lores);
    g_city.reset(12345);
    reset_player();
}

void game_update(uint32_t time) {
    const float previous_x = g_player.x;
    const float previous_z = g_player.z;

    if (buttons & Button::DPAD_LEFT) g_player.yaw += k_turn_speed;
    if (buttons & Button::DPAD_RIGHT) g_player.yaw -= k_turn_speed;

    const float forward_x = sinf(g_player.yaw);
    const float forward_z = cosf(g_player.yaw);

    if (buttons & Button::DPAD_UP) {
        g_player.velocity_x += forward_x * k_move_speed;
        g_player.velocity_z += forward_z * k_move_speed;
    }
    if (buttons & Button::DPAD_DOWN) {
        g_player.velocity_x -= forward_x * k_reverse_speed;
        g_player.velocity_z -= forward_z * k_reverse_speed;
    }

    g_player.velocity_x *= k_friction;
    g_player.velocity_z *= k_friction;
    g_player.x += g_player.velocity_x;
    g_player.z += g_player.velocity_z;

    if (g_player.z < -k_street_limit) g_player.z = -k_street_limit;
    if (g_player.z > k_street_limit) g_player.z = k_street_limit;

    if (g_city.collides(g_player.x, g_player.z, k_player_radius)) {
        g_player.x = previous_x;
        g_player.z = previous_z;
        g_player.velocity_x = 0.0f;
        g_player.velocity_z = 0.0f;
    }

    if (g_player.x < 1.0f) {
        g_player.x = 1.0f;
        g_player.velocity_x = 0.0f;
    }

    g_city.update(g_player.x);
    g_score += g_city.collect_gems(g_player.x, g_player.z, 1.5f);
}

void game_render(uint32_t time) {
    const uint32_t frame_start = now_us();

    g_rasterizer.begin_frame(pse::target_from_screen());
    g_rasterizer.clear_gradient(40, 60, 120, 60, 90, 160);

    g_renderer.set_orbit_camera(g_player.x, g_player.y, g_player.z,
                                g_player.yaw, 8.0f, 4.0f);

    // Checkerboard road. Drawn as flat boxes so it shares the depth buffer with
    // everything else rather than needing a separate ground pass.
    const int player_tile_x = static_cast<int>(floorf(g_player.x / 4.0f));
    const int player_tile_z = static_cast<int>(floorf(g_player.z / 4.0f));
    for (int dx = -5; dx <= 5; dx++) {
        for (int dz = -5; dz <= 5; dz++) {
            const int tile_x = player_tile_x + dx;
            const int tile_z = player_tile_z + dz;
            const bool dark = ((tile_x + tile_z) & 1) == 0;
            const uint8_t shade = dark ? 60 : 80;
            g_renderer.draw_box(tile_x * 4.0f + 2.0f, -0.5f, tile_z * 4.0f + 2.0f,
                                4.0f, 0.5f, 4.0f,
                                shade, shade, static_cast<uint8_t>(shade + 10),
                                shade, shade, static_cast<uint8_t>(shade + 10));
        }
    }

    g_city.render(g_renderer);
    g_city.render_gems(g_renderer, models::pico_santa::gem, time);

    g_renderer.draw_mesh(models::pico_santa::sleigh, g_player.x, g_player.y, g_player.z,
                         g_player.yaw, 1.0f);

    g_frame_us = now_us() - frame_start;

    // Score only. The screen is 120 pixels wide and the player needs one number.
    screen.pen = Pen(0, 0, 0, 160);
    screen.rectangle(Rect(0, 0, screen.bounds.w, 11));
    screen.pen = Pen(255, 255, 255);
    screen.text(std::to_string(g_score), minimal_font, Point(3, 2));

    if (k_show_stats) {
        screen.pen = Pen(0, 0, 0, 160);
        screen.rectangle(Rect(0, screen.bounds.h - 10, screen.bounds.w, 10));
        screen.pen = Pen(120, 240, 120);
        screen.text(std::to_string(g_frame_us / 1000) + "ms " +
                        std::to_string(g_rasterizer.triangles_drawn()) + "t",
                    minimal_font, Point(3, screen.bounds.h - 9));
    }
}

}  // namespace

// The one symbol this game exports. Everything above is internal linkage, so
// the console can link it beside three other games that also have a g_player.
PSE_GAME(pico_santa, game_init, game_update, game_render);
