// What a frame actually contains, asserted by rendering one.
//
// Three bugs on this branch were invisible to every other kind of test and
// obvious the moment somebody looked at a picture: the overworld ground came
// out one flat colour, the battle arena had sky where its floor should be, and
// the title's creature was swallowed by the ground it stood on. All three were
// the depth buffer, and all three compiled, ran, and passed the sim tests.
//
// So these render real frames through the real engine and assert on the pixels.
// No SDK, same as the preview harness; unlike the preview, this one fails.

#include <cstdio>
#include <vector>

#include "pse/pixel.hpp"

#include "render.hpp"
#include "sim.hpp"

namespace {

constexpr int k_w = 120;
constexpr int k_h = 120;

int g_failures = 0;

void check(bool ok, const char* what) {
    if (ok) return;
    std::printf("FAIL: %s\n", what);
    g_failures++;
}

struct Frame {
    std::vector<uint8_t> rgb = std::vector<uint8_t>(size_t(k_w) * k_h * 3);
    const uint8_t* at(int x, int y) const { return &rgb[(size_t(y) * k_w + x) * 3]; }
};

void render(const pm::World& w, Frame& f, uint32_t t) {
    pse::RenderTarget target{f.rgb.data(), k_w, k_h, k_w * 3,
                             pse::PixelFormat::rgb888};
    pmr::render_scene(w, target, t);
}

// Sky is blue AND pale: the gradient runs from 66AAEE to AADDEE. Water is blue
// and dark (3366BB), and counting it as sky is how the first version of this
// test failed on a pond.
bool is_sky(const uint8_t* p) {
    return p[2] > p[1] + 20 && p[2] > 0xD0 && p[1] > 0x90;
}

int count_sky(const Frame& f, int y0, int y1) {
    int n = 0;
    for (int y = y0; y < y1; y++)
        for (int x = 0; x < k_w; x++)
            if (is_sky(f.at(x, y))) n++;
    return n;
}

// The creatures are warm and the world is green, so red leading green picks
// them out of it without needing to know their exact shade.
int count_warm(const Frame& f, int x0, int y0, int x1, int y1) {
    int n = 0;
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++) {
            const uint8_t* p = f.at(x, y);
            if (p[0] > p[1] + 20 && p[0] > 100) n++;
        }
    return n;
}

int count_distinct_colours(const Frame& f, int y0, int y1) {
    // Cheap stand in for "the map has more than one material in it": the flat
    // ground bug produced exactly one colour across the whole play area.
    int seen[64] = {0};
    int n = 0;
    for (int y = y0; y < y1; y++) {
        for (int x = 0; x < k_w; x++) {
            const uint8_t* p = f.at(x, y);
            const int key = ((p[0] >> 6) << 4) | ((p[1] >> 6) << 2) | (p[2] >> 6);
            if (!seen[key]) { seen[key] = 1; n++; }
        }
    }
    return n;
}

pm::World fresh() {
    pm::World w;
    pm::world_init(w, 4242);
    pm::Input go{};
    go.a_pressed = true;
    pm::world_tick(w, go);          // leave the title
    return w;
}

void test_overworld_has_a_map_in_it() {
    pm::World w = fresh();
    w.zone = pm::zone_route1;
    w.tx = 11;
    w.ty = 17;
    w.mode = pm::Mode::Overworld;
    Frame f;
    render(w, f, 1000);
    // The flat ground bug: every tile tied with a quad underneath and lost, so
    // the whole play area came out one colour.
    const int colours = count_distinct_colours(f, 20, 110);
    std::printf("  route: %d distinct ground colours\n", colours);
    check(colours >= 4, "the overworld shows more than one material");
    // The near half of the frame is ground at any camera position in this
    // game, so sky there means a band was dropped.
    check(count_sky(f, 60, 110) == 0, "no sky in the near half of the overworld");
}

void test_battle_floor_is_solid() {
    pm::World w = fresh();
    w.mode = pm::Mode::Battle;
    w.battle.foe = pm::make_mon(2, 5);
    w.battle.active = 0;
    w.battle.state = pm::BattleState::Menu;
    Frame f;
    render(w, f, 1000);
    // The arena floor runs from under the treeline to the message panel. Sky
    // in there means a ground band was dropped whole, which is what happens
    // when a corner of it falls outside the depth range.
    const int sky = count_sky(f, 34, 84);
    std::printf("  battle: %d sky pixels in the arena\n", sky);
    check(sky == 0, "the battle arena has a floor");
    // And the creature standing on it has to survive the depth test against it.
    const int warm = count_warm(f, 0, 40, 60, 84);
    std::printf("  battle: %d creature pixels\n", warm);
    check(warm > 40, "the player's creature is not swallowed by the ground");
}

void test_title_shows_its_creature() {
    pm::World w;
    pm::world_init(w, 4242);
    Frame f;
    render(w, f, 1000);
    check(count_sky(f, 70, 120) == 0, "the title has ground under it");
    const int warm = count_warm(f, 10, 40, 80, 90);
    std::printf("  title: %d creature pixels\n", warm);
    check(warm > 40, "the title's creature is drawn");
}

void test_menus_draw_something() {
    pm::World w = fresh();
    w.mode = pm::Mode::Bag;
    Frame f;
    render(w, f, 1000);
    check(count_distinct_colours(f, 0, 120) >= 3, "the bag draws its panels");
    w.mode = pm::Mode::Party;
    render(w, f, 1000);
    check(count_distinct_colours(f, 0, 120) >= 3, "the party draws its rows");
}

}  // namespace

int main() {
    test_overworld_has_a_map_in_it();
    test_battle_floor_is_solid();
    test_title_shows_its_creature();
    test_menus_draw_something();

    if (g_failures) {
        std::printf("%d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("picomon render tests passed\n");
    return 0;
}
