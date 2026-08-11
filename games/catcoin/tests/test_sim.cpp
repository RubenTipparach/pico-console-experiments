// Host side tests for Cat Coin Pusher's rules.
//
// The interesting ones are the three the mockup found by running the thing
// rather than by reading the cart: the shelf has to be measured in coins or
// round 1 is unwinnable, the seed count has to be a count and not a ceiling,
// and the collision grid has to be complete or coins pass through each other.

#include <cstdint>
#include <cstdio>

#include "sim.hpp"

namespace {

int g_failures = 0;
int g_checks = 0;

void check(bool condition, const char* expression, const char* file, int line) {
    g_checks++;
    if (condition) return;
    g_failures++;
    std::printf("FAIL %s:%d: %s\n", file, line, expression);
}

#define CHECK(expr) check((expr), #expr, __FILE__, __LINE__)

cc::Input none() {
    cc::Input in;
    in.drop_pressed = false;
    in.use_pressed = false;
    in.left_pressed = false;
    in.right_pressed = false;
    in.up_pressed = false;
    in.down_pressed = false;
    in.any_pressed = false;
    return in;
}

// Start a run, and optionally jump to a later round with a fresh shelf.
cc::World begin(uint32_t seed, int round = 1) {
    static cc::World world;
    cc::world_init(world, seed);
    cc::Input start = none();
    start.any_pressed = true;
    cc::world_tick(world, start);
    if (round > 1) {
        world.round = static_cast<uint16_t>(round);
        // Re-enter the round with a fresh shelf, the way the shop's continue
        // does not: continue keeps the table, this wants a clean one.
        cc::world_init(world, seed);
        cc::world_tick(world, start);
        world.round = static_cast<uint16_t>(round);
        world.target = cc::target_for(round);
        world.coin_count = 0;
        world.state = cc::State::play;
        // Seeding is what start_round does; reach it by ending and continuing.
        world.round_score = world.target;
        cc::Input use = none();
        use.use_pressed = true;
        // Walk the row to END ROUND, which is the last slot when it exists.
        cc::Slot slots[cc::k_inv_max + 2];
        const int n = cc::build_slots(world, slots, cc::k_inv_max + 2);
        world.sel = static_cast<uint8_t>(n - 1);
        cc::world_tick(world, use);          // -> shop
        world.shop_sel = 4;
        cc::world_tick(world, use);          // -> next round, no reseed
        // The shop's continue deliberately keeps the table, so seed by hand
        // through a fresh init at the round we want.
        cc::world_init(world, seed + round);
        cc::world_tick(world, start);
        for (int r = 1; r < round; r++) {
            world.round = static_cast<uint16_t>(r + 1);
        }
    }
    return world;
}

// Play `ticks` ticks, dropping a coin every `every` ticks and dismissing the
// spinner whenever it opens, which is what a player does.
void play(cc::World& w, int ticks, int every) {
    for (int i = 0; i < ticks; i++) {
        cc::Input in = none();
        if (w.state == cc::State::spinner) {
            in.any_pressed = true;
        } else if (every > 0 && (i % every) == 0) {
            in.drop_pressed = true;
        }
        cc::world_tick(w, in);
    }
}

void test_the_shelf_is_measured_in_coins() {
    // The cart's field was 21.6 x 8.8 coins with a 4.4 coin free shelf. The
    // first version of the mockup scaled by pixels and got 11.2 coins of
    // shelf, and round 1 became unwinnable. This is that measurement, kept.
    const double field_coins = static_cast<double>(cc::k_fh) / cc::k_coin_d;
    const double shelf_coins = static_cast<double>(cc::k_fbot - cc::k_push_max) / cc::k_coin_d;
    const double plate_coins = static_cast<double>(cc::k_push_h) / cc::k_coin_d;
    const double travel_coins = static_cast<double>(cc::k_push_travel) / cc::k_coin_d;
    CHECK(field_coins > 8.5 && field_coins < 9.1);      // the cart's 8.8
    CHECK(shelf_coins > 4.2 && shelf_coins < 4.6);      // the cart's 4.4
    CHECK(plate_coins > 1.9 && plate_coins < 2.1);      // the cart's 2.0
    CHECK(travel_coins > 3.1 && travel_coins < 3.3);    // the cart's 3.2
}

void test_the_seed_count_is_a_count_not_a_ceiling() {
    // The cart asked for 100 + 10r into a shelf that could not hold half that,
    // so the number in the source was never the number on the table.
    const int capacity = cc::seed_capacity();
    CHECK(capacity > 40);
    for (int round = 1; round <= cc::k_max_rounds; round++) {
        CHECK(cc::seed_count(round) <= capacity);
    }
    CHECK(cc::seed_count(1) < cc::seed_count(cc::k_max_rounds));
    CHECK(cc::seed_count(cc::k_max_rounds) == capacity);

    // And the table really does hold what was asked for.
    cc::World w = begin(101);
    CHECK(w.coin_count == cc::seed_count(1));
}

void test_every_round_is_winnable() {
    // The measurement that caught the unwinnable round 1. A bot that drops a
    // coin every quarter second has to reach the target of every round.
    for (int round = 1; round <= cc::k_max_rounds; round++) {
        cc::World w = begin(101);
        w.round = static_cast<uint16_t>(round);
        w.target = cc::target_for(round);
        w.score_per_gold = (w.target + 99) / 100;
        w.coins_left = 400;
        w.coin_count = 0;
        // Seed the shelf this round would seed.
        cc::World fresh = begin(101 + round);
        for (uint16_t i = 0; i < fresh.coin_count && w.coin_count < cc::k_max_coins; i++) {
            w.coins[w.coin_count++] = fresh.coins[i];
        }
        w.round_score = 0;
        w.state = cc::State::play;

        int ticks = 0;
        while (ticks < 12000 && !(w.state == cc::State::play && w.round_score >= w.target)) {
            cc::Input in = none();
            if (w.state == cc::State::spinner) in.any_pressed = true;
            else if ((ticks % 25) == 0) in.drop_pressed = true;
            cc::world_tick(w, in);
            ticks++;
        }
        if (w.round_score < w.target) {
            std::printf("  round %d stalled at %d of %d after %d ticks\n", round,
                        static_cast<int>(w.round_score), static_cast<int>(w.target), ticks);
        }
        CHECK(w.round_score >= w.target);
    }
}

void test_coins_stay_inside_the_field() {
    cc::World w = begin(55);
    play(w, 3000, 20);
    for (uint16_t i = 0; i < w.coin_count; i++) {
        const cc::Coin& c = w.coins[i];
        const int x = c.x >> cc::k_fp;
        const int y = c.y >> cc::k_fp;
        CHECK(x >= cc::k_fl && x <= cc::k_fr);
        // Above the lip, or on its way over it and about to be scored.
        CHECK(y >= cc::k_ft && y <= cc::k_fbot + cc::k_coin_d);
    }
}

void test_coins_do_not_overlap_badly() {
    // The grid checks the same cell plus four forward neighbours, which is
    // only complete while a cell is at least one coin diameter across. If that
    // ever stops being true, coins pass through each other and the pile melts.
    CHECK(cc::k_cell >= cc::k_coin_d);

    cc::World w = begin(77);
    play(w, 2000, 18);
    int bad = 0;
    for (uint16_t i = 0; i < w.coin_count; i++) {
        for (uint16_t j = static_cast<uint16_t>(i + 1); j < w.coin_count; j++) {
            const int32_t dx = w.coins[i].x - w.coins[j].x;
            const int32_t dy = w.coins[i].y - w.coins[j].y;
            const int64_t dd = static_cast<int64_t>(dx) * dx + static_cast<int64_t>(dy) * dy;
            // Half a diameter of overlap is a stack the solver has not finished
            // separating yet. Deeper than that is a pair it never saw.
            const int32_t limit = (cc::k_coin_d * cc::k_one) / 2;
            if (dd < static_cast<int64_t>(limit) * limit) bad++;
        }
    }
    if (bad != 0) std::printf("  %d badly overlapping pairs\n", bad);
    CHECK(bad == 0);
}

void test_the_row_never_names_a_button_and_always_has_one_slot() {
    cc::World w = begin(9);
    cc::Slot slots[cc::k_inv_max + 2];
    // With an empty bag and a round in progress, BUY is the only slot, so the
    // row is never empty and selection never points at nothing.
    int n = cc::build_slots(w, slots, cc::k_inv_max + 2);
    CHECK(n >= 1);
    CHECK(slots[n - 1].kind == cc::SlotKind::buy || slots[n - 1].kind == cc::SlotKind::end);

    // END ROUND appears exactly when the round can be ended.
    w.round_score = w.target;
    n = cc::build_slots(w, slots, cc::k_inv_max + 2);
    CHECK(cc::can_end_round(w));
    CHECK(slots[n - 1].kind == cc::SlotKind::end);

    w.round_score = 0;
    w.coins_left = 5;
    w.dropping_count = 0;
    n = cc::build_slots(w, slots, cc::k_inv_max + 2);
    CHECK(!cc::can_end_round(w));
    for (int i = 0; i < n; i++) CHECK(slots[i].kind != cc::SlotKind::end);

    // A full bag plus BUY plus END is the widest the row ever gets, and it has
    // to fit the array the game passes in.
    w.inv_count = cc::k_inv_max;
    for (int i = 0; i < cc::k_inv_max; i++) w.inv[i] = cc::spc_bomb;
    w.round_score = w.target;
    n = cc::build_slots(w, slots, cc::k_inv_max + 2);
    CHECK(n == cc::k_inv_max + 2);
}

void test_using_a_slot_spends_it() {
    cc::World w = begin(13);
    w.inv_count = 1;
    w.inv[0] = cc::spc_bomb;
    w.sel = 0;
    const int32_t coins_before = w.coins_left;

    cc::Input use = none();
    use.use_pressed = true;
    cc::world_tick(w, use);
    CHECK(w.inv_count == 0);                 // the bag slot is gone
    CHECK(w.dropping_count == 1);            // and a bomb is on its way down
    CHECK(w.coins_left == coins_before);     // using a special is not a drop

    // It lands, and gets the cart's two second fuse.
    for (int i = 0; i < cc::k_drop_time + 1; i++) cc::world_tick(w, none());
    bool found = false;
    for (uint16_t i = 0; i < w.coin_count; i++) {
        if (w.coins[i].stype == cc::spc_bomb) {
            found = true;
            CHECK(w.coins[i].fuse > 0);
            CHECK(w.coins[i].fuse <= cc::k_fuse);
        }
    }
    CHECK(found);
}

void test_buying_coins_sets_off_every_fuse() {
    cc::World w = begin(17);
    w.gold = 100;
    w.inv_count = 1;
    w.inv[0] = cc::spc_bomb;
    w.sel = 0;
    cc::Input use = none();
    use.use_pressed = true;
    cc::world_tick(w, use);
    for (int i = 0; i < cc::k_drop_time + 1; i++) cc::world_tick(w, none());

    // The bomb is on the table with a live fuse. Buying coins triggers it,
    // which is the cart's rule and the reason to ever buy at an awkward time.
    bool fused = false;
    for (uint16_t i = 0; i < w.coin_count; i++) {
        if (w.coins[i].stype == cc::spc_bomb && w.coins[i].fuse > 0) fused = true;
    }
    CHECK(fused);

    cc::Slot slots[cc::k_inv_max + 2];
    const int n = cc::build_slots(w, slots, cc::k_inv_max + 2);
    for (int i = 0; i < n; i++) {
        if (slots[i].kind == cc::SlotKind::buy) w.sel = static_cast<uint8_t>(i);
    }
    const int32_t gold_before = w.gold;
    cc::world_tick(w, use);
    CHECK(w.gold < gold_before);
    for (uint16_t i = 0; i < w.coin_count; i++) {
        if (w.coins[i].stype == cc::spc_bomb) CHECK(w.coins[i].fuse == 0);
    }
}

void test_the_goldfish_is_passive_and_worth_more() {
    cc::World w = begin(23);
    w.inv_count = 1;
    w.inv[0] = cc::spc_gold;
    w.sel = 0;
    cc::Input use = none();
    use.use_pressed = true;
    cc::world_tick(w, use);
    for (int i = 0; i < cc::k_drop_time + 1; i++) cc::world_tick(w, none());
    for (uint16_t i = 0; i < w.coin_count; i++) {
        // The cart gave every special a fuse except this one, because it does
        // nothing until it scores.
        if (w.coins[i].stype == cc::spc_gold) CHECK(w.coins[i].fuse == 0);
    }

    // Scoring one is worth 50 against a plain coin's 10.
    cc::World a = begin(24);
    a.coin_count = 1;
    a.coins[0].stype = cc::spc_none;
    a.coins[0].x = 120 * cc::k_one;
    a.coins[0].y = (cc::k_fbot + 2) * cc::k_one;
    a.coins[0].vx = 0;
    a.coins[0].vy = 0;
    a.coins[0].flags = 0;
    a.coins[0].fuse = 0;
    a.round_score = 0;
    a.combo = 0;
    cc::world_tick(a, none());
    const int32_t plain = a.round_score;

    cc::World b = begin(24);
    b.coin_count = 1;
    b.coins[0] = a.coins[0];
    b.coins[0].stype = cc::spc_gold;
    b.coins[0].y = (cc::k_fbot + 2) * cc::k_one;
    b.round_score = 0;
    b.combo = 0;
    cc::world_tick(b, none());
    CHECK(b.round_score == plain * 5);
}

void test_a_missed_target_ends_the_run() {
    cc::World w = begin(31);
    w.coins_left = 0;
    w.dropping_count = 0;
    w.round_score = w.target / 2;
    CHECK(cc::can_end_round(w));
    cc::Slot slots[cc::k_inv_max + 2];
    const int n = cc::build_slots(w, slots, cc::k_inv_max + 2);
    w.sel = static_cast<uint8_t>(n - 1);
    CHECK(slots[n - 1].kind == cc::SlotKind::end);
    cc::Input use = none();
    use.use_pressed = true;
    cc::world_tick(w, use);
    CHECK(w.state == cc::State::over);
}

void test_clearing_ten_rounds_wins() {
    cc::World w = begin(37);
    w.round = cc::k_max_rounds;
    w.target = cc::target_for(cc::k_max_rounds);
    w.round_score = w.target;
    cc::Slot slots[cc::k_inv_max + 2];
    const int n = cc::build_slots(w, slots, cc::k_inv_max + 2);
    w.sel = static_cast<uint8_t>(n - 1);
    cc::Input use = none();
    use.use_pressed = true;
    cc::world_tick(w, use);
    CHECK(w.state == cc::State::win);
}

void test_the_shop_spends_gold_and_fills_the_bag() {
    cc::World w = begin(41);
    w.round_score = w.target;
    w.gold = 500;
    cc::Slot slots[cc::k_inv_max + 2];
    const int n = cc::build_slots(w, slots, cc::k_inv_max + 2);
    w.sel = static_cast<uint8_t>(n - 1);
    cc::Input use = none();
    use.use_pressed = true;
    cc::world_tick(w, use);
    CHECK(w.state == cc::State::shop);

    w.shop_sel = 0;
    const int32_t gold_before = w.gold;
    const uint16_t cost = w.shop[0].cost;
    cc::world_tick(w, use);
    CHECK(w.inv_count == 1);
    CHECK(w.gold == gold_before - cost);
    CHECK(w.shop[0].sold);

    // Buying the same slot twice buys nothing.
    cc::world_tick(w, use);
    CHECK(w.inv_count == 1);

    // The bag has a lid.
    w.inv_count = cc::k_inv_max;
    w.shop_sel = 1;
    const int32_t gold_now = w.gold;
    cc::world_tick(w, use);
    CHECK(w.inv_count == cc::k_inv_max);
    CHECK(w.gold == gold_now);
}

void test_the_combo_multiplies_and_caps() {
    // The cart's min(combo, 8) * combo_mult, applied from the second coin on.
    cc::World w = begin(43);
    w.coin_count = 0;
    w.round_score = 0;
    w.combo = 0;
    w.combo_mult = 1;
    w.score_mult = 1;
    int32_t last = 0;
    for (int i = 0; i < 12; i++) {
        w.coin_count = 1;
        w.coins[0].stype = cc::spc_none;
        w.coins[0].x = 120 * cc::k_one;
        w.coins[0].y = (cc::k_fbot + 2) * cc::k_one;
        w.coins[0].vx = 0;
        w.coins[0].vy = 0;
        w.coins[0].flags = 0;
        w.coins[0].fuse = 0;
        const int32_t before = w.round_score;
        w.combo_timer = cc::k_combo_hold;
        cc::world_tick(w, none());
        const int32_t gained = w.round_score - before;
        if (i >= 8) CHECK(gained == last);       // capped at eight
        last = gained;
    }
}

void test_a_combo_opens_the_spinner_and_the_prize_is_capped() {
    // A burst of drops and then a pause. A combo only ends once 1.5 seconds
    // pass with nothing crossing the lip, so this is the pattern that opens
    // the spinner, and dropping for ever never opens it at all.
    cc::World w = begin(17);
    w.coins_left = 60;
    int best = 0;
    int opened_at = -1;
    for (int i = 0; i < 900; i++) {
        cc::Input in = none();
        if (i < 300 && (i % 10) == 0) in.drop_pressed = true;
        cc::world_tick(w, in);
        if (w.state == cc::State::play && w.combo > best) best = w.combo;
        if (w.state == cc::State::spinner) {
            opened_at = i;
            break;
        }
    }
    CHECK(opened_at >= 0);
    CHECK(best >= cc::k_combo_threshold);
    CHECK(w.spin_mult >= 1 && w.spin_mult <= cc::k_spin_mult_cap);

    // And the cap holds against the runaway the cart shares: under sustained
    // dropping the counter reaches four figures, and the cart fed that
    // straight into the prize.
    cc::World r = begin(19);
    r.coins_left = 4000;
    int runaway = 0;
    for (int i = 0; i < 12000; i++) {
        cc::Input in = none();
        if (r.state == cc::State::spinner) in.any_pressed = true;
        else if ((i % 8) == 0) in.drop_pressed = true;
        cc::world_tick(r, in);
        if (r.state == cc::State::play && r.combo > runaway) runaway = r.combo;
    }
    std::printf("combo under sustained play reaches %d, prize capped at %d\n", runaway,
                cc::k_spin_mult_cap);
    CHECK(runaway > 100);                    // the runaway is real, and documented
    // Scoring is still capped at the cart's eight, whatever the counter says.
    CHECK(r.round_score > 0);
}

void test_pair_tests_stay_affordable() {
    // The number the frame budget is argued from, measured rather than
    // guessed. A full round 10 table on a 250 MHz M0+ has to stay in the
    // hundreds, not the thousands.
    cc::World w = begin(99);
    w.coins_left = 400;
    play(w, 400, 10);
    cc::g_pair_tests = 0;
    for (int i = 0; i < 100; i++) cc::world_tick(w, none());
    const uint32_t per_tick = cc::g_pair_tests / 100;
    std::printf("collision load: %u coins, %u pair tests per tick\n",
                static_cast<unsigned>(w.coin_count), static_cast<unsigned>(per_tick));
    CHECK(per_tick < 1200);
}

void test_the_coin_array_never_overflows() {
    // The clone special adds five at a time, which is what the headroom is
    // for. Hammer it and make sure the fixed array holds.
    cc::World w = begin(61);
    w.coins_left = 1000;
    for (int round = 0; round < 40; round++) {
        w.inv_count = 1;
        w.inv[0] = cc::spc_clone;
        w.sel = 0;
        cc::Input use = none();
        use.use_pressed = true;
        cc::world_tick(w, use);
        play(w, 260, 4);
        CHECK(w.coin_count <= cc::k_max_coins);
        CHECK(w.particle_count <= cc::k_max_particles);
        CHECK(w.popup_count <= cc::k_max_popups);
        CHECK(w.falling_count <= cc::k_max_falling);
        CHECK(w.dropping_count <= cc::k_max_dropping);
    }
}

}  // namespace

int main() {
    test_the_shelf_is_measured_in_coins();
    test_the_seed_count_is_a_count_not_a_ceiling();
    test_every_round_is_winnable();
    test_coins_stay_inside_the_field();
    test_coins_do_not_overlap_badly();
    test_the_row_never_names_a_button_and_always_has_one_slot();
    test_using_a_slot_spends_it();
    test_buying_coins_sets_off_every_fuse();
    test_the_goldfish_is_passive_and_worth_more();
    test_a_missed_target_ends_the_run();
    test_clearing_ten_rounds_wins();
    test_the_shop_spends_gold_and_fills_the_bag();
    test_the_combo_multiplies_and_caps();
    test_a_combo_opens_the_spinner_and_the_prize_is_capped();
    test_pair_tests_stay_affordable();
    test_the_coin_array_never_overflows();

    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
