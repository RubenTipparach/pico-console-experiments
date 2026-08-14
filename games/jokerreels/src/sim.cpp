#include "sim.hpp"

namespace jr {
namespace {

struct SymbolDef { const char* name; int16_t chips; };

// Chips climb steeply on purpose: the good symbols are things you PUT on a
// drum later, which is what makes a swap feel like a purchase rather than a
// shuffle.
const SymbolDef k_symbols_table[k_symbols] = {
    {"CHERRY", 10}, {"BELL", 15}, {"PLUM", 20}, {"BAR", 25},
    {"CLOVER", 30}, {"SEVEN", 40}, {"DIAMOND", 55}, {"CROWN", 75},
};

struct HandDef { const char* name; int16_t chips; int16_t mult;
                 int16_t per_chips; int16_t per_mult; };
// Best to worst, matching the Hand enum. The gaps are wide on purpose: the
// point of opening a drum is to make a better shape reachable, and a ladder
// with even rungs gives nothing to climb toward.
const HandDef k_hands_table[k_hands] = {
    {"FIVE OF A KIND", 300, 20, 60, 6},
    {"FOUR OF A KIND", 180, 14, 45, 5},
    {"FULL HOUSE",     130, 10, 35, 4},
    {"RUN",            100,  8, 30, 3},
    {"THREE OF A KIND", 70,  6, 25, 3},
    {"TWO PAIR",        45,  4, 20, 2},
    {"PAIR",            25,  3, 12, 1},
    {"NOTHING",          5,  1,  5, 1},
};

struct SpeedDef { const char* name; int16_t rate; int8_t mult; int8_t blur; };
// Rates are angle units a tick, in the k_step_one = 256 fixed point. update()
// runs at 100 Hz, so SLOW turns a facet in about 18 ticks and WILD in about 3.
const SpeedDef k_speeds_table[k_speeds] = {
    {"SLOW", 14, 0, 0},
    {"FAIR", 35, 2, 1},
    {"WILD", 75, 5, 2},
};

struct JokerDef { const char* name; const char* text; int8_t cost; };
const JokerDef k_jokers_table[k_jokers] = {
    {"GREASER",    "+4 MULT IF ALL REELS STOP FAST", 6},
    {"TWIN",       "+30 CHIPS PER PAIR",             5},
    {"RATCHET",    "+1 MULT PER SPIN THIS ROUND",    7},
    {"BLUR",       "X2 MULT IF YOU STOPPED NOTHING", 8},
    {"COLLECTOR",  "+10 CHIPS PER DIFFERENT SYMBOL", 5},
    {"METRONOME",  "+3 MULT IF ALL STOPS MATCH",     6},
    {"SUNK COST",  "+50 CHIPS PER SPIN ALREADY USED", 6},
    {"UNDERSTUDY", "COPIES THE JOKER ON ITS LEFT",   9},
};

struct ItemDef { const char* name; const char* text; int8_t cost;
                 int16_t chips; int16_t mult; };
// Chips and mult are what the consumable loads onto the NEXT spin. Both zero
// means it does its work the moment it is used, and use_item is where that
// work is: this table is only what the shop and the panel need to say.
const ItemDef k_items_table[k_items] = {
    {"HOT STREAK",  "+4 MULT ON THE NEXT SPIN",     4,   0, 4},
    {"DOUBLE DOWN", "+150 CHIPS ON THE NEXT SPIN",  4, 150, 0},
    {"SPARE SPIN",  "ONE MORE SPIN THIS ANTE",      5,   0, 0},
    {"LUCKY COIN",  "+6 GOLD, RIGHT NOW",           3,   0, 0},
    {"POLISH",      "IMPROVES A SYMBOL ON A DRUM",  5,   0, 0},
    {"BLUEPRINT",   "LEVELS THE LAST HAND YOU MADE", 6,  0, 0},
};

// Every drum stops itself eventually, in order, unless you stop it first.
// Without this a spin never ends, and the whole "leave them alone" line of
// play, which one joker pays x2 for, is unreachable.
const uint16_t k_auto_stop[k_drums] = {170, 220, 270, 320, 370};

// One row per reel, per line. Row 0 is the top, 1 is the payline, 2 is the
// bottom, and every line touches one cell of every reel.
const uint8_t k_payline_rows[k_lines][k_drums] = {
    {1, 1, 1, 1, 1},        // MIDDLE
    {0, 0, 0, 0, 0},        // TOP
    {2, 2, 2, 2, 2},        // BOTTOM
    {0, 1, 2, 1, 0},        // V
    {2, 1, 0, 1, 2},        // CARET
};
const char* const k_payline_names[k_lines] = {
    "MIDDLE", "TOP", "BOTTOM", "V", "CARET",
};

uint32_t next_random(World& w) {
    w.rng = w.rng * 1664525u + 1013904223u;
    return w.rng >> 8;
}

int random_below(World& w, int limit) {
    if (limit <= 0) return 0;
    return static_cast<int>(next_random(w) % static_cast<uint32_t>(limit));
}

// Angle reduced to one turn, always positive. The drums only ever turn one
// way, but the swap screen eases backwards, so this has to handle both.
int32_t wrap_turn(int32_t angle) {
    angle %= k_turn;
    if (angle < 0) angle += k_turn;
    return angle;
}

// How far round from the front, as a distance rather than a direction: 0 is
// dead centre front and k_turn/2 is dead centre back.
int32_t from_front(int32_t angle, int facet) {
    const int32_t a = wrap_turn(facet_mid(angle, facet));
    return a <= k_turn / 2 ? a : k_turn - a;
}

void set_facets_from_strip(World& w, int drum, int entry) {
    const int n = w.strip_len[drum];
    const int front = ((entry % k_facets) + k_facets) % k_facets;
    for (int k = 0; k < k_facets; k++) {
        w.facet[drum][(front + k) % k_facets] =
            w.strip[drum][(entry + k) % n];
    }
    w.cursor[drum] = static_cast<uint8_t>((entry + k_facets) % n);
    uint16_t mask = 0;
    for (int f = 0; f < k_facets; f++) {
        if (facet_hidden(w.angle[drum], f)) mask |= static_cast<uint16_t>(1u << f);
    }
    w.behind[drum] = mask;
}

void init_facets(World& w) {
    for (int d = 0; d < k_drums; d++) {
        const int n = w.strip_len[d];
        for (int f = 0; f < k_facets; f++) w.facet[d][f] = w.strip[d][f % n];
        w.cursor[d] = static_cast<uint8_t>(k_facets % n);
        uint16_t mask = 0;
        for (int f = 0; f < k_facets; f++) {
            if (facet_hidden(w.angle[d], f)) mask |= static_cast<uint16_t>(1u << f);
        }
        w.behind[d] = mask;
    }
}

// Called every tick a drum moves. Repaints only facets that have just gone out
// of sight, and only when the strip is longer than the drum has facets: while
// a strip is exactly twelve long, facet f simply IS strip entry f and nothing
// is ever repainted at all.
void refresh_facets(World& w, int drum) {
    const int n = w.strip_len[drum];
    if (n <= k_facets) return;
    uint16_t mask = 0;
    for (int f = 0; f < k_facets; f++) {
        const bool hidden = facet_hidden(w.angle[drum], f);
        if (hidden) {
            mask |= static_cast<uint16_t>(1u << f);
            if ((w.behind[drum] & (1u << f)) == 0) {
                w.facet[drum][f] = w.strip[drum][w.cursor[drum]];
                w.cursor[drum] = static_cast<uint8_t>((w.cursor[drum] + 1) % n);
            }
        }
    }
    w.behind[drum] = mask;
}

// Snap so a facet MIDDLE is at the front, which is the half step offset. Snap
// to a whole facet instead and the drum comes to rest showing the join between
// two symbols, straight down the middle of the reel.
void snap_drum(World& w, int drum) {
    const int32_t half = k_step_one / 2;
    int32_t a = w.angle[drum] + half;
    int32_t turns = a / k_step_one;
    if (a < 0 && a % k_step_one != 0) turns--;
    w.angle[drum] = turns * k_step_one - half;
    for (int r = 0; r < k_rows; r++) {
        w.grid[drum][r] = face_at(w, drum, facet_in_row(w, drum, r));
    }
    w.landed[drum] = w.grid[drum][1];
}

void push_tally(World& w, const char* what, int chips, int mult,
                uint8_t slot, bool joker, uint8_t line = k_no_line) {
    if (w.tally_len >= k_max_tally) return;
    TallyEntry& e = w.tally[w.tally_len++];
    e.what = what;
    e.line = line;
    e.chips = static_cast<int16_t>(chips);
    e.mult = static_cast<int16_t>(mult);
    e.slot = slot;
    e.joker = joker;
}

/* Scoring, once per payline.
 *
 * Every line that makes anything at all pays, and each one is its own tally
 * entry, so the count walks the lines and the screen draws whichever line it
 * is paying. That is what makes the grid teachable: a player sees the shape,
 * not just a number climbing.
 *
 * A line's chips are the hand's, plus the chips of the symbols that actually
 * made it. Paying for every symbol on every line would count the middle row
 * three times over, once for each line through it.
 */
void score(World& w) {
    w.tally_len = 0;
    w.tally_step = 0;
    w.chips = 0;
    w.mult = 0;
    w.hand_index = kNothing;

    /* The BEST line sets the mult. Every other paying line adds its chips.
     *
     * Giving every line its own mult and adding them up multiplies everything
     * by everything: five lines each worth 60 chips and 3 mult came out as 300
     * times 15 rather than five times 180, and the game measured 8,070 a spin
     * against an ante 8 target of 8,043. Three hundred hands off runs cleared
     * all eight antes, every time, which is not a difficulty curve.
     *
     * One mult and a growing pile of chips is also the shape the score box was
     * built for, and it keeps a second paying line worth having without making
     * it worth five times the first.
     */
    int best_hand = kNothing;
    uint8_t best_line = k_no_line;
    int line_chips[k_lines] = {0};
    for (uint8_t line = 0; line < k_lines; line++) {
        uint8_t symbols[k_drums];
        line_symbols(w, line, symbols);
        const uint8_t hand = hand_of(symbols);
        w.line_hand[line] = hand;
        if (hand == kNothing) continue;

        uint8_t groups[k_drums];
        hand_groups(symbols, groups);
        int chips = hand_chips(hand, w.hand_level[hand]);
        for (int d = 0; d < k_drums; d++) {
            if (groups[d] != k_no_group) chips += symbol_chips(symbols[d]);
        }
        line_chips[line] = chips;
        if (hand < best_hand) { best_hand = hand; best_line = line; }
    }
    w.hand_index = static_cast<uint8_t>(best_hand);

    // The best line first, carrying the mult, so the count opens on the thing
    // the player was aiming at.
    if (best_line != k_no_line) {
        push_tally(w, hand_name(static_cast<uint8_t>(best_hand)),
                   line_chips[best_line],
                   hand_mult(static_cast<uint8_t>(best_hand),
                             w.hand_level[best_hand]),
                   k_no_slot, false, best_line);
        for (uint8_t line = 0; line < k_lines; line++) {
            if (line == best_line || w.line_hand[line] == kNothing) continue;
            push_tally(w, hand_name(w.line_hand[line]), line_chips[line], 0,
                       k_no_slot, false, line);
        }
    }

    // Nothing anywhere still pays the floor, so a spin is never worth zero and
    // the count always has something to play.
    if (w.tally_len == 0) {
        push_tally(w, hand_name(kNothing),
                   hand_chips(kNothing, w.hand_level[kNothing]),
                   hand_mult(kNothing, w.hand_level[kNothing]), k_no_slot,
                   false, kMiddle);
    }

    // The speed dial pays here, once per reel you stopped yourself.
    int fast_stops = 0;
    bool stopped_any = false;
    bool all_same = true;
    int first_stop = -1;
    for (int d = 0; d < k_drums; d++) {
        const int at = w.stopped_at[d];
        if (at < 0) continue;
        stopped_any = true;
        if (first_stop < 0) first_stop = at;
        else if (at != first_stop) all_same = false;
        if (at == kWild) fast_stops++;
        if (k_speeds_table[at].mult > 0) {
            push_tally(w, k_speeds_table[at].name, 0, k_speeds_table[at].mult,
                       k_no_slot, false);
        }
    }

    // Counted over the whole grid, because that is what the player is looking
    // at, and over the paying lines, because that is what they aimed for.
    int counts[k_symbols] = {0};
    for (int d = 0; d < k_drums; d++) {
        for (int r = 0; r < k_rows; r++) counts[w.grid[d][r]]++;
    }
    int distinct = 0;
    for (int sym = 0; sym < k_symbols; sym++) if (counts[sym] > 0) distinct++;
    int paying = 0;
    for (uint8_t line = 0; line < k_lines; line++) {
        if (w.line_hand[line] != kNothing) paying++;
    }

    /* Anything a consumable loaded onto this spin, as its own line.
     *
     * Applied here rather than when the button was pressed, because the count
     * IS the explanation: a bonus that had already landed before the tally
     * started would be a number that moved with no line to account for it,
     * which is the one thing this scoring display exists to prevent.
     */
    for (int i = 0; i < w.loaded_count; i++) {
        const uint8_t which = w.loaded[i];
        push_tally(w, item_name(which), item_chips(which), item_mult(which),
                   k_no_slot, false);
    }
    w.loaded_count = 0;

    for (int j = 0; j < w.joker_count; j++) {
        uint8_t which = w.jokers[j];
        // UNDERSTUDY copies whatever is to its left, which is why it resolves
        // here rather than when it is bought.
        if (which == kUnderstudy && j > 0) which = w.jokers[j - 1];
        int chips = 0, mult = 0;
        switch (which) {
            case kGreaser:   if (fast_stops == k_drums) mult = 4; break;
            case kTwin:      chips = 30 * paying; break;
            case kRatchet:   mult = k_spins_per_round - w.spins; break;
            case kBlur:      if (!stopped_any) mult = -1; break;
            case kCollector: chips = 10 * distinct; break;
            case kMetronome: if (stopped_any && all_same) mult = 3; break;
            case kSunkCost:  chips = 50 * (k_spins_per_round - w.spins - 1); break;
            default: break;
        }
        if (chips || mult) {
            push_tally(w, k_jokers_table[w.jokers[j]].name, chips, mult,
                       static_cast<uint8_t>(j), true);
        }
    }
}

// Play the tally back one entry at a time. The render draws whatever the last
// applied entry was, which is how the count reads as a count.
bool apply_tally_step(World& w) {
    if (w.tally_step >= w.tally_len) return false;
    const TallyEntry& e = w.tally[w.tally_step++];
    // Where the numbers were before this entry. The screen counts from these
    // to the new ones across the entry's hold rather than cutting: a total
    // that jumps is a total nobody watched arrive.
    w.chips_from = w.chips;
    w.mult_from = w.mult;
    // Every entry adds, including the first. It used to be skipped because
    // score() seeded chips and mult with the base hand before the count
    // started; it does not any more, so skipping it dropped the best line and
    // with it the ONLY entry carrying a mult. Every hand scored zero.
    w.chips += e.chips;
    if (e.mult == -1) w.mult *= 2;
    else w.mult += e.mult;
    return true;
}

void finish_score(World& w) {
    w.scored = w.chips * w.mult;
    w.banked += w.scored;
    const int32_t quarter = w.target / 4 > 0 ? w.target / 4 : 1;
    int32_t earned = w.scored / quarter;
    if (earned < 1) earned = 1;
    w.gold = static_cast<uint16_t>(w.gold + earned);
    if (w.banked >= w.target) {
        w.state = kCleared;
        w.msg = "ANTE CLEAR";
    } else if (w.spins == 0) {
        w.state = kOver;
        w.msg = "OUT OF SPINS";
    } else {
        w.state = kIdle;
        w.msg = nullptr;
    }
    // Back to the dial, so the next pull is A from where the player was left.
    w.focus = kFocusDial;
    w.joker_menu = 0;
}

void start_spin(World& w) {
    if (w.spins == 0) return;
    w.spins--;
    w.state = kSpin;
    for (int d = 0; d < k_drums; d++) {
        w.spinning[d] = true;
        w.stopped_at[d] = -1;
        /* Where a reel comes to rest is a DRAW, and it was not one.
         *
         * Every drum started at the same angle, turned at the same rate, and
         * stopped itself on a fixed tick count, so a hands off spin landed on
         * the same facets in every run that has ever been played: measured
         * over 200 seeds, drum 0 stopped on facet 9 two hundred times out of
         * two hundred. The seed picked what was PAINTED on the reels and had
         * no say at all in where they stopped, so a run's spins walked the
         * same short cycle and the same few shapes kept coming up.
         *
         * A whole turn of offset, taken per spin per drum. It costs nothing to
         * see: the drums are stationary on the frame the pull is pressed and
         * turning on the next one, and a cylinder that jumped a third of a
         * revolution while starting to spin looks exactly like a cylinder
         * starting to spin.
         *
         * It does not touch the skill: stop_next snaps a drum where it IS, so
         * a reel you time yourself lands where you saw it. Only the ones you
         * leave alone are luck now, which is what leaving them alone means.
         */
        w.angle[d] += random_below(w, k_turn);
    }
    w.chips = 0;
    w.mult = 0;
    w.chips_from = 0;
    w.mult_from = 0;
    w.scored = 0;
    w.tally_len = 0;
    w.tally_step = 0;
    w.rush = 0;
    w.msg = nullptr;
    w.spin_ticks = 0;
    for (uint8_t line = 0; line < k_lines; line++) w.line_hand[line] = kNothing;
}

/* The speed dial, on BOTH axes.
 *
 * It is drawn as three boxes in a row, SLOW FAIR WILD across the panel, and
 * it only read up and down: the same shape of mistake the shop had, in a
 * mirror. A control laid out along one axis has to answer to that axis, and
 * taking the other one away to prove the point would only move the complaint.
 */
void turn_dial(World& w, const Buttons& btn) {
    if (btn.up || btn.right) {
        w.speed = static_cast<uint8_t>(w.speed + 1 < k_speeds ? w.speed + 1
                                                              : k_speeds - 1);
    }
    if (btn.down || btn.left) {
        w.speed = static_cast<uint8_t>(w.speed > 0 ? w.speed - 1 : 0);
    }
}

bool stop_next(World& w) {
    for (int d = 0; d < k_drums; d++) {
        if (!w.spinning[d]) continue;
        w.spinning[d] = false;
        w.stopped_at[d] = static_cast<int8_t>(w.speed);
        snap_drum(w, d);
        return true;
    }
    return false;
}

/* Spending a consumable.
 *
 * Two shapes, and the table says which: an entry with chips or mult LOADS the
 * next spin and is pushed onto `loaded` for score() to find, and an entry with
 * neither does its work here and now.
 *
 * A loaded one is not applied here on purpose. The count is the animation, and
 * a bonus that had already happened before the tally started would be a number
 * that changed with no line to explain it, which is the one thing this whole
 * scoring display exists to avoid.
 */
void use_item(World& w) {
    if (w.item_count == 0) return;
    if (w.item_sel >= w.item_count) w.item_sel = 0;
    const uint8_t which = w.items[w.item_sel];

    if (item_chips(which) != 0 || item_mult(which) != 0) {
        if (w.loaded_count >= k_max_items) return;
        w.loaded[w.loaded_count++] = which;
        w.msg = "LOADED";
    } else {
        switch (which) {
            case kSpareSpin:
                w.spins++;
                w.msg = "ONE MORE SPIN";
                break;
            case kLuckyCoin:
                w.gold = static_cast<uint16_t>(w.gold + 6);
                w.msg = "+6 GOLD";
                break;
            case kPolish: {
                // One entry on one drum, one tier better. It reaches the
                // deckbuilding rather than the score, which is what makes it
                // worth holding past the spin in front of you.
                const int d = random_below(w, k_drums);
                const int n = w.strip_len[d];
                const int e = random_below(w, n);
                uint8_t& face = w.strip[d][e];
                if (face + 1 < k_symbols) face++;
                set_facets_from_strip(w, d, w.cursor[d]);
                for (int r = 0; r < k_rows; r++) {
                    w.grid[d][r] = face_at(w, d, facet_in_row(w, d, r));
                }
                w.landed[d] = w.grid[d][1];
                w.msg = "A DRUM IMPROVED";
                break;
            }
            case kBlueprint:
                w.hand_level[w.hand_index % k_hands]++;
                w.msg = hand_name(w.hand_index);
                break;
            default:
                return;
        }
    }

    // Spent. The row closes up so the slots are always the ones you hold.
    for (int i = w.item_sel; i + 1 < w.item_count; i++) {
        w.items[i] = w.items[i + 1];
    }
    w.item_count--;
    if (w.item_sel >= w.item_count) {
        w.item_sel = w.item_count ? static_cast<uint8_t>(w.item_count - 1) : 0;
    }
}

/* Moving a joker, or selling it back.
 *
 * Order is not tidying: UNDERSTUDY copies whatever is on its LEFT, so sliding
 * one along the row is a real play, and it is the only way to aim that joker
 * at anything.
 */
void do_joker_act(World& w) {
    if (w.joker_count == 0) return;
    if (w.joker_sel >= w.joker_count) w.joker_sel = 0;
    const int i = w.joker_sel;
    switch (w.joker_act) {
        case kActLeft:
            if (i == 0) { w.msg = "ALREADY FIRST"; return; }
            {
                const uint8_t t = w.jokers[i - 1];
                w.jokers[i - 1] = w.jokers[i];
                w.jokers[i] = t;
            }
            w.joker_sel = static_cast<uint8_t>(i - 1);
            w.msg = "MOVED";
            return;
        case kActRight:
            if (i + 1 >= w.joker_count) { w.msg = "ALREADY LAST"; return; }
            {
                const uint8_t t = w.jokers[i + 1];
                w.jokers[i + 1] = w.jokers[i];
                w.jokers[i] = t;
            }
            w.joker_sel = static_cast<uint8_t>(i + 1);
            w.msg = "MOVED";
            return;
        default:
            w.gold = static_cast<uint16_t>(w.gold + joker_sale(w.jokers[i]));
            for (int k = i; k + 1 < w.joker_count; k++) {
                w.jokers[k] = w.jokers[k + 1];
            }
            w.joker_count--;
            if (w.joker_sel >= w.joker_count) {
                w.joker_sel = w.joker_count
                                  ? static_cast<uint8_t>(w.joker_count - 1) : 0;
            }
            w.msg = "SOLD";
            return;
    }
}

void buy(World& w) {
    if (w.shop_sel >= w.shop_len) return;
    ShopItem& item = w.shop[w.shop_sel];
    if (item.sold || w.gold < item.cost) return;
    if (item.kind == kShopJoker) {
        if (w.joker_count >= k_max_jokers) return;
        w.jokers[w.joker_count++] = item.which;
    } else if (item.kind == kShopHand) {
        w.hand_level[item.which]++;
    } else if (item.kind == kShopItem) {
        if (w.item_count >= k_max_items) return;
        w.items[w.item_count++] = item.which;
    } else {
        w.state = kSwap;
        w.swap_drum = 0;
        w.swap_face = 0;
        w.swap_to = w.strip[0][0];
    }
    w.gold = static_cast<uint16_t>(w.gold - item.cost);
    item.sold = true;
}

// The instructions, and where to put the player back afterwards.
void open_learn(World& w, uint8_t from) {
    w.state = kLearn;
    w.learn_page = 0;
    w.learn_return = from;
}

void next_ante(World& w) {
    w.ante++;
    if (w.ante > k_antes) {
        w.state = kWin;
        return;
    }
    w.target = target_for_ante(w.ante);
    w.banked = 0;
    w.spins = k_spins_per_round;
    w.state = kIdle;
    w.msg = nullptr;
}

}  // namespace

// ---------------------------------------------------------------------------

int symbol_chips(uint8_t s) { return k_symbols_table[s % k_symbols].chips; }
const char* symbol_name(uint8_t s) { return k_symbols_table[s % k_symbols].name; }
const char* hand_name(uint8_t h) { return k_hands_table[h % k_hands].name; }

int hand_chips(uint8_t h, int level) {
    const HandDef& d = k_hands_table[h % k_hands];
    return d.chips + d.per_chips * (level - 1);
}

int hand_mult(uint8_t h, int level) {
    const HandDef& d = k_hands_table[h % k_hands];
    return d.mult + d.per_mult * (level - 1);
}
int speed_rate(uint8_t s) { return k_speeds_table[s % k_speeds].rate; }
int speed_mult(uint8_t s) { return k_speeds_table[s % k_speeds].mult; }
int speed_blur(uint8_t s) { return k_speeds_table[s % k_speeds].blur; }
const char* speed_name(uint8_t s) { return k_speeds_table[s % k_speeds].name; }
const char* joker_name(uint8_t j) { return k_jokers_table[j % k_jokers].name; }
const char* joker_text(uint8_t j) { return k_jokers_table[j % k_jokers].text; }
int joker_cost(uint8_t j) { return k_jokers_table[j % k_jokers].cost; }
const char* item_name(uint8_t i) { return k_items_table[i % k_items].name; }
const char* item_text(uint8_t i) { return k_items_table[i % k_items].text; }
int item_cost(uint8_t i) { return k_items_table[i % k_items].cost; }
int item_chips(uint8_t i) { return k_items_table[i % k_items].chips; }
int item_mult(uint8_t i) { return k_items_table[i % k_items].mult; }

int joker_sale(uint8_t joker) {
    const int half = joker_cost(joker) / 2;
    return half < 2 ? 2 : half;
}

int reroll_cost(const World& w) {
    return k_reroll_base + k_reroll_step * w.rerolls;
}

uint8_t shop_reroll_index(const World& w) { return w.shop_len; }
uint8_t shop_next_index(const World& w) {
    return static_cast<uint8_t>(w.shop_len + 1);
}

/* How long the count holds on the entry showing, in ticks.
 *
 * A payline entry is a name and a number, and it is read as fast as it can be
 * read. A joker entry is the only one with anything to WATCH: its slot shakes
 * and the number it contributed pops over the side of the equation it touched.
 * At the payline's own pace that animation began and ended inside a fifth of a
 * second, which is not long enough to find something at the other end of the
 * panel from where you were looking.
 */
int tally_hold(const World& w) {
    // Long enough to read the line, watch the number climb to meet it, and
    // see which joker moved. It was 22 ticks, under a quarter of a second,
    // which is time to notice that something happened and not what.
    constexpr int k_step_ticks = 55;
    constexpr int k_joker_ticks = 70;
    int hold = k_step_ticks;
    if (w.tally_step > 0 && w.tally_step <= w.tally_len &&
        w.tally[w.tally_step - 1].joker) {
        hold = k_joker_ticks;
    }
    // A press does not SKIP: the same entries play in the same order, three
    // times as fast. Skipping outright would make the count something to get
    // rid of, and a count nobody reads may as well be a total.
    if (w.rush) hold /= 3;
    return hold < 6 ? 6 : hold;
}

int32_t facet_mid(int32_t angle, int facet) {
    return angle + facet * k_step_one + k_step_one / 2;
}

bool facet_hidden(int32_t angle, int facet) {
    // The cull happens at rather less than a quarter turn from the front, so
    // repainting at more than a third of a turn is comfortably behind it for
    // any camera this scene will have. Being late costs nothing: a facet has
    // most of a revolution out of sight after the trigger.
    return from_front(angle, facet) > (k_turn * 5) / 12;
}

int front_facet(const World& w, int drum) {
    int best = 0;
    int32_t best_d = k_turn;
    for (int f = 0; f < k_facets; f++) {
        const int32_t d = from_front(w.angle[drum], f);
        if (d < best_d) { best_d = d; best = f; }
    }
    return best;
}

uint8_t face_at(const World& w, int drum, int facet) {
    return w.facet[drum][facet % k_facets];
}

const uint8_t* payline_rows(uint8_t line) {
    return k_payline_rows[line % k_lines];
}

const char* payline_name(uint8_t line) {
    return k_payline_names[line % k_lines];
}

/* Which facet is showing in which row.
 *
 * The middle row is the front facet. Which neighbour is ABOVE it is a fact
 * about how the drum turns rather than a choice: a facet's centre sits at
 * y = -R sin(a), so a smaller angle is higher up the screen, and a smaller
 * angle is a lower facet index. Getting this backwards flips the top and
 * bottom rows, which turns the V payline into the caret and reads as the
 * diagonals being scored wrong.
 */
int facet_in_row(const World& w, int drum, int row) {
    const int front = front_facet(w, drum);
    const int offset = row - 1;          // -1 above, 0 the payline, +1 below
    return ((front + offset) % k_facets + k_facets) % k_facets;
}

void line_symbols(const World& w, uint8_t line, uint8_t out[k_drums]) {
    const uint8_t* rows = payline_rows(line);
    for (int d = 0; d < k_drums; d++) out[d] = w.grid[d][rows[d]];
}

int32_t angle_for_facet(int facet) {
    return -(facet * k_step_one + k_step_one / 2);
}

int32_t target_for_ante(int ante) {
    /* 2,500 and 1.6 times that each ante, in integers rather than a pow.
     *
     * Measured, not picked, and measured a THIRD time because the second
     * measurement was of a game nobody was playing.
     *
     * A hands off run, taking no risk on the dial and buying nothing, averages
     * 4,035 a spin, so five of them come to about 20,000 and stall at ante 6.
     * Stopping every reel at WILD averages 16,945 a spin, so five of those
     * reach about 85,000 against ante 8's 67,107: it wins most runs and not
     * all of them, which is what the dial is for.
     *
     * The 4,000 this replaces was calibrated when a hands off spin landed on
     * the same facets in every run and every drum carried the same ramp of
     * symbols. Stopping every reel the instant it started put facet 0 under
     * the payline on all five drums, and facet 0 was a CHERRY on all five, so
     * the "skilled" autopilot landed five of a kind on essentially every spin
     * and cleared 88% of runs. That was not a difficulty curve, it was an
     * exploit with a number written under it. Randomising where a reel comes
     * to rest took it away and left the skilled line winning 4% of runs, so
     * the targets come down to meet an honest game.
     */
    int32_t target = 2500;
    for (int i = 1; i < ante; i++) target = target * 8 / 5;
    return target;
}

namespace {

// Counts per symbol, and the two most common symbols. Everything a five reel
// hand needs comes out of this.
struct Tally5 {
    int count[k_symbols];
    int best;        // most common symbol
    int best_n;
    int second;      // next most common that also repeats, or -1
    int second_n;
    int distinct;
};

Tally5 tally_of(const uint8_t landed[k_drums]) {
    Tally5 t{};
    for (int s = 0; s < k_symbols; s++) t.count[s] = 0;
    for (int d = 0; d < k_drums; d++) t.count[landed[d]]++;
    t.best = -1; t.best_n = 0; t.second = -1; t.second_n = 0; t.distinct = 0;
    for (int s = 0; s < k_symbols; s++) {
        if (t.count[s] > 0) t.distinct++;
        if (t.count[s] > t.best_n) {
            t.second = t.best; t.second_n = t.best_n;
            t.best = s; t.best_n = t.count[s];
        } else if (t.count[s] > t.second_n) {
            t.second = s; t.second_n = t.count[s];
        }
    }
    return t;
}

// Five consecutive symbols in any order, which is what a run is on a reel
// whose symbols are ranked by what they are worth.
bool is_run(const Tally5& t) {
    if (t.distinct != k_drums) return false;
    int lowest = -1, highest = -1;
    for (int s = 0; s < k_symbols; s++) {
        if (t.count[s] == 0) continue;
        if (lowest < 0) lowest = s;
        highest = s;
    }
    return highest - lowest == k_drums - 1;
}

}  // namespace

uint8_t hand_of(const uint8_t landed[k_drums]) {
    const Tally5 t = tally_of(landed);
    if (t.best_n == 5) return kFive;
    if (t.best_n == 4) return kFour;
    if (t.best_n == 3 && t.second_n == 2) return kFullHouse;
    if (is_run(t)) return kRun;
    if (t.best_n == 3) return kThree;
    if (t.best_n == 2 && t.second_n == 2) return kTwoPair;
    if (t.best_n == 2) return kPair;
    return kNothing;
}

uint8_t hand_groups(const uint8_t landed[k_drums], uint8_t groups[k_drums]) {
    for (int d = 0; d < k_drums; d++) groups[d] = k_no_group;
    const Tally5 t = tally_of(landed);
    const uint8_t hand = hand_of(landed);

    if (hand == kNothing) return 0;

    // A run is one group of five, because the whole line is the hand. Every
    // other shape is one or two groups of matching symbols, and the line is
    // drawn through the reels that actually repeat.
    if (hand == kRun) {
        for (int d = 0; d < k_drums; d++) groups[d] = 0;
        return 1;
    }

    uint8_t count = 0;
    for (int d = 0; d < k_drums; d++) {
        if (t.best >= 0 && landed[d] == t.best && t.best_n >= 2) groups[d] = 0;
    }
    if (t.best_n >= 2) count = 1;
    if (t.second_n >= 2 && count < k_max_groups) {
        for (int d = 0; d < k_drums; d++) {
            if (landed[d] == t.second) groups[d] = count;
        }
        count++;
    }
    return count;
}

// The shelf itself, without touching the selection or the reroll price. Both
// opening the shop and rerolling it want this, and they want different things
// done around it.
static void stock_shop(World& w) {
    w.shop_len = 0;

    // Two jokers, a consumable, a hand to level, and a drum to open. Five
    // things: the panel holds five rows, and a shop with nothing in it that
    // you can spend on the spin in front of you is a shop about the run and
    // never about the game.
    uint8_t pool[k_jokers];
    int pool_len = 0;
    for (uint8_t j = 0; j < k_jokers; j++) {
        bool held = false;
        for (int i = 0; i < w.joker_count; i++) {
            if (w.jokers[i] == j) { held = true; break; }
        }
        if (!held) pool[pool_len++] = j;
    }
    for (int n = 0; n < 2 && pool_len > 0; n++) {
        const int k = random_below(w, pool_len);
        ShopItem& item = w.shop[w.shop_len++];
        item.kind = kShopJoker;
        item.which = pool[k];
        item.cost = static_cast<uint8_t>(joker_cost(pool[k]));
        item.sold = false;
        pool[k] = pool[--pool_len];
    }
    {
        ShopItem& item = w.shop[w.shop_len++];
        item.kind = kShopItem;
        item.which = static_cast<uint8_t>(random_below(w, k_items));
        item.cost = static_cast<uint8_t>(item_cost(item.which));
        item.sold = false;
    }
    {
        ShopItem& item = w.shop[w.shop_len++];
        item.kind = kShopHand;
        // Offered from the shapes a player actually lands, not from the whole
        // ladder. A level in FIVE OF A KIND is a level in a hand most runs
        // never see, so it reads as a wasted slot rather than as a choice.
        static const uint8_t k_offerable[] = {kThree, kTwoPair, kPair, kNothing};
        item.which = k_offerable[random_below(w, 4)];
        item.cost = 5;
        item.sold = false;
    }
    {
        ShopItem& item = w.shop[w.shop_len++];
        item.kind = kShopSwap;
        item.which = 0;
        item.cost = 4;
        item.sold = false;
    }
}

void world_open_shop(World& w) {
    w.state = kShop;
    w.shop_sel = 0;
    w.rerolls = 0;              // the price starts over with the shelf
    stock_shop(w);
}

void world_reroll_shop(World& w) {
    const int cost = reroll_cost(w);
    if (w.gold < cost) return;
    w.gold = static_cast<uint16_t>(w.gold - cost);
    w.rerolls++;
    stock_shop(w);
}

void world_init(World& w, uint32_t seed) {
    w = World{};
    w.rng = seed ? seed : 1u;
    w.state = kTitle;
    w.t = 0;
    w.ante = 1;
    w.target = target_for_ante(1);
    w.banked = 0;
    w.spins = k_spins_per_round;
    w.gold = 4;
    w.speed = kFair;
    w.hand_index = kNothing;
    w.learn_return = kIdle;
    w.item_count = 0;
    w.item_sel = 0;
    w.loaded_count = 0;
    w.rerolls = 0;
    w.focus = kFocusDial;
    w.joker_sel = 0;
    w.joker_menu = 0;
    w.joker_act = kActLeft;
    w.msg = nullptr;

    for (int h = 0; h < k_hands; h++) w.hand_level[h] = 1;

    /* The opening reels: one pool, shuffled per drum.
     *
     * They used to be built as `i % 6` with a one in three chance of a bump,
     * which is a RAMP, and every drum got the same one: two drums carried the
     * same symbol at the same strip position 63% of the time against 17% by
     * chance. With the drums at fixed offsets that made the landed symbols a
     * function of the offsets, and the offsets never changed, so the machine
     * had a handful of outcomes and cycled them.
     *
     * Weighted low, which was the intent all along and survives the change:
     * cherries are common and sevens are scarce, and DIAMOND and CROWN are on
     * no opening reel at all. They are what a swap is for.
     */
    static const uint8_t k_opening_pool[k_strip_start] = {
        kCherry, kCherry, kCherry, kCherry,
        kBell,   kBell,   kBell,
        kPlum,   kPlum,   kPlum,
        kBar,    kBar,
        kClover, kClover,
        kSeven,  kSeven,
    };
    for (int d = 0; d < k_drums; d++) {
        w.strip_len[d] = k_strip_start;
        for (int i = 0; i < k_strip_start; i++) w.strip[d][i] = k_opening_pool[i];
        // Fisher and Yates, so every drum is the same odds in a different
        // order. Same odds matters: a reel a player cannot reason about is
        // not a reel they can aim at.
        for (int i = k_strip_start - 1; i > 0; i--) {
            const int j = random_below(w, i + 1);
            const uint8_t t = w.strip[d][i];
            w.strip[d][i] = w.strip[d][j];
            w.strip[d][j] = t;
        }
        w.angle[d] = 0;
        w.spinning[d] = false;
        w.stopped_at[d] = -1;
    }
    init_facets(w);
    // Start at rest, which is a facet middle at the front rather than a seam.
    for (int d = 0; d < k_drums; d++) snap_drum(w, d);
}

void world_tick(World& w, const Buttons& btn) {
    w.t++;
    if (w.flash > 0) w.flash--;

    switch (w.state) {
        case kTitle: {
            for (int d = 0; d < k_drums; d++) {
                w.angle[d] += 5 + d;
                refresh_facets(w, d);
            }
            if (btn.any) {
                open_learn(w, kIdle);
                for (int d = 0; d < k_drums; d++) snap_drum(w, d);
            }
            return;
        }

        case kLearn: {
            /* Any button turns the page, and left turns it back.
             *
             * Nothing on screen names a button, so no press is the wrong one,
             * which is the half of rule 9 that survives a screen made entirely
             * of text. Left going backwards is the one thing worth knowing and
             * the dots along the bottom are what say there is somewhere to go.
             *
             * Past the last page it returns to WHERE IT WAS OPENED FROM rather
             * than to the machine. It used to always land on the machine, so
             * checking the hand table from the shop closed the shop, and the
             * next ante started with whatever gold was still in your pocket.
             */
            if (btn.left) {
                if (w.learn_page > 0) w.learn_page--;
                else w.state = w.learn_return;
            } else if (btn.any) {
                w.learn_page++;
                if (w.learn_page >= k_learn_pages) w.state = w.learn_return;
            }
            return;
        }

        case kSpin: {
            w.spin_ticks++;
            bool any = false;
            for (int d = 0; d < k_drums; d++) {
                if (!w.spinning[d]) continue;
                if (w.spin_ticks >= k_auto_stop[d]) {
                    // Stopped itself: stopped_at stays -1, so it pays no speed
                    // bonus and counts as a reel you did not touch.
                    w.spinning[d] = false;
                    snap_drum(w, d);
                    continue;
                }
                any = true;
                w.angle[d] += speed_rate(w.speed);
                refresh_facets(w, d);
            }
            turn_dial(w, btn);
            if (btn.a) stop_next(w);
            if (!any) {
                for (int d = 0; d < k_drums; d++) snap_drum(w, d);
                score(w);
                w.state = kCount;
                w.count_wait = 0;
                w.flash = 6;
            }
            return;
        }

        case kCount: {
            if (btn.a) w.rush = 1;
            w.count_wait++;
            if (w.count_wait >= static_cast<uint16_t>(tally_hold(w))) {
                w.count_wait = 0;
                if (!apply_tally_step(w)) finish_score(w);
            }
            return;
        }

        case kIdle: {
            /* The panel has a CURSOR between spins.
             *
             * Three rows of things and one D-pad: up and down hop between the
             * dial, the jokers and the consumables, and left and right act on
             * whichever row the cursor is in. That is what lets a player point
             * at a joker at all, which is what lets the panel say what it does
             * rather than leaving five pictures to be memorised.
             *
             * A is context sensitive and the cursor is what makes that safe:
             * it starts on the dial every time the machine comes back, so the
             * pull is always A from where you were left.
             */
            if (w.joker_menu) {
                // The actions on one joker, which is a modal of three words.
                if (btn.left && w.joker_act > 0) w.joker_act--;
                if (btn.right && w.joker_act + 1 < k_joker_acts) w.joker_act++;
                if (btn.b) w.joker_menu = 0;
                if (btn.a) { do_joker_act(w); w.joker_menu = 0; }
                return;
            }

            if (btn.up) {
                w.focus = static_cast<uint8_t>(w.focus > 0 ? w.focus - 1
                                                           : k_focuses - 1);
            }
            if (btn.down) {
                w.focus = static_cast<uint8_t>((w.focus + 1) % k_focuses);
            }

            if (w.focus == kFocusDial) {
                if (btn.right) {
                    w.speed = static_cast<uint8_t>(
                        w.speed + 1 < k_speeds ? w.speed + 1 : k_speeds - 1);
                }
                if (btn.left) {
                    w.speed = static_cast<uint8_t>(w.speed > 0 ? w.speed - 1 : 0);
                }
            } else if (w.focus == kFocusJokers) {
                if (w.joker_count == 0) {
                    w.joker_sel = 0;
                } else {
                    if (w.joker_sel >= w.joker_count) w.joker_sel = 0;
                    if (btn.left) {
                        w.joker_sel = static_cast<uint8_t>(
                            (w.joker_sel + w.joker_count - 1) % w.joker_count);
                    }
                    if (btn.right) {
                        w.joker_sel = static_cast<uint8_t>(
                            (w.joker_sel + 1) % w.joker_count);
                    }
                }
            } else {
                if (w.item_count == 0) {
                    w.item_sel = 0;
                } else {
                    if (w.item_sel >= w.item_count) w.item_sel = 0;
                    if (btn.left) {
                        w.item_sel = static_cast<uint8_t>(
                            (w.item_sel + w.item_count - 1) % w.item_count);
                    }
                    if (btn.right) {
                        w.item_sel = static_cast<uint8_t>(
                            (w.item_sel + 1) % w.item_count);
                    }
                }
            }

            // The instructions, for the moment a player wants to know what
            // they are aiming at.
            if (btn.b) open_learn(w, kIdle);
            // X spends a consumable wherever the cursor is, because reaching
            // for one is the same reflex whatever you were last looking at.
            if (btn.x) use_item(w);
            if (btn.y && w.item_count > 1) {
                w.item_sel = static_cast<uint8_t>((w.item_sel + 1) % w.item_count);
            }
            if (btn.a) {
                if (w.focus == kFocusJokers && w.joker_count > 0) {
                    w.joker_menu = 1;
                    w.joker_act = kActLeft;
                } else if (w.focus == kFocusItems && w.item_count > 0) {
                    use_item(w);
                } else if (w.focus == kFocusDial) {
                    start_spin(w);
                }
            }
            return;
        }

        case kCleared: {
            if (btn.any) world_open_shop(w);
            return;
        }

        case kShop: {
            /* Up and down, because the shop is a COLUMN.
             *
             * It only ever read left and right, which is the axis the cards do
             * not run along: four cards stacked down the screen with NEXT ANTE
             * under them, moved through sideways. Both axes work now rather
             * than one replacing the other, so a player who learned left and
             * right on the swap screen is not told they were wrong, and a
             * player reaching for the direction the list actually goes in gets
             * what they reached for.
             */
            const bool back = btn.left || btn.up;
            const bool on = btn.right || btn.down;
            const uint8_t last = shop_next_index(w);
            /* And it WRAPS.
             *
             * It stopped dead at both ends, which on a list this short is a
             * button that does nothing several times a visit: from the first
             * card to NEXT ANTE was six presses the wrong way round, and the
             * seventh press of a held direction simply did not answer. Nothing
             * here is ordered, so there is no bottom to fall off.
             */
            if (back) w.shop_sel = w.shop_sel > 0
                                       ? static_cast<uint8_t>(w.shop_sel - 1)
                                       : last;
            if (on) w.shop_sel = w.shop_sel < last
                                     ? static_cast<uint8_t>(w.shop_sel + 1)
                                     : 0;
            // The instructions, from the one screen where a player is spending
            // gold on a hand and wants to know what the hand is worth.
            if (btn.b) open_learn(w, kShop);
            if (btn.a) {
                if (w.shop_sel == shop_reroll_index(w)) world_reroll_shop(w);
                else if (w.shop_sel >= w.shop_len) next_ante(w);
                else buy(w);
            }
            return;
        }

        case kSwap: {
            const int len = w.strip_len[w.swap_drum];
            if (btn.left) w.swap_face = static_cast<uint8_t>((w.swap_face + len - 1) % len);
            if (btn.right) w.swap_face = static_cast<uint8_t>((w.swap_face + 1) % len);
            if (btn.up && w.swap_to + 1 < k_symbols) w.swap_to++;
            if (btn.down && w.swap_to > 0) w.swap_to--;
            if (btn.b) {
                w.swap_drum = static_cast<uint8_t>((w.swap_drum + 1) % k_drums);
                w.swap_face = static_cast<uint8_t>(w.swap_face % w.strip_len[w.swap_drum]);
            }
            if (btn.a) w.strip[w.swap_drum][w.swap_face] = w.swap_to;

            // Ease the selected entry round to the front, and repaint the drum
            // as a plain window on the strip so the entry the panel names is
            // the one under the payline. Entry 14 of a sixteen long strip has
            // no facet of its own, and looking at it is the whole screen.
            const int32_t want = angle_for_facet(w.swap_face % k_facets);
            int32_t cur = w.angle[w.swap_drum];
            // Wind the target to whichever turn is nearest, or stepping from
            // the last strip entry back to the first unwinds a whole
            // revolution to reach somewhere the drum already was.
            int32_t delta = wrap_turn(want - cur);
            if (delta > k_turn / 2) delta -= k_turn;
            w.angle[w.swap_drum] = cur + delta / 5;
            set_facets_from_strip(w, w.swap_drum, w.swap_face);
            if (btn.a) w.state = kShop;
            return;
        }

        case kOver:
        case kWin: {
            if (w.t > 60 && btn.any) {
                const uint32_t seed = w.rng ^ (w.t * 2654435761u);
                world_init(w, seed);
                w.state = kIdle;
            }
            return;
        }

        default:
            return;
    }
}

}  // namespace jr
