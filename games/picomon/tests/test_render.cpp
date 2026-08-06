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

// North is up. This is not a matter of taste: the sim walks the player to a
// smaller map y when they press up, and for a whole feature the renderer laid
// map rows straight onto world z, which put larger y (south) at the top. The
// map came out back to front, pressing up walked toward the bottom of the
// frame, and every screenshot looked plausible because the maps that existed
// then were nearly symmetric. It took a gym with a door at one end and a
// leader at the other to see it.
//
// So: stand in the gym one tile north of its door and prove the door is
// below. The door is the only tile of its colour in the room.
void test_north_is_up() {
    pm::World w = fresh();
    w.zone = pm::zone_stonegym;
    w.tx = 6;
    w.ty = 9;                        // the door is at 6,15, six rows south
    w.mode = pm::Mode::Overworld;
    w.fade = 0;
    Frame f;
    render(w, f, 1000);

    // Judged by where the door's pixels sit on average, not by whether any
    // stray pixel of that shade appears higher up: a mesh face somewhere else
    // in the room can land on the same colour, and a flipped map would move
    // the whole patch rather than a couple of pixels of it.
    const pm::TileDef& door = pm::k_tiles[pm::tile_door];
    int n = 0;
    long sum_y = 0;
    for (int y = 0; y < k_h; y++) {
        for (int x = 0; x < k_w; x++) {
            const uint8_t* p = f.at(x, y);
            if (p[0] != door.r || p[1] != door.g || p[2] != door.b) continue;
            n++;
            sum_y += y;
        }
    }
    const int mean_y = n ? int(sum_y / n) : -1;
    std::printf("  gym: %d door pixels, average row %d\n", n, mean_y);
    check(n > 20, "the door south of the player is drawn");
    check(mean_y > 70, "and it is below the player, because north is up");
}

// A room is not a field. Interiors used to draw the sky gradient and stand a
// cottage on every wall tile, because the walls were the same tile character
// as an outdoor house.
void test_indoors_has_no_sky() {
    pm::World w = fresh();
    w.zone = pm::zone_picomart;
    w.tx = 5;
    w.ty = 5;
    w.mode = pm::Mode::Overworld;
    w.fade = 0;
    Frame f;
    render(w, f, 1000);
    const int sky = count_sky(f, 0, k_h);
    std::printf("  mart: %d sky pixels indoors\n", sky);
    check(sky == 0, "there is no sky inside a building");
    check(count_distinct_colours(f, 0, k_h) >= 4, "and the room has a room in it");
}

void test_the_shop_screen_draws() {
    pm::World w = fresh();
    w.zone = pm::zone_picomart;
    w.tx = 5;
    w.ty = 4;
    w.facing = 0;
    w.mode = pm::Mode::Overworld;
    w.fade = 0;
    for (int i = 0; i < 20 && w.mode != pm::Mode::Shop; i++) {
        pm::Input go{};
        go.a_pressed = true;
        pm::world_tick(w, go);
    }
    check(w.mode == pm::Mode::Shop, "the clerk opens a counter");
    Frame f;
    render(w, f, 1000);
    // The list, the cursor and the description panel, plus the gold the money
    // and the cursor are drawn in.
    check(count_distinct_colours(f, 0, k_h) >= 4, "the shop draws its panels");
    int gold = 0;
    for (int y = 0; y < k_h; y++)
        for (int x = 0; x < k_w; x++) {
            const uint8_t* p = f.at(x, y);
            if (p[0] > 0xE0 && p[1] > 0x90 && p[1] < 0xD0 && p[2] < 0x60) gold++;
        }
    std::printf("  shop: %d gold pixels\n", gold);
    check(gold > 10, "the money and the cursor are drawn");
}

// Trees are sprites now. The check is not "a sprite was drawn" but "the thing
// that makes a tree a tree is on screen": the tree palette's outline and its
// trunk, neither of which any other material in the overworld uses.
void test_the_treeline_is_drawn() {
    pm::World w = fresh();
    w.zone = pm::zone_route1;
    w.tx = 11;
    w.ty = 3;                        // deep in the border trees
    w.mode = pm::Mode::Overworld;
    w.fade = 0;
    Frame f;
    render(w, f, 1000);

    int trunk = 0, outline = 0;
    for (int y = 0; y < k_h; y++) {
        for (int x = 0; x < k_w; x++) {
            const uint8_t* p = f.at(x, y);
            if (p[0] == 0x77 && p[1] == 0x55 && p[2] == 0x33) trunk++;
            if (p[0] == 0x22 && p[1] == 0x33 && p[2] == 0x22) outline++;
        }
    }
    std::printf("  route: %d trunk pixels, %d tree outline pixels\n",
                trunk, outline);
    check(trunk > 20, "the trees have trunks");
    check(outline > 100, "and the outlines that make them read as pixel art");
}

