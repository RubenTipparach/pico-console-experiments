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
// Sixteen facets, not twelve.
//
// A facet is 360/N degrees of the drum, so twelve of them is a 30 degree step
// and the reel curves away fast enough that only the front face reads. Sixteen
// is 22.5 degrees, which leaves the two neighbours of the front facet flat
// enough to be symbols rather than slivers, and THREE READABLE ROWS is what
// makes a payline a line: a 5 by 3 grid can be crossed by a middle row, a top,
// a bottom and two diagonals, and a hand can be made along any of them.
constexpr int k_facets = 16;

// Rows of the grid the player is looking at, and the reel's own row order:
// 0 is the top, 1 is the payline, 2 is the bottom.
constexpr int k_rows = 3;

// Five reels, not three.
//
// Three keeps the drums big and a hand readable at a glance, and it is what
// the mockup was built on. It also caps what a hand can be: on three symbols
// the only shapes are three of a kind, a run, a pair and nothing, which is
// four outcomes and two of them are the same shrug. Five reels is what makes
// a hand table worth having, and a hand table is what gives the deckbuilding
// something to aim at.
constexpr int k_drums = 5;
constexpr int k_strip_start = 16;
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
// Consumables: bought once, used once, gone
// ---------------------------------------------------------------------------
//
// A joker is a rule that keeps firing. A consumable is a single decision about
// a single spin, which is a different kind of thing to own: a joker is what
// the run IS, and a consumable is what you do about the spin in front of you.
// Two slots, because holding a hand of them turns every spin into an
// inventory screen and this game has 128 rows of panel.
//
// Some fire the moment they are used and some load the NEXT spin and pay as
// their own line in the count. The difference is in the table: an entry with
// chips or mult is a loaded one, and an entry with neither happens at once.
enum Item : uint8_t {
    kHotStreak = 0, kDoubleDown, kSpareSpin, kLuckyCoin, kPolish, kBlueprint,
    k_items,
};
constexpr int k_max_items = 2;

const char* item_name(uint8_t item);
const char* item_text(uint8_t item);
int item_cost(uint8_t item);
// What it adds to the spin it is used before. Both zero for one that fires
// straight away.
int item_chips(uint8_t item);
int item_mult(uint8_t item);

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
    // Which payline this entry is paying, or k_no_line for the entries that
    // are not about a line at all: the speed bonus and the jokers. The screen
    // draws the line while its entry is showing, so a player watching the
    // count sees the shape being paid for.
    uint8_t line;
    int16_t chips;
    // -1 marks a x2 rather than an addition, which is the one multiplicative
    // joker and not worth a second field.
    int16_t mult;
    // Which joker slot fired this, or k_no_slot for everything else.
    //
    // The name is already in `what`, so this looks redundant. It is not: the
    // screen has to shake the SLOT the joker sits in, and the alternative is
    // a name comparison against all five slots on every frame of the count,
    // to recover something the tally knew when it was written down. The entry
    // says where it came from because that is what it is: a slot did this.
    uint8_t slot;
    bool joker;
};
constexpr int k_max_tally = 20;
constexpr uint8_t k_no_slot = 255;

// Which reels made the hand, so the screen can draw a line through them.
//
// A tally line says PAIR and a number. It does not say WHICH cells paired, and
// on a 5 by 3 grid that is the whole question: a player who cannot see why they
// scored cannot aim at scoring more.
constexpr int k_max_groups = 2;
constexpr uint8_t k_no_group = 255;

// The paylines, in the order they are scored and drawn.
//
// The five a real machine starts with: the middle row, the two outer rows, and
// the two diagonals. Every one of them is a path through one cell per reel, so
// every one of them is five symbols and can make any hand.
enum Payline : uint8_t {
    kMiddle = 0, kTop, kBottom, kVee, kCaret, k_lines,
};
constexpr uint8_t k_no_line = 255;

// The row this line takes on each reel.
const uint8_t* payline_rows(uint8_t line);
const char* payline_name(uint8_t line);

enum State : uint8_t {
    kTitle = 0, kLearn, kIdle, kSpin, kCount, kCleared, kShop, kSwap, kOver,
    kWin,
};

// ---------------------------------------------------------------------------
// The cursor on the panel, between spins
// ---------------------------------------------------------------------------
//
// Three rows of things, and up and down hop between them: the dial, the
// jokers, the consumables. Left and right then act on whichever row the
// cursor is in, which is what lets one D-pad drive three controls without any
// of them needing a button of its own.
//
// The dial keeps both axes MID SPIN, where there is nothing else to point at
// and the dial is the only decision left.
enum Focus : uint8_t { kFocusDial = 0, kFocusJokers, kFocusItems, k_focuses };

// What A offers on a joker: shuffle it along the row, or sell it back.
//
// Order matters to a joker: UNDERSTUDY copies whatever is on its LEFT, so
// moving one is a real play and not tidying.
enum JokerAct : uint8_t { kActLeft = 0, kActRight, kActSell, k_joker_acts };

// Half what it cost, rounded down, and never less than two. Selling at cost
// would make the shop a place to park gold.
int joker_sale(uint8_t joker);

