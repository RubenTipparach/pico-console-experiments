// The chase camera, which is the one part of the renderer that carries state
// between frames and so is the one part that can be wrong in a way a single
// screenshot cannot show.
//
// It is eased toward the ship rather than bolted to it, and the three axes are
// eased independently. Independent easing does not preserve a rotation: lerp
// halfway between two unit vectors and the result is shorter than either, and
// a basis that is no longer orthonormal goes straight into a view matrix and
// skews and scales everything drawn through it. On a 120 pixel screen that
// looks like the scene breathing slightly during a turn, which is exactly the
// kind of thing nobody files a bug about.
//
// The pico-8 game this camera is modelled on lerps its axes and never
// re-orthonormalises, and gets away with it because its projection is a hand
// written dot product per vertex. This one cannot, so the property is
// measured here.

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "pse/pixel.hpp"

#include "render.hpp"
#include "sim.hpp"

namespace {

int g_checks = 0;
int g_failures = 0;

void check(bool ok, const char* expr, int line) {
    g_checks++;
    if (ok) return;
    g_failures++;
    std::printf("FAIL line %d: %s\n", line, expr);
}

#define CHECK(expr) check((expr), #expr, __LINE__)

constexpr int k_w = 120;
constexpr int k_h = 120;

std::vector<uint8_t> g_buffer(static_cast<size_t>(k_w) * k_h * 3);

pse::RenderTarget target() {
    return {g_buffer.data(), k_w, k_h, k_w * 3, pse::PixelFormat::rgb888};
}

sdr::Chrome playing() {
    sdr::Chrome chrome{};
    chrome.screen = sdr::Screen::Play;
    return chrome;
}

// One frame at a plausible frame time.
sdr::CameraState frame(const sd::World& world, const sdr::Chrome& chrome,
                       uint32_t& clock, uint32_t dt_ms = 16) {
    clock += dt_ms;
    sdr::render_scene(world, chrome, target(), clock);
    return sdr::last_camera();
}

float dot3(const float a[3], const float b[3]) {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

// The property the whole file exists for: still a rotation.
void expect_orthonormal(const sdr::CameraState& cam, int line) {
    const float lr = dot3(cam.right, cam.right);
    const float lu = dot3(cam.up, cam.up);
    const float lf = dot3(cam.forward, cam.forward);
    check(std::fabs(lr - 1.0f) < 0.002f, "right is unit length", line);
    check(std::fabs(lu - 1.0f) < 0.002f, "up is unit length", line);
    check(std::fabs(lf - 1.0f) < 0.002f, "forward is unit length", line);
    check(std::fabs(dot3(cam.right, cam.up)) < 0.002f, "right is square to up",
          line);
    check(std::fabs(dot3(cam.right, cam.forward)) < 0.002f,
          "right is square to forward", line);
    check(std::fabs(dot3(cam.up, cam.forward)) < 0.002f,
          "up is square to forward", line);
}

void ship_forward(const sd::World& world, float out[3]) {
    pse::Basis basis;
    sd::player_basis(world, basis);
    out[0] = basis.m[2];
    out[1] = basis.m[5];
    out[2] = basis.m[8];
}

// ---- the tests ----

// The first frame has nothing to ease from, so it must land on the ship rather
// than fly in from wherever the camera was left by the last sortie.
void test_the_first_frame_snaps_rather_than_swooping() {
    sd::World world;
    sd::world_init(world);
    uint32_t clock = 1000;

    const sdr::CameraState cam = frame(world, playing(), clock, 5000);
    expect_orthonormal(cam, __LINE__);

    float nose[3];
    ship_forward(world, nose);
    // Pointing exactly where the ship points on the very first frame.
    CHECK(dot3(cam.forward, nose) > 0.999f);

    // And sitting behind it, not on it.
    const float dx = cam.x - static_cast<float>(world.x) / sd::k_one;
    const float dy = cam.y - static_cast<float>(world.y) / sd::k_one;
    const float dz = cam.z - static_cast<float>(world.z) / sd::k_one;
    const float range = std::sqrt(dx * dx + dy * dy + dz * dz);
    CHECK(range > 3.0f && range < 12.0f);
}

// The point of the whole thing: during a turn the camera is BEHIND the ship's
// heading, and it catches up once the turn stops.
void test_the_camera_trails_a_turn_and_then_catches_up() {
    sd::World world;
    sd::world_init(world);
    world.wave_timer = 60000;             // an empty sky, nothing to distract
    uint32_t clock = 1000;

    frame(world, playing(), clock, 5000);   // seed

    sd::Input yaw{};
    yaw.yaw = 1;

    // Turn hard for half a second, a frame of sim per frame of render.
    float worst = 1.0f;
    for (int f = 0; f < 30; f++) {
        for (int t = 0; t < 2; t++) sd::world_tick(world, yaw);
        const sdr::CameraState cam = frame(world, playing(), clock);
        expect_orthonormal(cam, __LINE__);
        float nose[3];
        ship_forward(world, nose);
        const float agree = dot3(cam.forward, nose);
        if (agree < worst) worst = agree;
    }
    // It really is lagging: a rigid camera would hold this at 1.0 exactly.
    std::printf("camera lag during turn: cos %.4f\n",
                static_cast<double>(worst));
    CHECK(worst < 0.9995f);

    // Stop turning and it settles back onto the nose.
    for (int f = 0; f < 60; f++) {
        for (int t = 0; t < 2; t++) sd::world_tick(world, sd::Input{});
        frame(world, playing(), clock);
    }
    const sdr::CameraState settled = sdr::last_camera();
    expect_orthonormal(settled, __LINE__);
    float nose[3];
    ship_forward(world, nose);
    CHECK(dot3(settled.forward, nose) > 0.999f);
}

// A roll is the case that breaks a naive lerp hardest, because right and up
// sweep through a whole circle while forward barely moves. If the basis is
// going to stop being a basis, it stops here.
void test_a_hard_roll_keeps_the_basis_a_basis() {
    sd::World world;
    sd::world_init(world);
    world.wave_timer = 60000;
    uint32_t clock = 1000;
    frame(world, playing(), clock, 5000);

    sd::Input roll{};
    roll.roll = 1;
    for (int f = 0; f < 200; f++) {
        for (int t = 0; t < 2; t++) sd::world_tick(world, roll);
        const sdr::CameraState cam = frame(world, playing(), clock);
        expect_orthonormal(cam, __LINE__);
    }
}

// Holding the target button turns the camera off the nose and onto the
// contact, and letting go brings it back. This is the padlock view, and the
// measurement is whether the target is nearer the middle of the lens than the
// ship's own heading is.
void test_padlock_puts_the_target_in_front_of_the_lens() {
    sd::World world;
    sd::world_init(world);
    uint32_t clock = 1000;

    // A wave on the field, and something selected.
    world.wave = 1;
    world.phase = sd::Phase::Briefing;
    world.wave_timer = 0;
    sd::world_tick(world, sd::Input{});
    sd::Input pick{};
    pick.cycle_target = true;
    sd::world_tick(world, pick);

    const sd::Ship* target = sd::target_ship(world);
    CHECK(target != nullptr);
    if (target == nullptr) return;

    // Turn away from it so the nose and the contact genuinely disagree.
    sd::Input yaw{};
    yaw.yaw = 1;
    for (int t = 0; t < 90; t++) sd::world_tick(world, yaw);

    sdr::Chrome chrome = playing();
    frame(world, chrome, clock, 5000);      // seed, not looking

    auto aim_at_target = [&](const sdr::CameraState& cam) {
        float to[3] = {static_cast<float>(target->x) / sd::k_one - cam.x,
                       static_cast<float>(target->y) / sd::k_one - cam.y,
                       static_cast<float>(target->z) / sd::k_one - cam.z};
        const float len = std::sqrt(dot3(to, to));
        if (len < 1e-6f) return 1.0f;
        to[0] /= len; to[1] /= len; to[2] /= len;
        return dot3(cam.forward, to);
    };

    const float looking_away = aim_at_target(sdr::last_camera());

    // Hold the button and let the camera swing.
    chrome.look_at_target = true;
    for (int f = 0; f < 60; f++) frame(world, chrome, clock);
    const sdr::CameraState locked = sdr::last_camera();
    expect_orthonormal(locked, __LINE__);
    const float looking_at = aim_at_target(locked);

    std::printf("padlock: off target cos %.4f, on target cos %.4f\n",
                static_cast<double>(looking_away),
                static_cast<double>(looking_at));
    CHECK(looking_at > looking_away);
    CHECK(looking_at > 0.99f);

    // Let go and it comes back to the nose.
    chrome.look_at_target = false;
    for (int f = 0; f < 90; f++) frame(world, chrome, clock);
    const sdr::CameraState released = sdr::last_camera();
    expect_orthonormal(released, __LINE__);
    float nose[3];
    ship_forward(world, nose);
    CHECK(dot3(released.forward, nose) > 0.999f);
}

// Padlock onto a contact sitting exactly off the wingtip.
//
// This is the case the first version of the padlock basis got wrong. It took
// the ship's RIGHT as the roll reference and squared it against the look
// direction, and those two are parallel when the target is abeam, so the
// reference collapsed and the roll snapped to a world axis. Abeam is not an
// edge case for this feature, it is the main one: a contact off the wingtip is
// the whole reason to look away from the nose.
void test_padlock_holds_its_roll_with_the_target_abeam() {
    sd::World world;
    sd::world_init(world);
    world.wave_timer = 60000;
    uint32_t clock = 1000;

    // One contact, parked exactly off the right wingtip. The ship starts at
    // the origin looking down +z, so +x is abeam.
    world.ships[0] = sd::Ship{};
    world.ships[0].active = true;
    world.ships[0].cls = sd::Hull::Fighter;
    world.ships[0].q = pse::quat_identity();
    world.ships[0].hull = world.ships[0].hull_max = 26;
    world.ships[0].x = sd::units(40);
    world.target = 0;
    world.target_sub = -1;

    sdr::Chrome chrome = playing();
    chrome.look_at_target = true;

    // Settle, then check the camera is both looking at it and still upright
    // relative to the ship rather than rolled onto some world axis.
    frame(world, chrome, clock, 5000);
    for (int f = 0; f < 40; f++) frame(world, chrome, clock);

    const sdr::CameraState cam = sdr::last_camera();
    expect_orthonormal(cam, __LINE__);

    // Looking along +x, at the contact.
    CHECK(cam.forward[0] > 0.9f);

    // And the camera's up still agrees with the ship's up, which is world +y.
    // A collapsed reference would have put it anywhere.
    pse::Basis basis;
    sd::player_basis(world, basis);
    const float ship_up[3] = {basis.m[1], basis.m[4], basis.m[7]};
    std::printf("abeam padlock: up . ship up = %.4f\n",
                static_cast<double>(dot3(cam.up, ship_up)));
    CHECK(dot3(cam.up, ship_up) > 0.9f);
}

// A ship that moves a long way between frames is a restart, not a manoeuvre.
// Easing across that would be a long swoop through the arena.
void test_a_teleport_snaps_instead_of_flying_across_the_arena() {
    sd::World world;
    sd::world_init(world);
    world.wave_timer = 60000;
    uint32_t clock = 1000;
    frame(world, playing(), clock, 5000);

    world.x += sd::units(120);
    world.z -= sd::units(90);
    const sdr::CameraState cam = frame(world, playing(), clock);
    expect_orthonormal(cam, __LINE__);

    const float dx = cam.x - static_cast<float>(world.x) / sd::k_one;
    const float dy = cam.y - static_cast<float>(world.y) / sd::k_one;
    const float dz = cam.z - static_cast<float>(world.z) / sd::k_one;
    // Right behind the ship again on the very next frame, not somewhere on the
    // way there.
    CHECK(std::sqrt(dx * dx + dy * dy + dz * dz) < 12.0f);
}

// The frame time matters: the easing is per second, not per frame, so a slow
// frame has to move the camera further than a fast one. Otherwise the camera
// lags differently on the device than it does on a laptop.
void test_the_ease_follows_the_clock_and_not_the_frame_count() {
    sd::Input yaw{};
    yaw.yaw = 1;

    auto lag_after = [&](uint32_t dt_ms, int frames, int ticks_per_frame) {
        sd::World world;
        sd::world_init(world);
        world.wave_timer = 60000;
        uint32_t clock = 1000;
        frame(world, playing(), clock, 5000);
        for (int f = 0; f < frames; f++) {
            for (int t = 0; t < ticks_per_frame; t++) sd::world_tick(world, yaw);
            frame(world, playing(), clock, dt_ms);
        }
        float nose[3];
        ship_forward(world, nose);
        return dot3(sdr::last_camera().forward, nose);
    };

    // The same half second of turning, once at 16 ms a frame and once at 32.
    // Same elapsed time and the same amount of turn, so the camera should end
    // up about as far behind either way.
    const float fine = lag_after(16, 30, 2);
    const float coarse = lag_after(32, 15, 4);
    std::printf("lag at 16ms %.4f, at 32ms %.4f\n",
                static_cast<double>(fine), static_cast<double>(coarse));
    CHECK(std::fabs(fine - coarse) < 0.01f);
}

// The settings menus are real, and they show the setting.
//
// Toggling lives in game.cpp, which is SDK code no host test can compile, so
// what can be checked here is the half that decides what a player SEES: that
// the pause and title menus redraw when a setting changes rather than printing
// a fixed label that happens to read "SOUND ON".
void test_the_menus_show_the_settings_they_toggle() {
    sd::World world;
    sd::world_init(world);
    uint32_t clock = 1000;

    auto snapshot = [&](sdr::Screen screen, bool sound, bool invert) {
        sdr::Chrome chrome{};
        chrome.screen = screen;
        chrome.sound_on = sound;
        chrome.invert_pitch = invert;
        clock += 16;
        sdr::render_scene(world, chrome, target(), clock);
        return g_buffer;   // a copy
    };

    for (sdr::Screen screen : {sdr::Screen::Paused, sdr::Screen::Title}) {
        const std::vector<uint8_t> sound_on = snapshot(screen, true, false);
        const std::vector<uint8_t> sound_off = snapshot(screen, false, false);
        const std::vector<uint8_t> inverted = snapshot(screen, true, true);

        // Flipping either setting has to change the picture. If it does not,
        // the row is a label rather than a readout.
        CHECK(sound_on != sound_off);
        CHECK(sound_on != inverted);
        CHECK(sound_off != inverted);
    }
}

}  // namespace

int main() {
    test_the_menus_show_the_settings_they_toggle();
    test_the_first_frame_snaps_rather_than_swooping();
    test_the_camera_trails_a_turn_and_then_catches_up();
    test_a_hard_roll_keeps_the_basis_a_basis();
    test_padlock_puts_the_target_in_front_of_the_lens();
    test_padlock_holds_its_roll_with_the_target_abeam();
    test_a_teleport_snaps_instead_of_flying_across_the_arena();
    test_the_ease_follows_the_clock_and_not_the_frame_count();

    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
