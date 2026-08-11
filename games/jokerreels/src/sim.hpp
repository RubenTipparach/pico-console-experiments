#pragma once

// Joker Reels: the rules.
//
// A slot machine with a Balatro shaped run around it. The slot half is the
// verb, the Balatro half is the reason to keep pulling. No SDK and no engine
// here: this file and sim.cpp are plain integer C++, so the whole game can be
// played by the host tests without a device or a window.
//
// Everything is fixed size. There is no allocation anywhere in a run.

#include <cstdint>

namespace jr {

// ---------------------------------------------------------------------------
// The drums
// ---------------------------------------------------------------------------

// Twelve physical facets, and a REEL STRIP that can be longer than that.
//
// A facet that has turned out of sight can be repainted with a different strip
// entry before it comes back round, so a twelve sided prism can present a
// strip of any length. That is how a real machine's virtual reel works, and it
// is what lets a swap put a symbol on a drum that has no facet spare.
constexpr int k_facets = 12;

// Five reels, not three.
//
// Three keeps the drums big and a hand readable at a glance, and it is what
// the mockup was built on. It also caps what a hand can be: on three symbols
// the only shapes are three of a kind, a run, a pair and nothing, which is
// four outcomes and two of them are the same shrug. Five reels is what makes
// a hand table worth having, and a hand table is what gives the deckbuilding
// something to aim at.
constexpr int k_drums = 5;
constexpr int k_strip_start = 12;
constexpr int k_strip_max = 24;

// Angles are in facet steps, fixed point, because the RP2040 has no FPU and
// this runs every tick. One turn is k_facets * k_step_one.
constexpr int k_angle_shift = 8;
constexpr int k_step_one = 1 << k_angle_shift;          // one facet
constexpr int k_turn = k_facets * k_step_one;           // one revolution

constexpr int k_symbols = 8;

// Chips are what a symbol is worth before any multiplier, the way Balatro's
// cards carry chips. The order is tools/gen_jokerreels_symbols.py's ORDER, and
// tools/tests/test_jokerreels_art.py is what keeps the two in step.
enum Symbol : uint8_t {
    kCherry = 0, kBell, kPlum, kBar, kClover, kSeven, kDiamond, kCrown,
};

int symbol_chips(uint8_t symbol);
const char* symbol_name(uint8_t symbol);

// ---------------------------------------------------------------------------
// Hands, in the Balatro sense: what the three landed symbols make
// ---------------------------------------------------------------------------
// Ordered best to worst, which is also the order hand_of tests them in: the
// first shape that fits wins, so FULL HOUSE has to be looked for before THREE
// OF A KIND or a full house would score as three of a kind and the two pair in
// it would count for nothing.
enum Hand : uint8_t {
    kFive = 0, kFour, kFullHouse, kRun, kThree, kTwoPair, kPair, kNothing,
    k_hands,
};

const char* hand_name(uint8_t hand);
int hand_chips(uint8_t hand, int level);
int hand_mult(uint8_t hand, int level);

// ---------------------------------------------------------------------------
// Speed, which is the whole risk of the game on one dial
// ---------------------------------------------------------------------------
//
// Slow enough to read a symbol coming round and stop it, or fast enough that
// you cannot, and the mult pays for the difference.
enum Speed : uint8_t { kSlow = 0, kFair, kWild, k_speeds };

// Angle steps per tick, in the fixed point above.
int speed_rate(uint8_t speed);
int speed_mult(uint8_t speed);
// 0 draws the symbol, 1 draws it too dark to read, 2 does not draw it at all.
int speed_blur(uint8_t speed);
const char* speed_name(uint8_t speed);

// ---------------------------------------------------------------------------
// Jokers: every one is a rule that fires while the score is counted
// ---------------------------------------------------------------------------
enum Joker : uint8_t {
    kGreaser = 0, kTwin, kRatchet, kBlur, kCollector, kMetronome, kSunkCost,
    kUnderstudy, k_jokers,
};

const char* joker_name(uint8_t joker);
const char* joker_text(uint8_t joker);
int joker_cost(uint8_t joker);

constexpr int k_max_jokers = 5;
constexpr int k_antes = 8;
constexpr int k_spins_per_round = 5;

// ---------------------------------------------------------------------------
// Scoring, built as a list of steps rather than one number
// ---------------------------------------------------------------------------
//
// Balatro's whole feel is watching the tally happen: the hand lands, then each
// symbol adds chips, then each joker fires in order. A machine that printed a
// total would throw that away, so the score is a script the render plays back
// one entry at a time.
struct TallyEntry {
    const char* what;
    int16_t chips;
    // -1 marks a x2 rather than an addition, which is the one multiplicative
    // joker and not worth a second field.
    int16_t mult;
    uint8_t colour;
    bool joker;
};
constexpr int k_max_tally = 20;

// Which reels made the hand, so the screen can draw a line through them.
//
// A tally line says PAIR and a number. It does not say WHICH two reels paired,
// and on five reels that is the whole question: a player who cannot see why
// they scored cannot aim at scoring more. So the scorer records the groups and
// the renderer joins them up.
constexpr int k_max_groups = 2;
constexpr uint8_t k_no_group = 255;

enum State : uint8_t {
    kTitle = 0, kLearn, kIdle, kSpin, kCount, kCleared, kShop, kSwap, kOver,
    kWin,
};

// How many pages the how to play screen has. Rule 9 keeps text off the screen
// by default and says in as many words that an explicit request for more text
// wins, which this is: a scoring system nobody can see is not sparse, it is
// opaque. Shown once on the way out of the title, and reachable again from
// the shop, which is the moment a player is deciding what a hand is worth.
constexpr int k_learn_pages = 3;

// What the player pressed this tick. Edge triggered by the caller.
struct Buttons {
    bool a, b, up, down, left, right;
    bool any;
};

enum ShopKind : uint8_t { kShopJoker = 0, kShopHand, kShopSwap };

struct ShopItem {
    uint8_t kind;
    uint8_t which;      // joker index, or hand index
    uint8_t cost;
    bool sold;
};
constexpr int k_shop_items = 4;

struct World {
    uint32_t rng;
    uint8_t state;
    uint32_t t;

