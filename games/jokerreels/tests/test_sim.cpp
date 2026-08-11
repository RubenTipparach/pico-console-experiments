// What Joker Reels' rules promise.
//
// The mockup this game was designed in found four bugs by being run and looked
// at, and three of them were in this half. They are all checks here now, and
// each one is written against something OTHER than the code it is checking,
// because a check that asks the code whether it agrees with itself cannot
// fail.

#include <cmath>
#include <cstdio>

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
    check(jr::target_for_ante(1) == 300, "the first ante wants 300");
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
    test_the_run_is_deterministic();
    test_the_budget();

    std::printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