// How many pages the how to play screen has. Rule 9 keeps text off the screen
// by default and says in as many words that an explicit request for more text
// wins, which this is: a scoring system nobody can see is not sparse, it is
// opaque.
//
// It is a MENU rather than a thing that happens to you once. It plays on the
// way out of the title, and B opens it again from between spins and from the
// shop, which are the two moments a player is deciding what a hand is worth.
// It returns to wherever it was opened from, so reading the hand table in the
// shop does not cost the shop.
constexpr int k_learn_pages = 6;

// What the player pressed this tick. Edge triggered by the caller.
struct Buttons {
    bool a, b, x, y, up, down, left, right;
    bool any;
};

enum ShopKind : uint8_t { kShopJoker = 0, kShopHand, kShopItem, kShopSwap };

struct ShopItem {
    uint8_t kind;
    uint8_t which;      // joker index, hand index, or consumable index
    uint8_t cost;
    bool sold;
};
constexpr int k_shop_items = 5;

// Rerolling the shelf, and why it gets dearer.
//
// A flat price is not a decision: with gold to spare you reroll until the
// shelf says what you wanted, which makes the shop a slot machine inside a
// slot machine and takes the choice out of both. Climbing means the first
// reroll is cheap enough to take and the third is a real trade against the
// thing you could have bought with it.
//
// It resets each time the shop opens, rather than climbing across the whole
// run. A price that never comes down is a feature that stops existing around
// ante three, and a feature you cannot use is worse than one you never had.
constexpr int k_reroll_base = 3;
constexpr int k_reroll_step = 2;
int reroll_cost(const struct World& w);

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
    // What is showing, all fifteen cells of it. grid[reel][1] is the payline
    // row, which is what `landed` used to mean on its own.
    uint8_t grid[k_drums][k_rows];
    uint8_t landed[k_drums];
    // The hand each payline made, or kNothing for one that made none.
    uint8_t line_hand[k_lines];
    uint32_t spin_ticks;

    uint8_t speed;
    uint8_t jokers[k_max_jokers];
    uint8_t joker_count;
    uint8_t hand_level[k_hands];

    // The consumables held, which one is picked, and the ones already spent
    // on the spin about to be counted.
    uint8_t items[k_max_items];
    uint8_t item_count;
    uint8_t item_sel;
    uint8_t loaded[k_max_items];
    uint8_t loaded_count;

    int32_t chips;
    int32_t mult;
    // What they were before the entry now showing was applied. The screen
    // counts from these to the ones above across the entry's hold, which is
    // the whole reason a tally is played back rather than summed.
    int32_t chips_from;
    int32_t mult_from;
    uint8_t hand_index;
    int32_t scored;

    TallyEntry tally[k_max_tally];
    uint8_t tally_len;
    uint8_t tally_step;
    uint16_t count_wait;
    // Set by pressing A while the count runs. It does not skip anything: the
    // same entries play in the same order, faster. A count you cannot hurry is
    // a count you resent by the fiftieth spin, and one you can skip outright
    // is one nobody ever reads.
    uint8_t rush;

    ShopItem shop[k_shop_items];
    uint8_t shop_len;
    uint8_t shop_sel;
    uint8_t rerolls;            // this visit, which is what sets the price

    uint8_t focus;              // which row of the panel the cursor is in
    uint8_t joker_sel;
    uint8_t joker_menu;         // 0 closed, else the actions are showing
    uint8_t joker_act;

    uint8_t learn_page;
    uint8_t learn_return;       // the state the instructions were opened from
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

// How long the count holds on the entry that is showing, in ticks.
//
// A joker gets longer than a payline does. It is the only entry with anything
// to watch: the slot shakes and the number it contributed pops over the side
// of the equation it touched, and at the payline's own pace that animation
// starts and is gone before a player has found it. The renderer asks rather
// than keeping a copy, so an animation cannot outlast its own entry.
int tally_hold(const World& w);

// Exposed for the tests and the renderer. Not part of playing a run.
int32_t target_for_ante(int ante);
// The hand, and which reels made it. `groups` is filled with a group index
// per reel, or k_no_group for a reel that took no part.
uint8_t hand_of(const uint8_t landed[k_drums]);
uint8_t hand_groups(const uint8_t landed[k_drums], uint8_t groups[k_drums]);

// The five symbols a payline crosses. The renderer asks for these rather than
// keeping its own idea of where a line goes.
void line_symbols(const World& w, uint8_t line, uint8_t out[k_drums]);

// Which facet of `drum` is showing in `row`. The middle row is the front
// facet; the rows either side are its neighbours, and which neighbour is the
// top one is a fact about how the drum turns, so it lives here.
int facet_in_row(const World& w, int drum, int row);
void world_open_shop(World& w);
void world_reroll_shop(World& w);

// The shop is ONE list: the cards, then REROLL, then NEXT ANTE. Naming the
// two indices past the last card here rather than writing shop_len + 1 in the
// renderer and in the rules keeps them agreeing about which row is which.
uint8_t shop_reroll_index(const World& w);
uint8_t shop_next_index(const World& w);

}  // namespace jr
