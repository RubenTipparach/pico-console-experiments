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
const HandDef k_hands_table[k_hands] = {
    {"THREE OF A KIND", 100, 8, 40, 3},
    {"RUN",              60, 5, 25, 2},
    {"PAIR",             30, 3, 15, 1},
    {"NOTHING",           5, 1,  5, 1},
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

// Every drum stops itself eventually, in order, unless you stop it first.
// Without this a spin never ends, and the whole "leave them alone" line of
// play, which one joker pays x2 for, is unreachable.
const uint16_t k_auto_stop[k_drums] = {190, 260, 330};

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
    w.landed[drum] = face_at(w, drum, front_facet(w, drum));
}

void push_tally(World& w, const char* what, int chips, int mult,
                uint8_t colour, bool joker) {
    if (w.tally_len >= k_max_tally) return;
    TallyEntry& e = w.tally[w.tally_len++];
    e.what = what;
    e.chips = static_cast<int16_t>(chips);
    e.mult = static_cast<int16_t>(mult);
    e.colour = colour;
    e.joker = joker;
}

void score(World& w) {
    const uint8_t idx = hand_of(w.landed);
    w.hand_index = idx;
    const HandDef& h = k_hands_table[idx];
    const int level = w.hand_level[idx] - 1;
    const int base_chips = h.chips + h.per_chips * level;
    const int base_mult = h.mult + h.per_mult * level;

    w.tally_len = 0;
    w.tally_step = 0;
    w.chips = base_chips;
    w.mult = base_mult;
    push_tally(w, h.name, base_chips, base_mult, 7, false);

    for (int d = 0; d < k_drums; d++) {
        const uint8_t s = w.landed[d];
        push_tally(w, k_symbols_table[s].name, k_symbols_table[s].chips, 0,
                   static_cast<uint8_t>(8 + s % 7), false);
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
                       9, false);
        }
    }

    int counts[k_symbols] = {0};
    for (int d = 0; d < k_drums; d++) counts[w.landed[d]]++;
    int distinct = 0, pairs = 0;
    for (int s = 0; s < k_symbols; s++) {
        if (counts[s] > 0) distinct++;
        if (counts[s] >= 2) pairs++;
    }

    for (int j = 0; j < w.joker_count; j++) {
        uint8_t which = w.jokers[j];
        // UNDERSTUDY copies whatever is to its left, which is why it resolves
        // here rather than when it is bought.
        if (which == kUnderstudy && j > 0) which = w.jokers[j - 1];
        int chips = 0, mult = 0;
        switch (which) {
            case kGreaser:   if (fast_stops == k_drums) mult = 4; break;
            case kTwin:      chips = 30 * pairs; break;
            case kRatchet:   mult = k_spins_per_round - w.spins; break;
            case kBlur:      if (!stopped_any) mult = -1; break;
            case kCollector: chips = 10 * distinct; break;
            case kMetronome: if (stopped_any && all_same) mult = 3; break;
            case kSunkCost:  chips = 50 * (k_spins_per_round - w.spins - 1); break;
            default: break;
        }
        if (chips || mult) {
            push_tally(w, k_jokers_table[w.jokers[j]].name, chips, mult, 10,
                       true);
        }
    }
}

// Play the tally back one entry at a time. The render draws whatever the last
// applied entry was, which is how the count reads as a count.
bool apply_tally_step(World& w) {
    if (w.tally_step >= w.tally_len) return false;
    const TallyEntry& e = w.tally[w.tally_step++];
    if (w.tally_step == 1) return true;      // the hand itself is already in
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
}