// Two kinds of tree, and which is which is level data rather than a runtime
// guess. tools/picomon_data.py floods inward from the map edge and rewrites
// the trees the outside cannot reach, so the wall that frames the map stays
// billboards and the ones the player walks around become geometry.
//
// This pins the split itself, not a screenshot of it. Both failure modes are
// silent otherwise: a flood fill that reaches everything turns every tree
// back into a sprite and nothing looks wrong, and one that reaches nothing
// puts twenty triangles on all hundred and thirty border tiles of Hometown
// and only shows up as a frame rate on hardware nobody is holding.
void test_the_border_is_sprites_and_the_inside_is_geometry() {
    int border = 0, inside = 0, inside_on_the_edge = 0;
    for (int zi = 0; zi < pm::k_zone_count; zi++) {
        const pm::Zone& z = pm::k_zones[zi];
        for (int y = 0; y < z.h; y++) {
            for (int x = 0; x < z.w; x++) {
                const uint8_t t = z.tiles[y * z.w + x];
                if (t == pm::tile_tree) border++;
                if (t != pm::tile_treecore) continue;
                inside++;
                if (x == 0 || y == 0 || x == z.w - 1 || y == z.h - 1)
                    inside_on_the_edge++;
            }
        }
    }
    std::printf("  trees: %d border sprites, %d interior meshes\n",
                border, inside);
    check(border > 100, "the map is still framed by sprite trees");
    check(inside > 0, "and some trees inside it are geometry");
    check(inside * 4 < border,
          "the geometry is the small half: it costs 20 triangles a tile and "
          "the border does not");
    check(inside_on_the_edge == 0,
          "nothing on the outermost ring became geometry, because the flood "
          "fill starts there");
}

// And the mesh actually reaches the screen. The tile can be labelled
// perfectly and still draw nothing, which is what a missing case in the prop
// switch looks like: the tile keeps its ground colour and the tree is simply
// absent, on a map where absent trees are unremarkable.
void test_an_interior_tree_is_drawn() {
    pm::World w = fresh();
    w.zone = pm::zone_route1;
    // Standing two tiles south of the lone tree at 4,2, which the flood fill
    // cannot reach: the row above it and both sides are open ground.
    w.tx = 4;
    w.ty = 4;
    w.facing = 0;
    w.mode = pm::Mode::Overworld;
    w.fade = 0;
    Frame f;
    render(w, f, 1000);

    // Counting green would pass on the border treeline alone, which is most
    // of this frame and is exactly what the test must not credit. So it
    // counts green that could only have been shaded: the ground is drawn as
    // flat tile colours and a sprite is drawn straight out of its palette, so
    // every green on screen is one of a known handful unless a lit mesh put
    // it there.
    static const uint8_t k_flat[][3] = {
        {0x55, 0x99, 0x44}, {0x44, 0x88, 0x44}, {0x33, 0x77, 0x33},  // ground
        {0x22, 0x33, 0x22}, {0x22, 0x66, 0x33}, {0x33, 0x88, 0x44},  // sprite
        {0x55, 0xAA, 0x55}, {0x77, 0xCC, 0x66},
    };
    int shaded = 0;
    for (int y = 0; y < k_h; y++) {
        for (int x = 0; x < k_w; x++) {
            const uint8_t* p = f.at(x, y);
            if (!(p[1] > p[0] && p[1] > p[2])) continue;
            bool flat = false;
            for (const auto& c : k_flat)
                if (p[0] == c[0] && p[1] == c[1] && p[2] == c[2]) flat = true;
            if (!flat) shaded++;
        }
    }
    std::printf("  interior tree: %d shaded green pixels\n", shaded);
    check(shaded > 40,
          "the mesh tree in the middle of Route 1 is on screen, in greens no "
          "flat tile and no sprite palette entry could have produced");
}


// Tall grass grows tufts. The encounter tile used to be nothing but a
// darker ground colour, so the one tile the game is about looked like lawn.
//
// The counted colours are the mockup's PAL_GRASS, and two of the three are
// unique to it: 226633 and 55AA55 appear nowhere else in an overworld
// frame. 338844 is deliberately excluded even though the tuft uses it,
// because the tree sprites use it too, and a count that trees can satisfy
// is a count that proves nothing about grass.
int count_grass(const Frame& f) {
    int n = 0;
    for (int y = 0; y < k_h; y++) {
        for (int x = 0; x < k_w; x++) {
            const uint8_t* p = f.at(x, y);
            if ((p[0] == 0x22 && p[1] == 0x66 && p[2] == 0x33) ||
                (p[0] == 0x55 && p[1] == 0xAA && p[2] == 0x55)) n++;
        }
    }
    return n;
}

