// What Joker Reels' rules promise.
//
// The mockup this game was designed in found four bugs by being run and looked
// at, and three of them were in this half. They are all checks here now, and
// each one is written against something OTHER than the code it is checking,
// because a check that asks the code whether it agrees with itself cannot
// fail.

#include <cmath>
#include <cstdio>
#include <cstring>

#include "render.hpp"
#include "sim.hpp"

namespace {

int failures = 0;
int checks = 0;

void check(bool ok, const char* what) {
    checks++;
    if (!ok) {
        failures++;
        std::printf("FAIL %s\n", what);
    }
}

jr::Buttons none() { return jr::Buttons{}; }

jr::Buttons press_a() {
    jr::Buttons b{};
    b.a = true;
    b.any = true;
    return b;
}

void play(jr::World& w, int ticks) {
    for (int i = 0; i < ticks; i++) jr::world_tick(w, none());
}

// A world at the table, ready to pull. The opening is the title and then the
// how to play pages, so getting there is several presses and not one.
jr::World started(uint32_t seed) {
    jr::World w;
    jr::world_init(w, seed);
    for (int i = 0; i < 2 + jr::k_learn_pages && w.state != jr::kIdle; i++) {
        jr::world_tick(w, press_a());
    }
    return w;
}

/* Where a facet actually is, computed the way the RENDERER computes it rather
 * than the way the rules do.
 *
 * render.cpp places facet f at (0, -R sin a, -R cos a) with its outward normal
 * along the same vector, and the camera sits at -k_cam_dist on z. So a facet
 * is visible exactly while its normal points back at the camera, which is a
 * different question to "is its angle small" and is the one that matters.
 */
float facet_angle_of(const jr::World& w, int drum, int facet) {
    return static_cast<float>(jr::facet_mid(w.angle[drum], facet)) *
           (2.0f * jrr::k_pi / jr::k_turn);
}

// The dot product the rasterizer's winding test comes down to for a facet on
// this drum: positive means the face is turned toward the camera and would be
// drawn, negative means it is backface culled.
float facing(const jr::World& w, int drum, int facet) {
    const float a = facet_angle_of(w, drum, facet);
    const float ny = -std::sin(a);
    const float nz = -std::cos(a);
    // Vector from the facet's centre to the eye.
    const float ey = -jrr::k_drum_radius * ny;
    const float ez = -jrr::k_cam_dist - jrr::k_drum_radius * nz;
    const float len = std::sqrt(ey * ey + ez * ez);
    return (ny * ey + nz * ez) / (len > 0.0f ? len : 1.0f);
}

// Where the facet's centre lands vertically, in window rows.
float facet_row(const jr::World& w, int drum, int facet) {
    const float a = facet_angle_of(w, drum, facet);
    const float wy = -jrr::k_drum_radius * std::sin(a);
    const float wz = -jrr::k_drum_radius * std::cos(a);
    const float depth = jrr::k_cam_dist + wz;
    const float focal =
        1.0f / std::tan(jrr::k_fov_degrees * jrr::k_pi / 180.0f * 0.5f);
    const float scale = focal * (jrr::k_screen_w - 1) * 0.5f / depth;
    return jrr::k_window_h * 0.5f - wy * scale;
}

// ---------------------------------------------------------------------------

void test_the_scored_facet_is_the_one_being_looked_at() {
    // The bug this replaces: the front was taken to be where the cosine is at
    // its maximum, which is the FAR side of the drum. The game scored whichever
    // symbol was hidden round the back, so a drum could show a BAR while the
    // panel under it said PLUM.
    for (uint32_t seed = 1; seed <= 12; seed++) {
        jr::World w = started(seed * 977u);
        for (int d = 0; d < jr::k_drums; d++) {
            for (int step = 0; step < 24; step++) {
                w.angle[d] = step * jr::k_step_one / 2 + 37;
                // Land it the way a stop does.
                jr::Buttons b = press_a();
                w.state = jr::kSpin;
                w.spinning[0] = w.spinning[1] = w.spinning[2] = false;
                w.spinning[d] = true;
                jr::world_tick(w, b);

                const int f = jr::front_facet(w, d);

                // 1. It is the facet the projector puts nearest the camera.
                int nearest = -1;
                float best = 1e9f;
                for (int i = 0; i < jr::k_facets; i++) {
                    const float a = facet_angle_of(w, d, i);
                    const float depth =
                        jrr::k_cam_dist - jrr::k_drum_radius * std::cos(a);
                    if (depth < best) { best = depth; nearest = i; }
                }
                if (nearest != f) {
                    std::printf("  drum %d step %d: rules say facet %d, "
                                "projector says %d\n", d, step, f, nearest);
                }
                check(nearest == f, "the scored facet is the nearest one");

                // 2. It is one the rasterizer would actually draw.
                check(facing(w, d, f) > 0.0f,
                      "the scored facet is not backface culled");

                // 3. Its middle lands on the payline row, which is what makes
                //    a drum come to rest showing a symbol rather than a seam.
                const float row = facet_row(w, d, f);
                if (std::fabs(row - jrr::k_window_h * 0.5f) > 1.5f) {
                    std::printf("  drum %d step %d: scored facet sits at row "
                                "%.1f, payline is %d\n", d, step, row,
                                jrr::k_window_h / 2);
                }
                check(std::fabs(row - jrr::k_window_h * 0.5f) <= 1.5f,
                      "the scored facet rests on the payline");

                // 4. And it carries what the rules say landed on it.
                check(w.landed[d] == jr::face_at(w, d, f),
                      "landed is what the front facet carries");
            }
        }
    }
}

void test_a_facet_only_changes_symbol_out_of_sight() {
    // The whole reason a facet's symbol is state rather than a formula. The
    // formula version advances a twelve entry window at the instant a facet is
    // dead centre front, and it shifts EVERY facet at once, so the symbol you
    // are looking at changes identity while you are looking at it.
    //
    // Checked against facing(), which is the renderer's own visibility, and
    // never against facet_hidden(), which is the code being checked.
    int repaints = 0;
    for (int speed = 0; speed < jr::k_speeds; speed++) {
        jr::World w = started(17u);
        // A strip longer than the drum has facets, which is the only case
        // where a facet is ever repainted at all. Without this the check
        // passes by never exercising the thing it is checking.
        for (int d = 0; d < jr::k_drums; d++) {
            for (int i = jr::k_facets; i < 20; i++) {
                w.strip[d][i] = static_cast<uint8_t>(i % jr::k_symbols);
            }
            w.strip_len[d] = 20;
        }
        w.speed = static_cast<uint8_t>(speed);
        jr::world_tick(w, press_a());       // pull

        uint8_t before[jr::k_drums][jr::k_facets];
        for (int t = 0; t < 400 && w.state == jr::kSpin; t++) {
            for (int d = 0; d < jr::k_drums; d++) {
                for (int f = 0; f < jr::k_facets; f++) before[d][f] = w.facet[d][f];
            }
            jr::world_tick(w, none());
            for (int d = 0; d < jr::k_drums; d++) {
                for (int f = 0; f < jr::k_facets; f++) {
                    if (before[d][f] == w.facet[d][f]) continue;
                    repaints++;
                    checks++;
                    if (facing(w, d, f) > 0.0f) {
                        failures++;
                        std::printf("FAIL %s: drum %d facet %d changed symbol "
                                    "while it was on screen (facing %.3f)\n",
                                    jr::speed_name(static_cast<uint8_t>(speed)),
                                    d, f, facing(w, d, f));
                    }
                }
            }
        }
    }
    std::printf("  %d facet repaints, all of them out of sight\n", repaints);
    check(repaints > 0,
          "some facet was actually repainted, so the check proved something");
}

void test_a_hands_off_spin_ends() {
    // Reels used to stop only when the player stopped them, so a hands off
    // spin turned for ever and the joker that pays x2 for stopping nothing was
    // unreachable.
    for (uint32_t seed = 1; seed <= 20; seed++) {
        jr::World w = started(seed * 31u);
        jr::world_tick(w, press_a());
        check(w.state == jr::kSpin, "a pull starts a spin");
        int ticks = 0;
        while (w.state == jr::kSpin && ticks < 1000) {
            jr::world_tick(w, none());
            ticks++;
        }
        check(w.state != jr::kSpin, "a hands off spin ends on its own");
        check(ticks < 400, "and it ends in about four seconds, not eventually");
        for (int d = 0; d < jr::k_drums; d++) {
            check(w.stopped_at[d] < 0,
                  "a reel that stopped itself pays no speed bonus");
        }
    }
}

void test_stopping_pays_and_costs() {
    jr::World w = started(5u);
    jr::world_tick(w, press_a());
    w.speed = jr::kWild;
    for (int d = 0; d < jr::k_drums; d++) jr::world_tick(w, press_a());
    // One more tick to notice. A tick counts what is still turning BEFORE it
    // applies the stop, so the reel the last press stopped is already still
    // when the next tick looks. That is one frame, and it is the difference
    // between the count starting under a settled drum and under a moving one.
    check(w.state == jr::kSpin, "the last stop lands on the tick that made it");
    jr::world_tick(w, none());
    check(w.state == jr::kCount, "and the count starts on the next one");
    for (int d = 0; d < jr::k_drums; d++) {
        check(w.stopped_at[d] == jr::kWild, "each reel remembers its speed");
    }
    int wild_lines = 0;
    for (int i = 0; i < w.tally_len; i++) {
        if (w.tally[i].mult == jr::speed_mult(jr::kWild)) wild_lines++;
    }
    check(wild_lines >= jr::k_drums, "every stopped reel pays its mult");
}

void test_hands() {
    // Every shape five reels can make, and the reels that made it. The order
    // the shapes are tested in is the whole of this function's risk: a full
    // house read as three of a kind scores less and loses the pair, and a run
    // read as nothing loses everything.
    struct Case {
        uint8_t landed[jr::k_drums];
        uint8_t hand;
        uint8_t groups;         // how many match lines it should draw
        const char* what;
    };
    const Case cases[] = {
        {{jr::kBell, jr::kBell, jr::kBell, jr::kBell, jr::kBell},
         jr::kFive, 1, "five of a kind"},
        {{jr::kBell, jr::kBell, jr::kCrown, jr::kBell, jr::kBell},
         jr::kFour, 1, "four of a kind, in any position"},
        {{jr::kBell, jr::kCrown, jr::kBell, jr::kCrown, jr::kBell},
         jr::kFullHouse, 2, "a full house is three and a pair"},
        {{jr::kCherry, jr::kBell, jr::kPlum, jr::kBar, jr::kClover},
         jr::kRun, 1, "five consecutive symbols"},
        {{jr::kClover, jr::kBar, jr::kPlum, jr::kBell, jr::kCherry},
         jr::kRun, 1, "a run in any order"},
        {{jr::kBell, jr::kBell, jr::kBell, jr::kCrown, jr::kCherry},
         jr::kThree, 1, "three of a kind"},
        {{jr::kBell, jr::kBell, jr::kCrown, jr::kCrown, jr::kCherry},
         jr::kTwoPair, 2, "two pair"},
        {{jr::kBell, jr::kBell, jr::kCrown, jr::kSeven, jr::kCherry},
         jr::kPair, 1, "a pair"},
        {{jr::kCherry, jr::kPlum, jr::kClover, jr::kDiamond, jr::kCrown},
         jr::kNothing, 0, "and nothing"},
    };

    for (const Case& c : cases) {
        const uint8_t hand = jr::hand_of(c.landed);
        if (hand != c.hand) {
            std::printf("  %s: got %s, wanted %s\n", c.what,
                        jr::hand_name(hand), jr::hand_name(c.hand));
        }
        check(hand == c.hand, c.what);

        uint8_t groups[jr::k_drums];
        const uint8_t n = jr::hand_groups(c.landed, groups);
        check(n == c.groups, "the right number of match lines");

        // A reel is in a group only if it shares its symbol with another reel
        // in that group, and a run puts every reel in one. Checked against the
        // landed symbols rather than against hand_groups' own reasoning.
        for (int d = 0; d < jr::k_drums; d++) {
            if (groups[d] == jr::k_no_group) continue;
            check(groups[d] < n, "a group index is one of the groups drawn");
            if (hand == jr::kRun) continue;
            int shared = 0;
            for (int e = 0; e < jr::k_drums; e++) {
                if (e != d && groups[e] == groups[d] &&
                    c.landed[e] == c.landed[d]) {
                    shared++;
                }
            }
            check(shared >= 1, "a reel in a group matches another reel in it");
        }
        if (hand == jr::kRun) {
            int in = 0;
            for (int d = 0; d < jr::k_drums; d++) {
                if (groups[d] != jr::k_no_group) in++;
            }
            check(in == jr::k_drums, "a run draws through every reel");
        }
    }

    // The ladder has to be a ladder: a better shape is worth more, at every
    // level. Nothing else checks this and a table is exactly where a typo
    // hides.
    for (int h = 1; h < jr::k_hands; h++) {
        for (int level = 1; level <= 3; level++) {
            const int better = jr::hand_chips(static_cast<uint8_t>(h - 1), level) *
                               jr::hand_mult(static_cast<uint8_t>(h - 1), level);
            const int worse = jr::hand_chips(static_cast<uint8_t>(h), level) *
                              jr::hand_mult(static_cast<uint8_t>(h), level);
            if (better <= worse) {
                std::printf("  %s (%d) is not worth more than %s (%d) at LV%d\n",
                            jr::hand_name(static_cast<uint8_t>(h - 1)), better,
                            jr::hand_name(static_cast<uint8_t>(h)), worse, level);
            }
            check(better > worse, "a better hand is worth more");
        }
    }
}

void test_a_swap_changes_what_a_drum_can_land_on() {
    jr::World w = started(9u);
    w.state = jr::kSwap;
    w.swap_drum = 1;
    w.swap_face = 3;
    w.swap_to = jr::kCrown;
    const uint8_t was = w.strip[1][3];
    check(was != jr::kCrown, "the face was not already a crown");
    jr::world_tick(w, press_a());
    check(w.strip[1][3] == jr::kCrown, "the strip entry changed");
    check(w.state == jr::kShop, "and the screen closes");

    // The panel names an entry and the drum has to be showing that entry, or
    // the screen is lying about what you are buying.
    jr::World look = started(9u);
    look.state = jr::kSwap;
    look.swap_drum = 1;
    for (int entry = 0; entry < look.strip_len[1]; entry++) {
        look.swap_face = static_cast<uint8_t>(entry);
        play(look, 60);
        const int f = jr::front_facet(look, 1);
        check(jr::face_at(look, 1, f) == look.strip[1][entry],
              "the drum shows the entry the panel names");
    }
}

void test_a_strip_longer_than_the_drum_still_reaches_every_symbol() {
    // The point of the facet trick: 24 symbols on 12 facets.
    jr::World w = started(23u);
    for (int i = 0; i < jr::k_strip_max; i++) {
        w.strip[0][i] = static_cast<uint8_t>(i % jr::k_symbols);
    }
    w.strip_len[0] = jr::k_strip_max;
    w.speed = jr::kSlow;
    jr::world_tick(w, press_a());

    bool seen[jr::k_symbols] = {false};
    for (int t = 0; t < 4000; t++) {
        if (w.state != jr::kSpin) {
            // Keep the run alive. Letting it end reseeds the world, which
            // regenerates the strips: the first version of this test spent
            // most of its ticks measuring a twelve entry strip it had not
            // written, and reported five symbols of eight as a renderer bug.
            w.spins = jr::k_spins_per_round;
            w.banked = 0;
            w.state = jr::kIdle;
            jr::world_tick(w, press_a());
            if (w.state != jr::kSpin) break;
        }
        jr::world_tick(w, none());
        seen[jr::face_at(w, 0, jr::front_facet(w, 0))] = true;
    }
    int reached = 0;
    for (int s = 0; s < jr::k_symbols; s++) if (seen[s]) reached++;
    std::printf("  a 24 entry strip reached %d of %d symbols\n",
                reached, jr::k_symbols);
    check(reached == jr::k_symbols,
          "every symbol on a 24 entry strip comes round on 12 facets");
}

void test_a_run_is_finite_and_a_shop_is_a_choice() {
    jr::World w = started(3u);
    // The opening target is about one spin of the hands off floor, which is
    // measured in test_the_floor_loses_and_skill_wins rather than assumed.
    check(jr::target_for_ante(1) == 4000, "the first ante wants 4000");
    check(jr::target_for_ante(8) > jr::target_for_ante(7) * 3 / 2,
          "and each one wants half again more");

    w.banked = w.target;
    w.state = jr::kCleared;
    jr::world_tick(w, press_a());
    check(w.state == jr::kShop, "clearing an ante opens the shop");
    check(w.shop_len == jr::k_shop_items, "which has four things in it");
    int jokers = 0, hands = 0, swaps = 0;
    for (int i = 0; i < w.shop_len; i++) {
        if (w.shop[i].kind == jr::kShopJoker) jokers++;
        if (w.shop[i].kind == jr::kShopHand) hands++;
        if (w.shop[i].kind == jr::kShopSwap) swaps++;
    }
    check(jokers == 2 && hands == 1 && swaps == 1,
          "two jokers, a hand to level, and a drum to open");

    // A joker you already hold is never offered again.
    jr::World held = started(3u);
    held.joker_count = 0;
    for (uint8_t j = 0; j < jr::k_jokers - 1; j++) {
        if (held.joker_count < jr::k_max_jokers) held.jokers[held.joker_count++] = j;
    }
    jr::world_open_shop(held);
    for (int i = 0; i < held.shop_len; i++) {
        if (held.shop[i].kind != jr::kShopJoker) continue;
        for (int h = 0; h < held.joker_count; h++) {
            check(held.shop[i].which != held.jokers[h],
                  "the shop never offers a joker you hold");
        }
    }
}

/* The shop is a column, so it has to move on the column's axis.
 *
 * It only read left and right, which is the one axis the cards do not run
 * along: four of them stacked down the screen, with NEXT ANTE under the last,
 * driven sideways. Both axes work now, and this checks BOTH, because making
 * up and down work by taking left and right away would be a different bug
 * with the same shape.
 */
void test_the_shop_moves_on_both_axes() {
    for (int axis = 0; axis < 2; axis++) {
        jr::World w = started(19u);
        w.banked = w.target;
        w.state = jr::kCleared;
        jr::world_tick(w, press_a());
        check(w.state == jr::kShop, "the shop opened");
        check(w.shop_sel == 0, "on the first card");

        jr::Buttons on{};
        jr::Buttons back{};
        if (axis == 0) { on.right = true; back.left = true; }
        else { on.down = true; back.up = true; }
        on.any = true;
        back.any = true;

        // All the way down, one press at a time, through the cards and on to
        // REROLL and NEXT ANTE. The shop is one list and every row of it has
        // to be reachable on the axis the list runs along.
        const int rows = jr::shop_next_index(w);
        for (int i = 0; i < rows; i++) {
            const uint8_t before = w.shop_sel;
            jr::world_tick(w, on);
            check(w.shop_sel == before + 1, "a press moves one row");
        }
        check(w.shop_sel == jr::shop_reroll_index(w) + 1,
              "and REROLL is a row of its own before NEXT ANTE");
        check(w.shop_sel == jr::shop_next_index(w), "and stops on NEXT ANTE");
        jr::world_tick(w, on);
        check(w.shop_sel == jr::shop_next_index(w), "and goes no further");

        for (int i = 0; i < rows; i++) jr::world_tick(w, back);
        check(w.shop_sel == 0, "and all the way back to the first card");
        jr::world_tick(w, back);
        check(w.shop_sel == 0, "and no further than that either");
        check(w.state == jr::kShop, "without leaving the shop");
    }
}

/* The instructions are a menu, and a menu you can get out of.
 *
 * B opens them from the table and from the shop, and closing them puts the
 * player back where they were. Landing on the machine instead is not a
 * cosmetic difference: the shop is the only place gold can be spent, and it
 * is built once per ante, so being dropped out of it silently ends the round's
 * shopping with the gold still in your pocket.
 */
void test_the_instructions_come_back_to_where_they_opened() {
    jr::Buttons b{};
    b.b = true;
    b.any = true;

    jr::World table = started(23u);
    check(table.state == jr::kIdle, "at the table");
    jr::world_tick(table, b);
    check(table.state == jr::kLearn, "B opens the instructions");
    check(table.learn_page == 0, "at the first page");
    for (int i = 0; i < jr::k_learn_pages; i++) jr::world_tick(table, press_a());
    check(table.state == jr::kIdle, "and paging off the end goes back");

    jr::World shop = started(23u);
    shop.banked = shop.target;
    shop.state = jr::kCleared;
    jr::world_tick(shop, press_a());
    const uint16_t gold = shop.gold;
    const uint8_t len = shop.shop_len;
    jr::world_tick(shop, b);
    check(shop.state == jr::kLearn, "and B opens them from the shop too");
    for (int i = 0; i < jr::k_learn_pages; i++) jr::world_tick(shop, press_a());
    check(shop.state == jr::kShop, "which is where paging off the end returns");
    check(shop.gold == gold && shop.shop_len == len,
          "with the same gold and the same shelf");

    // Left goes back, and off the front of page one it closes.
    jr::Buttons left{};
    left.left = true;
    left.any = true;
    jr::World page = started(23u);
    jr::world_tick(page, b);
    jr::world_tick(page, press_a());
    check(page.learn_page == 1, "a press turns the page");
    jr::world_tick(page, left);
    check(page.learn_page == 0 && page.state == jr::kLearn, "left turns it back");
    jr::world_tick(page, left);
    check(page.state == jr::kIdle, "and left off page one closes the menu");
}

/* A joker's tally entry names the SLOT it came from, and is held longer.
 *
 * The screen shakes a slot and pops a number over the equation, and it can
 * only do either if the entry says which of the five did the work. So this
 * checks the slot against the entry's own NAME: an off by one would shake the
 * neighbour of the joker that scored, which is not a crash, not a wrong total,
 * and not something anybody would catch by playing.
 *
 * Run over many seeds rather than one, because which jokers fire depends on
 * what landed, and a single spin can easily be one where only one of them
 * does. UNDERSTUDY is the slot worth reaching: it scores whatever is on its
 * left and pays under its own name, so it is the one where the slot and the
 * effect genuinely come from different places.
 */
void test_a_joker_entry_names_its_slot() {
    int entries = 0;
    bool slot_seen[3] = {false, false, false};
    for (int seed = 1; seed <= 40; seed++) {
        jr::World w = started(static_cast<uint32_t>(seed) * 3557u);
        w.joker_count = 3;
        w.jokers[0] = jr::kTwin;
        w.jokers[1] = jr::kUnderstudy;   // scores as TWIN, pays as itself
        w.jokers[2] = jr::kCollector;    // fires on every spin there has been
        jr::world_tick(w, press_a());    // pull
        for (int t = 0; t < 900 && w.state != jr::kCount; t++) play(w, 1);
        if (w.state != jr::kCount) continue;

        for (int i = 0; i < w.tally_len; i++) {
            const jr::TallyEntry& e = w.tally[i];
            if (!e.joker) {
                check(e.slot == jr::k_no_slot,
                      "an entry that is not a joker names no slot");
                continue;
            }
            entries++;
            if (e.slot >= w.joker_count) {
                check(false, "a joker entry names a slot this run holds");
                continue;
            }
            slot_seen[e.slot] = true;
            // The entry carries the name of the joker in the slot it names.
            // That is the whole invariant, and it is checked against the
            // WORLD's row rather than against the order score() happened to
            // walk in.
            check(std::strcmp(jr::joker_name(w.jokers[e.slot]), e.what) == 0,
                  "and the joker in that slot is the one the entry names");
        }
    }
    check(entries > 0, "jokers fired across those spins");
    check(slot_seen[0] && slot_seen[1] && slot_seen[2],
          "and all three slots were exercised, the understudy included");

    // The count lingers on a joker. Stepped rather than asserted against a
    // literal, because the duration lives in the rules and the renderer reads
    // it from there: a check with its own copy would pass while the two
    // disagreed.
    jr::World w = started(101u);
    w.joker_count = 1;
    w.jokers[0] = jr::kCollector;
    jr::world_tick(w, press_a());
    for (int t = 0; t < 900 && w.state != jr::kCount; t++) play(w, 1);
    int joker_hold = 0, line_hold = 0;
    for (int guard = 0; guard < 2000 && w.state == jr::kCount; guard++) {
        play(w, 1);
        if (w.tally_step == 0) continue;
        const jr::TallyEntry& e = w.tally[w.tally_step - 1];
        if (e.joker) joker_hold = jr::tally_hold(w);
        else line_hold = jr::tally_hold(w);
    }
    check(joker_hold > line_hold && line_hold > 0,
          "a joker is held longer than a payline is");
}

/* The speed dial answers the axis it is drawn along.
 *
 * Three boxes in a row, SLOW FAIR WILD, and it only read up and down: the
 * shop's bug in a mirror. Both axes work, and both are checked, because making
 * left and right work by taking up and down away would be the same complaint
 * pointing the other way.
 */
void test_the_dial_turns_on_both_axes() {
    for (int axis = 0; axis < 2; axis++) {
        jr::Buttons faster{};
        jr::Buttons slower{};
        if (axis == 0) { faster.up = true; slower.down = true; }
        else { faster.right = true; slower.left = true; }
        faster.any = true;
        slower.any = true;

        // Between spins.
        jr::World w = started(41u);
        w.speed = jr::kSlow;
        for (int i = 1; i < jr::k_speeds; i++) {
            jr::world_tick(w, faster);
            check(w.speed == i, "a press moves the dial one notch up");
        }
        jr::world_tick(w, faster);
        check(w.speed == jr::k_speeds - 1, "and stops at the top");
        for (int i = jr::k_speeds - 2; i >= 0; i--) {
            jr::world_tick(w, slower);
            check(w.speed == i, "and one notch down");
        }
        jr::world_tick(w, slower);
        check(w.speed == 0, "and stops at the bottom");
        check(w.state == jr::kIdle, "without pulling anything");

        // And MID SPIN, which is the whole point of the dial: the risk is a
        // decision you can still change while the reels are turning.
        jr::World spin = started(41u);
        spin.speed = jr::kSlow;
        jr::world_tick(spin, press_a());
        check(spin.state == jr::kSpin, "the reels are turning");
        jr::world_tick(spin, faster);
        check(spin.speed == jr::kFair, "and the dial still moves");
    }
}

/* Rerolling the shelf, and the price climbing.
 *
 * The price is what makes it a decision, so it is the thing worth pinning: a
 * flat one turns the shop into a slot machine inside a slot machine.
 */
void test_the_reroll_costs_more_every_time() {
    jr::World w = started(53u);
    w.banked = w.target;
    w.state = jr::kCleared;
    jr::world_tick(w, press_a());
    check(w.state == jr::kShop, "the shop opened");
    check(jr::reroll_cost(w) == jr::k_reroll_base, "at the base price");

    w.gold = 200;
    uint8_t before[jr::k_shop_items];
    for (int i = 0; i < w.shop_len; i++) before[i] = w.shop[i].which;

    int last = 0;
    bool changed = false;
    for (int n = 0; n < 4; n++) {
        const int cost = jr::reroll_cost(w);
        check(cost > last, "each reroll is dearer than the one before");
        last = cost;
        const uint16_t gold = w.gold;
        w.shop_sel = jr::shop_reroll_index(w);
        jr::world_tick(w, press_a());
        check(w.gold == gold - cost, "and it charges what it says");
        check(w.shop_len == jr::k_shop_items, "and the shelf is refilled");
        check(w.state == jr::kShop, "and stays in the shop");
        for (int i = 0; i < w.shop_len; i++) {
            if (w.shop[i].which != before[i]) changed = true;
        }
    }
    check(changed, "and the shelf actually changes");

    // Too poor is a no-op rather than a debt.
    w.gold = 0;
    const uint8_t len = w.shop_len;
    w.shop_sel = jr::shop_reroll_index(w);
    jr::world_tick(w, press_a());
    check(w.gold == 0 && w.shop_len == len,
          "a reroll you cannot afford does nothing");

    // And the price starts over with the next shelf, or the feature stops
    // existing around ante three.
    jr::world_open_shop(w);
    check(jr::reroll_cost(w) == jr::k_reroll_base,
          "a new shop starts at the base price again");
}

/* Consumables: bought, held, spent, gone.
 *
 * The two shapes are checked separately because they fail differently. One
 * that fires at once has to change the world on the press. One that loads the
 * next spin has to change NOTHING until the count reaches it, or the score
 * moves with no line to account for it.
 */
void test_a_consumable_is_spent_once() {
    jr::Buttons use{};
    use.x = true;
    use.any = true;

    // The immediate kind.
    jr::World w = started(67u);
    w.item_count = 1;
    w.items[0] = jr::kLuckyCoin;
    const uint16_t gold = w.gold;
    jr::world_tick(w, use);
    check(w.gold == gold + 6, "LUCKY COIN pays on the press");
    check(w.item_count == 0, "and is gone");
    jr::world_tick(w, use);
    check(w.gold == gold + 6, "and pressing again spends nothing");

    // The loaded kind, which must not touch the score until the count does.
    jr::World load = started(67u);
    load.item_count = 1;
    load.items[0] = jr::kHotStreak;
    jr::world_tick(load, use);
    check(load.item_count == 0, "HOT STREAK leaves the row");
    check(load.chips == 0 && load.mult == 0,
          "and changes nothing yet, because the count is the explanation");

    jr::world_tick(load, press_a());        // pull
    for (int t = 0; t < 900 && load.state != jr::kCount; t++) play(load, 1);
    check(load.state == jr::kCount, "the spin landed");
    bool found = false;
    for (int i = 0; i < load.tally_len; i++) {
        if (std::strcmp(load.tally[i].what, jr::item_name(jr::kHotStreak)) != 0) {
            continue;
        }
        found = true;
        check(load.tally[i].mult == jr::item_mult(jr::kHotStreak),
              "and its line pays what the table says");
    }
    check(found, "and it has a line of its own in the count");

    // One spin only: the next one has no line for it.
    for (int t = 0; t < 2000 && load.state == jr::kCount; t++) play(load, 1);
    if (load.state == jr::kIdle) {
        jr::world_tick(load, press_a());
        for (int t = 0; t < 900 && load.state != jr::kCount; t++) play(load, 1);
        bool again = false;
        for (int i = 0; i < load.tally_len; i++) {
            if (std::strcmp(load.tally[i].what,
                            jr::item_name(jr::kHotStreak)) == 0) {
                again = true;
            }
        }
        check(!again, "and it does not pay a second time");
    }

    // Y picks between them, and only matters when there are two.
    jr::Buttons pick{};
    pick.y = true;
    pick.any = true;
    jr::World two = started(67u);
    two.item_count = 2;
    two.items[0] = jr::kLuckyCoin;
    two.items[1] = jr::kSpareSpin;
    two.item_sel = 0;
    jr::world_tick(two, pick);
    check(two.item_sel == 1, "Y moves the pick");
    const uint8_t spins = two.spins;
    jr::world_tick(two, use);
    check(two.spins == spins + 1, "and X spends the one picked, not the first");
    check(two.item_count == 1 && two.items[0] == jr::kLuckyCoin,
          "and the row closes up around the one that is gone");

    // Buying one is what puts it there in the first place.
    jr::World shop = started(67u);
    shop.banked = shop.target;
    shop.state = jr::kCleared;
    jr::world_tick(shop, press_a());
    shop.gold = 60;
    int card = -1;
    for (int i = 0; i < shop.shop_len; i++) {
        if (shop.shop[i].kind == jr::kShopItem) card = i;
    }
    check(card >= 0, "the shelf offers a consumable");
    if (card >= 0) {
        shop.shop_sel = static_cast<uint8_t>(card);
        const uint8_t which = shop.shop[card].which;
        jr::world_tick(shop, press_a());
        check(shop.item_count == 1 && shop.items[0] == which,
              "and buying it puts that one in the row");
        check(shop.shop[card].sold, "and marks the card sold");
    }
}

/* The difficulty curve, measured rather than hoped for.
 *
 * Two autopilots: one that pulls and never touches anything, and one that
 * plays the dial at WILD and stops every reel. The floor has to lose and the
 * skilled line has to mostly win, and the gap between them is the game.
 *
 * This is here because the numbers have already been wrong twice, both times
 * invisibly. Giving every payline its own mult and adding them up multiplied
 * everything by everything, and 300 hands off runs cleared all eight antes
 * without a button being pressed. Then the targets, written for three reels
 * and one line, survived the change to five of each and did it again.
 */
void run_autopilot(uint32_t seed, bool skilled, int& reached, bool& won) {
    jr::World w;
    jr::world_init(w, seed);
    for (int i = 0; i < 2 + jr::k_learn_pages && w.state != jr::kIdle; i++) {
        jr::world_tick(w, press_a());
    }
    for (int t = 0; t < 60000; t++) {
        jr::Buttons b{};
        const bool act = w.state == jr::kIdle || w.state == jr::kCleared ||
                         w.state == jr::kShop ||
                         (skilled && w.state == jr::kSpin);
        if (act) b = press_a();
        // Straight past the shelf. Asked for by name rather than computed as
        // shop_len, which is what this said and which is now the REROLL row:
        // the autopilot rerolled the shop forever and every run died on ante
        // one, which is the whole difficulty measurement quietly reading a
        // game nobody was playing.
        if (w.state == jr::kShop) w.shop_sel = jr::shop_next_index(w);
        if (skilled) w.speed = jr::kWild;
        jr::world_tick(w, b);
        if (w.state == jr::kOver || w.state == jr::kWin) break;
    }
    won = w.state == jr::kWin;
    reached = w.ante;
}

void test_the_floor_loses_and_skill_wins() {
    int floor_wins = 0, floor_ante = 0;
    int skilled_wins = 0;
    const int runs = 120;
    for (int i = 0; i < runs; i++) {
        const uint32_t seed = static_cast<uint32_t>(i + 1) * 7919u;
        int reached = 0;
        bool won = false;
        run_autopilot(seed, false, reached, won);
        if (won) floor_wins++;
        floor_ante += reached;

        run_autopilot(seed, true, reached, won);
        if (won) skilled_wins++;
    }
    const int mean_ante = floor_ante / runs;
    std::printf("  floor: 0 wins wanted, %d seen, mean ante %d\n",
                floor_wins, mean_ante);
    std::printf("  skilled: %d of %d runs won\n", skilled_wins, runs);

    // Pulling and touching nothing must not finish the game. If it does, the
    // dial, the jokers and the whole shop are decoration.
    check(floor_wins == 0, "the hands off floor never wins");
    // But it has to get somewhere, or the opening antes are a wall.
    check(mean_ante >= 3, "and it is not a wall from the first ante");
    check(mean_ante <= 7, "and it does not walk to the last one either");
    // Playing the dial has to be worth it, and not a formality.
    check(skilled_wins > runs / 2, "playing the dial usually wins");
    check(skilled_wins < runs, "and is not a guarantee");
}

void test_the_run_is_deterministic() {
    jr::World a, b;
    jr::world_init(a, 12345u);
    jr::world_init(b, 12345u);
    for (int t = 0; t < 600; t++) {
        const jr::Buttons btn = (t % 37 == 0) ? press_a() : none();
        jr::world_tick(a, btn);
        jr::world_tick(b, btn);
    }
    check(a.banked == b.banked && a.gold == b.gold && a.state == b.state,
          "the same seed and the same buttons give the same run");
}

void test_the_budget() {
    // A promise about RAM, checked by the compiler rather than by a comment.
    // Five reels rather than three cost 168 bytes: the strips are the bulk of
    // it, at 24 symbols a reel.
    check(sizeof(jr::World) <= 768, "the whole run is under three quarters of a KB");
    std::printf("  sizeof(jr::World) = %zu bytes\n", sizeof(jr::World));
}

}  // namespace

int main() {
    test_the_scored_facet_is_the_one_being_looked_at();
    test_a_facet_only_changes_symbol_out_of_sight();
    test_a_hands_off_spin_ends();
    test_stopping_pays_and_costs();
    test_hands();
    test_a_swap_changes_what_a_drum_can_land_on();
    test_a_strip_longer_than_the_drum_still_reaches_every_symbol();
    test_a_run_is_finite_and_a_shop_is_a_choice();
    test_the_shop_moves_on_both_axes();
    test_the_instructions_come_back_to_where_they_opened();
    test_a_joker_entry_names_its_slot();
    test_the_dial_turns_on_both_axes();
    test_the_reroll_costs_more_every_time();
    test_a_consumable_is_spent_once();
    test_the_floor_loses_and_skill_wins();
    test_the_run_is_deterministic();
    test_the_budget();

    std::printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