void start_spin(World& w) {
    if (w.spins == 0) return;
    w.spins--;
    w.state = kSpin;
    for (int d = 0; d < k_drums; d++) {
        w.spinning[d] = true;
        w.stopped_at[d] = -1;
    }
    w.chips = 0;
    w.mult = 0;
    w.scored = 0;
    w.tally_len = 0;
    w.tally_step = 0;
    w.msg = nullptr;
    w.spin_ticks = 0;
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

void buy(World& w) {
    if (w.shop_sel >= w.shop_len) return;
    ShopItem& item = w.shop[w.shop_sel];
    if (item.sold || w.gold < item.cost) return;
    if (item.kind == kShopJoker) {
        if (w.joker_count >= k_max_jokers) return;
        w.jokers[w.joker_count++] = item.which;
    } else if (item.kind == kShopHand) {
        w.hand_level[item.which]++;
    } else {
        w.state = kSwap;
        w.swap_drum = 0;
        w.swap_face = 0;
        w.swap_to = w.strip[0][0];
    }
    w.gold = static_cast<uint16_t>(w.gold - item.cost);
    item.sold = true;
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
int speed_rate(uint8_t s) { return k_speeds_table[s % k_speeds].rate; }
int speed_mult(uint8_t s) { return k_speeds_table[s % k_speeds].mult; }
int speed_blur(uint8_t s) { return k_speeds_table[s % k_speeds].blur; }
const char* speed_name(uint8_t s) { return k_speeds_table[s % k_speeds].name; }
const char* joker_name(uint8_t j) { return k_jokers_table[j % k_jokers].name; }
const char* joker_text(uint8_t j) { return k_jokers_table[j % k_jokers].text; }
int joker_cost(uint8_t j) { return k_jokers_table[j % k_jokers].cost; }

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

int32_t angle_for_facet(int facet) {
    return -(facet * k_step_one + k_step_one / 2);
}

int32_t target_for_ante(int ante) {
    // 300 and half again each time, in integers rather than a pow.
    int32_t target = 300;
    for (int i = 1; i < ante; i++) target = target * 8 / 5;
    return target;
}

uint8_t hand_of(const uint8_t landed[k_drums]) {
    const uint8_t a = landed[0], b = landed[1], c = landed[2];
    if (a == b && b == c) return kThree;
    uint8_t s[3] = {a, b, c};
    for (int i = 0; i < 2; i++) {
        for (int j = i + 1; j < 3; j++) {
            if (s[j] < s[i]) { const uint8_t t = s[i]; s[i] = s[j]; s[j] = t; }
        }
    }
    if (s[1] == s[0] + 1 && s[2] == s[1] + 1) return kRun;
    if (a == b || b == c || a == c) return kPair;
    return kNothing;
}

void world_open_shop(World& w) {
    w.state = kShop;
    w.shop_sel = 0;
    w.shop_len = 0;

    // Two jokers, a hand to level, and a drum to open. Four things, because a
    // 240 wide panel holds four and a choice between four is still a choice.
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
        item.kind = kShopHand;
        item.which = static_cast<uint8_t>(random_below(w, 3));
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
    w.msg = nullptr;

    for (int h = 0; h < k_hands; h++) w.hand_level[h] = 1;

    for (int d = 0; d < k_drums; d++) {
        w.strip_len[d] = k_strip_start;
        for (int i = 0; i < k_strip_start; i++) {
            // Opening strips are weighted low: the good symbols are things you
            // put on later.
            int s = i % 6;
            if (random_below(w, 10) < 3) s++;
            if (s > 5) s = 5;
            w.strip[d][i] = static_cast<uint8_t>(s);
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
            if (btn.any) { w.state = kIdle; for (int d = 0; d < k_drums; d++) snap_drum(w, d); }
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
            if (btn.up) w.speed = static_cast<uint8_t>(w.speed + 1 < k_speeds ? w.speed + 1 : k_speeds - 1);
            if (btn.down) w.speed = static_cast<uint8_t>(w.speed > 0 ? w.speed - 1 : 0);
            if (btn.a) stop_next(w);
            if (!any) {
                for (int d = 0; d < k_drums; d++) {
                    w.landed[d] = face_at(w, d, front_facet(w, d));
                }
                score(w);
                w.state = kCount;
                w.count_wait = 0;
                w.flash = 6;
            }
            return;
        }

        case kCount: {
            w.count_wait++;
            if (w.count_wait >= 22) {
                w.count_wait = 0;
                if (!apply_tally_step(w)) finish_score(w);
            }
            return;
        }

        case kIdle: {
            if (btn.up) w.speed = static_cast<uint8_t>(w.speed + 1 < k_speeds ? w.speed + 1 : k_speeds - 1);
            if (btn.down) w.speed = static_cast<uint8_t>(w.speed > 0 ? w.speed - 1 : 0);
            if (btn.a) start_spin(w);
            return;
        }

        case kCleared: {
            if (btn.any) world_open_shop(w);
            return;
        }

        case kShop: {
            if (btn.left && w.shop_sel > 0) w.shop_sel--;
            if (btn.right && w.shop_sel < w.shop_len) w.shop_sel++;
            if (btn.a) {
                if (w.shop_sel >= w.shop_len) next_ante(w);
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