pm::World on_route(int tx, int ty) {
    pm::World w = fresh();
    w.zone = pm::zone_route1;
    w.mode = pm::Mode::Overworld;
    w.fade = 0;
    w.tx = int8_t(tx);
    w.ty = int8_t(ty);
    return w;
}

void test_tall_grass_grows_tufts() {
    pm::World w = on_route(11, 6);       // on the path beside the west patch
    Frame f;
    render(w, f, 0);
    const int n = count_grass(f);
    std::printf("  grass: %d tuft pixels\n", n);
    check(n > 25, "the tall grass is drawn as tufts, not as a flat colour");
}

void test_the_grass_sways() {
    pm::World w = on_route(11, 6);
    Frame a, b;
    render(w, a, 0);
    render(w, b, 700);
    int moved = 0;
    for (int y = 0; y < k_h; y++) {
        for (int x = 0; x < k_w; x++) {
            const uint8_t* pa = a.at(x, y);
            const uint8_t* pb = b.at(x, y);
            const bool ta = pa[0] == 0x55 && pa[1] == 0xAA && pa[2] == 0x55;
            const bool tb = pb[0] == 0x55 && pb[1] == 0xAA && pb[2] == 0x55;
            if (ta != tb) moved++;
        }
    }
    std::printf("  grass: %d lit pixels moved between two moments\n", moved);
    check(moved > 5, "the tufts move with time, so the field sways");
}

// The parting. Standing in the grass pushes every blade within the parting
// radius away and flattens it, so the space the player's body takes up is
// visibly clear. The camera is locked to the player, which is what makes
// the feet a constant screen position: measured once at (59, 58), and if
// the camera ever moves this fails loudly rather than drifting.
void test_the_grass_parts_around_the_player() {
    pm::World w = on_route(5, 6);        // standing inside the west patch
    Frame f;
    render(w, f, 0);
    check(count_grass(f) > 25, "the rest of the patch still has its tufts");
    int inside = 0;
    for (int y = 55; y <= 61; y++) {
        for (int x = 56; x <= 62; x++) {
            const uint8_t* p = f.at(x, y);
            if ((p[0] == 0x22 && p[1] == 0x66 && p[2] == 0x33) ||
                (p[0] == 0x55 && p[1] == 0xAA && p[2] == 0x55)) inside++;
        }
    }
    std::printf("  grass: %d tuft pixels at the player's feet\n", inside);
    check(inside == 0,
          "no tuft stands inside the parting radius, so the grass opens "
          "around the body standing in it");
}

// The strike. Every one of these was missing: a turn resolved, a line of text
// appeared, and nothing on screen moved, so a player could not tell a hit
// from a miss or a strong hit from a weak one.
void test_a_hit_flashes_shakes_and_drains() {
    pm::World base = fresh();
    base.mode = pm::Mode::Battle;
    base.battle = pm::Battle{};
    base.battle.foe = pm::make_mon(2, 12);
    base.battle.trainer_npc = 0xFF;
    base.battle.active = 0;
    base.battle.state = pm::BattleState::Attack;
    base.battle.player_first = true;
    // The tick the blow lands on: the flash and the burst are brightest here
    // and decay over the four ticks after it, so this is where they are
    // measurable rather than merely present.
    base.battle.timer = 11;

    // A frame with nothing landing, as the control.
    pm::World quiet = base;
    Frame calm;
    render(quiet, calm, 1000);

    // And the same frame with a hit on the foe recorded.
    pm::World hit = base;
    hit.battle.fx_dmg[pm::Battle::k_foe] = 9;
    hit.battle.fx_mult[pm::Battle::k_foe] = 8;      // super effective
    hit.battle.fx_type[pm::Battle::k_foe] = uint8_t(pm::Type::Ember);
    Frame flash;
    render(hit, flash, 1000);

    // The foe stands in the upper right of the arena. The window stops short
    // of y 55, where the player's own HP plate starts: that panel is white
    // trim and would drown the signal.
    auto near_white = [](const Frame& f) {
        int n = 0;
        for (int y = 22; y < 52; y++)
            for (int x = 68; x < 112; x++) {
                const uint8_t* p = f.at(x, y);
                if (p[0] > 0xD0 && p[1] > 0xD0 && p[2] > 0xD0) n++;
            }
        return n;
    };
    const int calm_white = near_white(calm), flash_white = near_white(flash);
    std::printf("  battle: %d white pixels calm, %d mid strike\n",
                calm_white, flash_white);
    check(flash_white > calm_white + 15, "the creature taking a hit whitens");

    // The burst throws the attacking type's colour around it. Ember is warm,
    // and nothing else in that corner of the arena is.
    auto warm_sparks = [](const Frame& f) {
        int n = 0;
        for (int y = 15; y < 54; y++)
            for (int x = 60; x < 118; x++) {
                const uint8_t* p = f.at(x, y);
                if (p[0] > 0xE0 && p[1] > 0x60 && p[1] < 0xC0 && p[2] < 0x60) n++;
            }
        return n;
    };
    std::printf("  battle: %d burst pixels\n", warm_sparks(flash));
    check(warm_sparks(flash) > warm_sparks(calm) + 4,
          "and a burst in the attacking move's colour");

    // The bar drains across the strike rather than having already jumped:
    // partway through, the foe's bar has to be wider than its real HP.
    auto bar_width = [](const Frame& f) {
        int n = 0;
        for (int x = 0; x < k_w; x++) {
            const uint8_t* p = f.at(x, 17);          // the foe plate's bar row
            if (p[1] > p[0] && p[1] > p[2] && p[1] > 0x90) n++;
        }
        return n;
    };
    pm::World done = hit;
    done.battle.timer = 0;                            // the beat has finished
    Frame settled;
    render(done, settled, 1000);
    std::printf("  battle: bar %d px mid strike, %d px after\n",
                bar_width(flash), bar_width(settled));
    check(bar_width(flash) > bar_width(settled),
          "the HP bar is still draining mid strike");
}