    uint8_t ante;
    int32_t target;
    int32_t banked;
    uint8_t spins;
    uint16_t gold;

    // Each drum's ordered ring of symbol indices. Swapping one is the
    // deckbuilding: it changes what that drum can ever land on.
    uint8_t strip[k_drums][k_strip_max];
    uint8_t strip_len[k_drums];

    // What is painted on each facet, and it is STATE rather than a formula.
    //
    // The obvious version computes it from the angle: a window of twelve strip
    // entries centred on whichever entry is at the front. It jitters, and in
    // the worst possible place: that window advances at the instant a facet is
    // dead centre front, and it shifts every facet at once, so the symbol you
    // are looking at changes identity while you are looking at it. Each facet
    // carrying its own symbol, repainted only as it passes behind, is what
    // makes a drum read as one continuous object.
    uint8_t facet[k_drums][k_facets];
    uint8_t cursor[k_drums];        // next strip entry to hand out
    uint16_t behind[k_drums];       // bit per facet: was it hidden last tick

    int32_t angle[k_drums];
    bool spinning[k_drums];
    int8_t stopped_at[k_drums];     // which speed a reel was stopped at, or -1
    uint8_t landed[k_drums];
    // Which match group each reel is in, or k_no_group. Set when a hand is
    // scored and cleared when a spin starts.
    uint8_t group_of[k_drums];
    uint8_t group_count;
    uint32_t spin_ticks;

    uint8_t speed;
    uint8_t jokers[k_max_jokers];
    uint8_t joker_count;
    uint8_t hand_level[k_hands];

    int32_t chips;
    int32_t mult;
    uint8_t hand_index;
    int32_t scored;

    TallyEntry tally[k_max_tally];
    uint8_t tally_len;
    uint8_t tally_step;
    uint16_t count_wait;

    ShopItem shop[k_shop_items];
    uint8_t shop_len;
    uint8_t shop_sel;

    uint8_t learn_page;
    uint8_t swap_drum;
    uint8_t swap_face;
    uint8_t swap_to;

    const char* msg;
    uint8_t flash;
};

// ---------------------------------------------------------------------------
// The geometry the renderer and the rules have to agree on
// ---------------------------------------------------------------------------

// Facet f's middle, as an angle. A facet spans [angle + f*step, +step], so its
// middle is half a step further round. Everything that asks where a facet is
// has to ask about its middle: asking about its leading edge puts the seam
// between two facets dead centre screen, which is a vertical line down every
// reel.
int32_t facet_mid(int32_t angle, int facet);

// Which facet is under the payline, and what is painted on it. The front is
// where the middle is nearest a whole turn, which is the facet the camera is
// looking straight at.
int front_facet(const World& w, int drum);
uint8_t face_at(const World& w, int drum, int facet);

// True when facet f is far enough round to be backface culled, and therefore
// safe to repaint. Deliberately later than the cull itself: being late costs
// nothing, and being early shows the player a symbol changing identity.
bool facet_hidden(int32_t angle, int facet);

// The angle that puts facet f dead centre front.
int32_t angle_for_facet(int facet);

// ---------------------------------------------------------------------------
// The run
// ---------------------------------------------------------------------------
void world_init(World& w, uint32_t seed);
void world_tick(World& w, const Buttons& btn);

// Exposed for the tests and the renderer. Not part of playing a run.
int32_t target_for_ante(int ante);
// The hand, and which reels made it. `groups` is filled with a group index
// per reel, or k_no_group for a reel that took no part.
uint8_t hand_of(const uint8_t landed[k_drums]);
uint8_t hand_groups(const uint8_t landed[k_drums], uint8_t groups[k_drums]);
void world_open_shop(World& w);

}  // namespace jr