// The one at the front is the bigger one.
//
// The camera gives the player's side 13 pixels per world unit and the foe's
// 10, so the same species drawn at the same scale is only 30 percent smaller
// at the back, which does not read as distance at all. An attempt to pay the
// foe back for its distance overshot and inverted it: the same species came
// out taller at the back than at the front, and the depth illusion broke
// completely. Both creatures here are EMBERKIT, so any difference on screen
// is staging and nothing else.
void test_the_near_creature_is_the_bigger_one() {
    pm::World w = fresh();
    w.mode = pm::Mode::Battle;
    w.battle = pm::Battle{};
    w.battle.foe = pm::make_mon(0, 5);          // the same species as the starter
    w.battle.trainer_npc = 0xFF;
    w.battle.active = 0;
    w.battle.state = pm::BattleState::Menu;
    check(w.party[0].species == w.battle.foe.species,
          "both sides of this test are the same species");
    Frame f;
    render(w, f, 1000);

    // Emberkit is orange and nothing else in the arena is. Measure each side's
    // patch of it, splitting the frame down the middle between them.
    auto span = [&f](int x0, int x1, int& top, int& bottom, int& count) {
        top = 999; bottom = -1; count = 0;
        for (int y = 0; y < 84; y++)
            for (int x = x0; x < x1; x++) {
                const uint8_t* p = f.at(x, y);
                if (!(p[0] > 0xA0 && p[1] > 0x40 && p[1] < 0xA0 && p[2] < 0x50))
                    continue;
                if (y < top) top = y;
                if (y > bottom) bottom = y;
                count++;
            }
    };
    int mt, mb, mc, ft, fb, fc;
    span(0, 58, mt, mb, mc);            // the player's half
    span(62, 120, ft, fb, fc);          // the foe's half
    const int mine = mb - mt, theirs = fb - ft;
    std::printf("  battle: same species, near %d px tall (%d px), "
                "far %d px tall (%d px)\n", mine, mc, theirs, fc);
    check(mc > 60 && fc > 20, "both creatures are actually on screen");
    // Measured off the design mockup: about 35 pixels at the front and 22 at
    // the back. The arena runs a little larger than that on purpose, but the
    // ratio is the thing being pinned here, and the band is wide enough to
    // retune inside and narrow enough that inverting it fails.
    check(mine * 100 > theirs * 140,
          "the creature at the front is at least 1.4x the one at the back, or "
          "nothing about the arena reads as depth");
    check(mine * 100 < theirs * 250,
          "and not so much bigger that the far one is a speck");
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
    test_north_is_up();
    test_indoors_has_no_sky();
    test_the_shop_screen_draws();
    test_the_treeline_is_drawn();
    test_the_border_is_sprites_and_the_inside_is_geometry();
    test_an_interior_tree_is_drawn();
    test_tall_grass_grows_tufts();
    test_the_grass_sways();
    test_the_grass_parts_around_the_player();
    test_a_hit_flashes_shakes_and_drains();
    test_the_near_creature_is_the_bigger_one();
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
