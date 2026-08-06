#include "sim.hpp"

// The rules. Every number here is an integer on purpose: the RP2040 has no
// FPU, so a float in a turn resolution is a software emulated multiply in the
// one place a player is watching the screen and waiting.

namespace pm {
namespace {

uint32_t next_random(World& w) {
    // xorshift32. Deterministic from the seed, which is what makes the host
    // tests able to assert on a whole playthrough.
    uint32_t x = w.rng;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    w.rng = x;
    return x;
}

int roll(World& w, int n) { return n > 0 ? int(next_random(w) % uint32_t(n)) : 0; }

const int8_t k_dx[4] = {0, 1, 0, -1};      // north, east, south, west
const int8_t k_dy[4] = {-1, 0, 1, 0};
constexpr uint8_t k_facing_south = 2;

// Stat stages as a fraction, so a stage is a shift and a divide rather than a
// table of floats.
int apply_stage(int value, int stage) {
    if (stage > 0) return value * (2 + stage) / 2;
    if (stage < 0) return value * 2 / (2 - stage);
    return value;
}

const uint8_t k_shake[32] = {
    90, 119, 135, 147, 156, 164, 172, 178,
    183, 189, 193, 198, 202, 206, 210, 213,
    217, 220, 223, 226, 229, 231, 234, 237,
    239, 242, 244, 246, 248, 251, 253, 255,
};

void push_msg(Battle& b, Msg kind, uint8_t a = 0, uint8_t c = 0) {
    if (b.msg_count >= 4) return;      // a turn never needs a fifth line
    b.msgq[(b.msg_head + b.msg_count) & 3] = Message{kind, a, c};
    b.msg_count++;
}

bool pop_msg(Battle& b) {
    if (b.msg_count == 0) return false;
    b.msg_head = (b.msg_head + 1) & 3;
    b.msg_count--;
    return b.msg_count > 0;
}

}  // namespace

// ---------------------------------------------------------------- queries

const Zone& zone_of(const World& w) { return k_zones[w.zone]; }

uint8_t tile_at(const Zone& z, int x, int y) {
    if (x < 0 || y < 0 || x >= z.w || y >= z.h) return 0xFF;
    return z.tiles[y * z.w + x];
}

bool tile_walkable(const Zone& z, int x, int y) {
    const uint8_t t = tile_at(z, x, y);
    if (t == 0xFF) return false;
    return (k_tiles[t].flags & k_tile_walk) != 0;
}

bool npc_present(const World& w, const NpcDef& n) {
    if (n.cond == k_no_flag) return true;
    const bool hide = (n.cond & k_cond_hide) != 0;
    const bool set = flag_get(w, n.cond & 0x7F);
    return hide ? !set : set;
}

void player_offset(const World& w, int16_t& ox, int16_t& oy) {
    if (w.step == 0) { ox = 0; oy = 0; return; }
    // step counts DOWN, from k_step_ticks to zero, so the weight on the tile
    // the player left has to count down with it: full at the start of the
    // step, none by the time they land. Deriving t as k_step_ticks minus the
    // counter runs the slide backwards, and because the camera is pinned to
    // this same offset, the whole world runs backwards with it: the player
    // snapped a tile forward the instant a direction was pressed, drifted
    // back to where they started over the next seven ticks, then snapped
    // forward again. That is the rubber band, once per tile, in every zone.
    const int t = int(w.step) * 256 / k_step_ticks;
    ox = int16_t((int(w.step_from_x) - int(w.tx)) * t);
    oy = int16_t((int(w.step_from_y) - int(w.ty)) * t);
}

bool flag_get(const World& w, uint8_t flag) {
    return (w.flags[flag >> 3] >> (flag & 7)) & 1;
}

void flag_set(World& w, uint8_t flag) {
    w.flags[flag >> 3] |= uint8_t(1u << (flag & 7));
}

namespace {
void dex_set(uint8_t* bits, uint8_t species) {
    bits[species >> 3] |= uint8_t(1u << (species & 7));
}
}  // namespace

// ----------------------------------------------------------------- stats

uint8_t stat_hp(uint8_t species, uint8_t level) {
    const int base = k_species[species].hp;
    const int v = (2 * base * level) / 100 + level + 10;
    return uint8_t(v > 255 ? 255 : v);
}

uint8_t stat_of(uint8_t base, uint8_t level) {
    const int v = (2 * int(base) * level) / 100 + 5;
    return uint8_t(v > 255 ? 255 : v);
}

uint16_t xp_for_level(uint8_t level) {
    // Experience needed to leave this level. Deliberately not a cube: a cube
    // overflows sixteen bits before level fifty, and a save block that has to
    // grow for a curve nobody notices is a bad trade.
    return uint16_t(6 * int(level) * int(level) + 20);
}

Mon make_mon(uint8_t species, uint8_t level) {
    Mon m{};
    m.species = species;
    m.level = level;
    m.xp = 0;
    m.max_hp = stat_hp(species, level);
    m.hp = m.max_hp;
    // The last four moves it is eligible for, oldest dropped first, which is
    // what a player expects without ever being asked.
    const Species& s = k_species[species];
    for (int i = 0; i < 4; i++) { m.moves[i] = 0xFF; m.pp[i] = 0; }
    for (int i = 0; i < s.learn_count; i++) {
        const LearnEntry& e = k_learn[s.learn_first + i];
        if (e.level > level) break;
        for (int j = 0; j < 3; j++) { m.moves[j] = m.moves[j + 1]; m.pp[j] = m.pp[j + 1]; }
        m.moves[3] = e.move;
        m.pp[3] = k_moves[e.move].pp;
    }
    // Slide them to the front so slot 0 is always filled.
    int write = 0;
    for (int i = 0; i < 4; i++) {
        if (m.moves[i] == 0xFF) continue;
        m.moves[write] = m.moves[i];
        m.pp[write] = m.pp[i];
        write++;
    }
    for (int i = write; i < 4; i++) { m.moves[i] = 0xFF; m.pp[i] = 0; }
    return m;
}

// --------------------------------------------------------------- formulas

int type_multiplier(uint8_t attack_type, uint8_t defend_type) {
    // Ember burns leaf, leaf breaks stone, stone grounds spark, spark charges
    // tide, tide douses ember. Mind is even against everything and doubled
    // against itself. One rule and no chart to show on a 120 pixel screen.
    static const uint8_t k_beats[6] = {2, 0, 4, 1, 3, 5};
    const uint8_t mind = uint8_t(Type::Mind);
    if (attack_type == mind && defend_type == mind) return 8;
    if (attack_type == mind || defend_type == mind) return 4;
    if (k_beats[attack_type] == defend_type) return 8;
    if (k_beats[defend_type] == attack_type) return 2;
    return 4;
}

int damage_of(const Mon& attacker, const Mon& defender, uint8_t move,
              int8_t atk_stage, int8_t def_stage, uint8_t rng_roll) {
    const Move& mv = k_moves[move];
    if (mv.power == 0) return 0;
    const Species& as = k_species[attacker.species];
    const Species& ds = k_species[defender.species];
    const int atk = apply_stage(stat_of(as.atk, attacker.level), atk_stage);
    const int def = apply_stage(stat_of(ds.def, defender.level), def_stage);
    const int mult = type_multiplier(mv.type, ds.type);
    // Evaluated left to right so the intermediate never leaves 32 bits: the
    // largest term here is about 2 * 100 / 5 * 95 * 255, well inside it.
    int d = (2 * attacker.level / 5 + 2) * mv.power;
    d = d * atk / (def > 0 ? def : 1);
    d = d / 50 + 2;
    d = d * mult / 4;
    d = d * (217 + rng_roll % 39) / 255;
    return d < 1 ? 1 : d;
}

uint8_t catch_value(const Mon& target, uint8_t ball_quarters) {
    const int max_hp = target.max_hp > 0 ? target.max_hp : 1;
    int a = (3 * max_hp - 2 * target.hp) * k_species[target.species].catch_rate;
    a = a * ball_quarters / 4;
    a = a / (3 * max_hp);
    if (a < 1) a = 1;
    if (a > 255) a = 255;
    return uint8_t(a);
}

uint8_t shake_threshold(uint8_t catch_a) {
    // The real formula is a fourth root, which is a non starter on a chip with
    // no FPU. tools/ generated this table from it once; the device indexes it.
    return k_shake[catch_a >> 3];
}

// ------------------------------------------------------------- lifecycle

void world_new_game(World& w) {
    w.mode = Mode::Overworld;
    w.zone = k_start.zone;
    w.tx = k_start.x;
    w.ty = k_start.y;
    w.facing = k_start.facing;
    w.step = 0;
    // Until the player rests at a CENTRE, home is where they woke up.
    w.home_zone = k_start.zone;
    w.home_x = k_start.x;
    w.home_y = k_start.y;
    w.money = k_start.money;
    w.party_count = 0;
    for (int i = 0; i < k_start.party_count && i < k_max_party; i++) {
        const PartyEntry& p = k_parties[k_start.party_first + i];
        w.party[w.party_count++] = make_mon(p.species, p.level);
        dex_set(w.seen, p.species);
        dex_set(w.caught, p.species);
    }
    w.bag_count = 0;
    for (int i = 0; i < k_start.bag_count && i < k_max_bag; i++) {
        w.bag[w.bag_count].item = k_start_bag[i].item;
        w.bag[w.bag_count].count = k_start_bag[i].count;
        w.bag_count++;
    }
}

void world_init(World& w, uint32_t seed) {
    w = World{};
    w.rng = seed ? seed : 0x9E3779B9u;
    w.mode = Mode::Title;
    w.battle.trainer_npc = 0xFF;
    world_new_game(w);
    w.mode = Mode::Title;
}

void world_make_save(const World& w, SaveData& out) {
    out = SaveData{};
    out.version = k_save_version;
    out.zone = w.zone;
    out.x = w.tx;
    out.y = w.ty;
    out.facing = w.facing;
    out.home_zone = w.home_zone;
    out.home_x = w.home_x;
    out.home_y = w.home_y;
    out.money = w.money;
    out.party_count = w.party_count;
    for (int i = 0; i < k_max_party; i++) out.party[i] = w.party[i];
    out.bag_count = w.bag_count;
    for (int i = 0; i < k_max_bag; i++) out.bag[i] = w.bag[i];
    for (int i = 0; i < k_flag_bytes; i++) out.flags[i] = w.flags[i];
    for (int i = 0; i < k_dex_bytes; i++) { out.seen[i] = w.seen[i]; out.caught[i] = w.caught[i]; }
}

bool world_load(World& w, const SaveData& in) {
    if (in.version != k_save_version) return false;
    if (in.zone >= k_zone_count) return false;
    if (in.party_count == 0 || in.party_count > k_max_party) return false;
    w.zone = in.zone;
    w.tx = in.x;
    w.ty = in.y;
    w.facing = in.facing & 3;
    if (in.home_zone >= k_zone_count) return false;
    if (!tile_walkable(k_zones[in.home_zone], in.home_x, in.home_y)) return false;
    w.home_zone = in.home_zone;
    w.home_x = in.home_x;
    w.home_y = in.home_y;
    w.money = in.money;
    w.party_count = in.party_count;
    for (int i = 0; i < k_max_party; i++) w.party[i] = in.party[i];
    w.bag_count = in.bag_count > k_max_bag ? k_max_bag : in.bag_count;
    for (int i = 0; i < k_max_bag; i++) w.bag[i] = in.bag[i];
    for (int i = 0; i < k_flag_bytes; i++) w.flags[i] = in.flags[i];
    for (int i = 0; i < k_dex_bytes; i++) { w.seen[i] = in.seen[i]; w.caught[i] = in.caught[i]; }
    if (!tile_walkable(zone_of(w), w.tx, w.ty)) return false;
    w.step = 0;
    w.mode = Mode::Overworld;
    return true;
}

// ---------------------------------------------------------------- dialogue

namespace {

void say(World& w, uint16_t first, uint8_t count, uint8_t npc = 0xFF) {
    if (count == 0) return;
    w.text_first = first;
    w.text_count = count;
    w.text_page = 0;
    w.talking_npc = npc;
    w.mode = Mode::Dialogue;
}

int bag_find(const World& w, uint8_t item) {
    for (int i = 0; i < w.bag_count; i++) if (w.bag[i].item == item) return i;
    return -1;
}

void bag_add(World& w, uint8_t item, uint8_t count) {
    const int at = bag_find(w, item);
    if (at >= 0) {
        const int total = w.bag[at].count + count;
        w.bag[at].count = uint8_t(total > 99 ? 99 : total);
        return;
    }
    if (w.bag_count >= k_max_bag) return;
    w.bag[w.bag_count].item = item;
    w.bag[w.bag_count].count = count;
    w.bag_count++;
}

void bag_take(World& w, int slot) {
    if (slot < 0 || slot >= w.bag_count) return;
    if (--w.bag[slot].count > 0) return;
    for (int i = slot; i + 1 < w.bag_count; i++) w.bag[i] = w.bag[i + 1];
    w.bag_count--;
}

int first_alive(const World& w) {
    for (int i = 0; i < w.party_count; i++) if (w.party[i].hp > 0) return i;
    return -1;
}

// Full HP and full PP for the whole party. The nurse does this, and so does a
// whiteout: waking up with no health is a soft lock, and waking up with no PP
// is the same soft lock two steps later, which is why both are here and not
// just the health.
void party_restore(World& w) {
    for (int i = 0; i < w.party_count; i++) {
        w.party[i].hp = w.party[i].max_hp;
        for (int m = 0; m < 4; m++) {
            if (w.party[i].moves[m] != 0xFF) {
                w.party[i].pp[m] = k_moves[w.party[i].moves[m]].pp;
            }
        }
    }
}

}  // namespace

// ---------------------------------------------------------------- battle

namespace {

void start_battle(World& w, const Mon& foe, bool wild, uint8_t npc_index,
                  uint16_t reward) {
    Battle& b = w.battle;
    b = Battle{};
    b.foe = foe;
    b.wild = wild;
    b.trainer_npc = npc_index;
    b.trainer_next = 1;
    b.reward = reward;
    b.active = uint8_t(first_alive(w) < 0 ? 0 : first_alive(w));
    b.state = BattleState::Intro;
    b.ball_item = 0xFF;
    b.msg_head = 0;
    b.msg_count = 0;
    push_msg(b, wild ? Msg::WildAppeared : Msg::TrainerSent, foe.species);
    dex_set(w.seen, foe.species);
    w.mode = Mode::Battle;
    w.sfx = Sfx::Encounter;
}

void grant_xp(World& w, Mon& winner, const Mon& loser) {
    Battle& b = w.battle;
    const int gain = k_species[loser.species].xp_yield * loser.level / 7;
    int xp = winner.xp + gain;
    while (winner.level < 50 && xp >= xp_for_level(winner.level)) {
        xp -= xp_for_level(winner.level);
        winner.level++;
        const uint8_t new_max = stat_hp(winner.species, winner.level);
        winner.hp = uint8_t(winner.hp + (new_max - winner.max_hp));
        winner.max_hp = new_max;
        push_msg(b, Msg::LevelUp, winner.species, winner.level);
        w.sfx = Sfx::LevelUp;
        // A move learned at this level replaces the oldest one it has.
        const Species& s = k_species[winner.species];
        for (int i = 0; i < s.learn_count; i++) {
            const LearnEntry& e = k_learn[s.learn_first + i];
            if (e.level != winner.level) continue;
            bool known = false;
            for (int j = 0; j < 4; j++) known |= winner.moves[j] == e.move;
            if (known) continue;
            for (int j = 0; j < 3; j++) {
                winner.moves[j] = winner.moves[j + 1];
                winner.pp[j] = winner.pp[j + 1];
            }
            winner.moves[3] = e.move;
            winner.pp[3] = k_moves[e.move].pp;
        }
        // Evolution, checked after the level lands so it can chain.
        if (s.evolve_level && winner.level >= s.evolve_level) {
            winner.species = s.evolve_into;
            const uint8_t evolved_max = stat_hp(winner.species, winner.level);
            winner.hp = uint8_t(winner.hp + (evolved_max - winner.max_hp));
            winner.max_hp = evolved_max;
            dex_set(w.seen, winner.species);
            dex_set(w.caught, winner.species);
            push_msg(b, Msg::Evolved, winner.species);
        }
    }
    winner.xp = uint16_t(xp);
}

void apply_effect(Battle& b, uint8_t effect, bool on_foe) {
    int8_t* stage = nullptr;
    Msg msg = Msg::StatFell;
    switch (MoveEffect(effect)) {
        case MoveEffect::LowerDef: stage = on_foe ? &b.foe_def_stage : &b.def_stage; break;
        case MoveEffect::LowerSpd: stage = on_foe ? &b.foe_spd_stage : &b.spd_stage; break;
        case MoveEffect::RaiseDef: stage = on_foe ? &b.foe_def_stage : &b.def_stage;
                                   msg = Msg::StatRose; break;
        case MoveEffect::RaiseSpd: stage = on_foe ? &b.foe_spd_stage : &b.spd_stage;
                                   msg = Msg::StatRose; break;
        default: return;
    }
    const int delta = (msg == Msg::StatRose) ? 1 : -1;
    const int next = *stage + delta;
    if (next < -6 || next > 6) return;
    *stage = int8_t(next);
    push_msg(b, msg, on_foe ? 1 : 0);
}

// One creature attacks the other. Returns true if the target fainted.
bool use_move(World& w, Battle& b, bool by_player, uint8_t move_slot) {
    Mon& attacker = by_player ? w.party[b.active] : b.foe;
    Mon& target = by_player ? b.foe : w.party[b.active];
    const uint8_t move = attacker.moves[move_slot];
    if (move == 0xFF) return false;
    if (by_player && attacker.pp[move_slot] == 0) {
        push_msg(b, Msg::OutOfPP);
        return false;
    }
    if (attacker.pp[move_slot] > 0) attacker.pp[move_slot]--;
    push_msg(b, by_player ? Msg::YouUsed : Msg::FoeUsed, move);
    b.anim_kind = k_moves[move].type;

    const Move& mv = k_moves[move];
    if (mv.power == 0) {
        apply_effect(b, mv.effect, by_player);
        return false;
    }
    const int mult = type_multiplier(mv.type, k_species[target.species].type);
    const int dmg = damage_of(attacker, target,move,
                              by_player ? b.atk_stage : b.foe_atk_stage,
                              by_player ? b.foe_def_stage : b.def_stage,
                              uint8_t(next_random(w)));
    const int taken = dmg >= target.hp ? target.hp : dmg;
    target.hp = uint8_t(target.hp - taken);
    // Record it for the animation beat. What it lost, not what was rolled, so
    // the bar the renderer drains matches the bar it lands on.
    const int side = by_player ? Battle::k_foe : Battle::k_you;
    b.fx_dmg[side] = uint8_t(taken);
    b.fx_mult[side] = uint8_t(mult);
    b.fx_type[side] = mv.type;
    w.sfx = mult > 4 ? Sfx::SuperHit : Sfx::Hit;
    if (mult > 4) push_msg(b, Msg::SuperEffective);
    else if (mult < 4) push_msg(b, Msg::NotVery);
    if (MoveEffect(mv.effect) == MoveEffect::Drain) {
        const int heal = dmg / 2 + 1;
        const int cap = attacker.max_hp - attacker.hp;
        attacker.hp = uint8_t(attacker.hp + (heal > cap ? cap : heal));
    }
    return target.hp == 0;
}

void foe_turn(World& w, Battle& b) {
    if (b.foe.hp == 0 || w.party[b.active].hp == 0) return;
    // The foe picks the move that would hurt most, which is enough thought for
    // a creature and cheaper than a table of behaviours.
    int best = 0, best_dmg = -1;
    for (int i = 0; i < 4; i++) {
        if (b.foe.moves[i] == 0xFF || b.foe.pp[i] == 0) continue;
        const int d = damage_of(b.foe, w.party[b.active], b.foe.moves[i],
                                b.foe_atk_stage, b.def_stage, 236);
        if (d > best_dmg) { best_dmg = d; best = i; }
    }
    if (best_dmg < 0) return;
    if (use_move(w, b, false, uint8_t(best))) {
        push_msg(b, Msg::YouFainted, w.party[b.active].species);
        w.sfx = Sfx::Faint;
    }
}

void resolve_turn(World& w, Battle& b) {
    // A fresh turn, so nothing from the last one plays again.
    b.fx_dmg[0] = b.fx_dmg[1] = 0;
    b.fx_mult[0] = b.fx_mult[1] = 4;
    b.fx_type[0] = b.fx_type[1] = 0;
    const Mon& me = w.party[b.active];
    const int my_spd = apply_stage(stat_of(k_species[me.species].spd, me.level), b.spd_stage);
    const int foe_spd = apply_stage(
        stat_of(k_species[b.foe.species].spd, b.foe.level), b.foe_spd_stage);
    b.player_first = my_spd >= foe_spd;

    if (b.player_first) {
        if (use_move(w, b, true, b.queued_move)) {
            push_msg(b, Msg::FoeFainted, b.foe.species);
            w.sfx = Sfx::Faint;
        } else {
            foe_turn(w, b);
        }
    } else {
        foe_turn(w, b);
        if (w.party[b.active].hp > 0) {
            if (use_move(w, b, true, b.queued_move)) {
                push_msg(b, Msg::FoeFainted, b.foe.species);
                w.sfx = Sfx::Faint;
            }
        }
    }
    b.state = BattleState::Attack;
    b.timer = 14;
}

const NpcDef* trainer_of(const World& w) {
    if (w.battle.trainer_npc == 0xFF) return nullptr;
    const Zone& z = zone_of(w);
    if (w.battle.trainer_npc >= z.npc_count) return nullptr;
    return &z.npcs[w.battle.trainer_npc];
}

void end_battle(World& w) {
    Battle& b = w.battle;
    const NpcDef* t = trainer_of(w);
    if (t && b.foe.hp == 0) {
        flag_set(w, t->flag);
        const int total = w.money + b.reward;
        w.money = uint16_t(total > 65535 ? 65535 : total);
    }
    w.battle.trainer_npc = 0xFF;
    w.save_pending = true;
    if (t && t->win_count && b.foe.hp == 0) {
        say(w, t->win_first, t->win_count);
        return;
    }
    w.mode = Mode::Overworld;
}

// The foe fainted. A trainer sends out the next one, a wild battle ends.
void after_foe_faint(World& w, Battle& b) {
    grant_xp(w, w.party[b.active], b.foe);
    const NpcDef* t = trainer_of(w);
    if (t && b.trainer_next < t->party_count) {
        const PartyEntry& p = k_parties[t->party_first + b.trainer_next];
        b.trainer_next++;
        b.foe = make_mon(p.species, p.level);
        b.foe_atk_stage = b.foe_def_stage = b.foe_spd_stage = 0;
        dex_set(w.seen, b.foe.species);
        push_msg(b, Msg::TrainerSent, b.foe.species);
        b.state = BattleState::Message;
        return;
    }
    b.state = BattleState::Over;
}

// Defined with the rest of the overworld, below. A whiteout is a zone change
// like any other and should not grow its own copy of one.
void enter_zone(World& w, uint8_t zone, uint8_t x, uint8_t y, uint8_t facing);

// Every creature is down.
//
// The party is patched up and the player wakes at the last CENTRE they rested
// at, which is what `home` records; before they have rested anywhere it is
// where the game started them. It used to be the start of the game
// unconditionally, and silently: the screen simply cut to a different town
// with no line of text, which reads as a bug rather than as a defeat.
void whiteout(World& w) {
    party_restore(w);
    w.battle = Battle{};
    w.battle.trainer_npc = 0xFF;
    w.mode = Mode::Overworld;
    enter_zone(w, w.home_zone, w.home_x, w.home_y, k_facing_south);
    say(w, k_start.whiteout_first, k_start.whiteout_count);
}

void battle_tick(World& w, const Input& in) {
    Battle& b = w.battle;
    const bool go = in.a_pressed || in.b_pressed;

    switch (b.state) {
        case BattleState::Intro:
        case BattleState::Message:
            if (!go) return;
            if (pop_msg(b)) return;
            if (b.foe.hp == 0) { after_foe_faint(w, b); return; }
            if (w.party[b.active].hp == 0) {
                const int next = first_alive(w);
                if (next < 0) {
                    whiteout(w);
                    return;
                }
                b.active = uint8_t(next);
                b.atk_stage = b.def_stage = b.spd_stage = 0;
                b.state = BattleState::Menu;
                return;
            }
            b.state = b.caught ? BattleState::Over : BattleState::Menu;
            return;

        case BattleState::Over:
            if (!go) return;
            if (pop_msg(b)) return;
            end_battle(w);
            return;

        case BattleState::Attack:
            if (b.timer > 0) { b.timer--; return; }
            b.state = BattleState::Message;
            return;

        case BattleState::Menu:
            if (in.up_pressed || in.down_pressed) b.cursor ^= 2;
            if (in.left_pressed || in.right_pressed) b.cursor ^= 1;
            if (!in.a_pressed) return;
            w.sfx = Sfx::Select;
            if (b.cursor == 0) { b.state = BattleState::Moves; return; }
            if (b.cursor == 1) { w.mode = Mode::Bag; w.menu_cursor = 0; return; }
            if (b.cursor == 2) { w.mode = Mode::Party; w.menu_cursor = 0; return; }
            if (!b.wild) {
                push_msg(b, Msg::NoRunning);
                b.state = BattleState::Message;
                return;
            }
            if (int(next_random(w) & 0xFF) < 160) {
                // Getting away is not the foe fainting, so it must not take
                // the experience path. Over is the only exit that skips it.
                push_msg(b, Msg::GotAway);
                b.state = BattleState::Over;
            } else {
                push_msg(b, Msg::CouldNotRun);
                foe_turn(w, b);
                b.state = BattleState::Message;
            }
            return;

        case BattleState::Moves: {
            if (in.up_pressed || in.down_pressed) b.move_cursor ^= 2;
            if (in.left_pressed || in.right_pressed) b.move_cursor ^= 1;
            if (in.b_pressed) { b.state = BattleState::Menu; return; }
            if (!in.a_pressed) return;
            const Mon& me = w.party[b.active];
            if (me.moves[b.move_cursor] == 0xFF) return;
            if (me.pp[b.move_cursor] == 0) {
                push_msg(b, Msg::OutOfPP);
                b.state = BattleState::Message;
                return;
            }
            b.queued_move = b.move_cursor;
            resolve_turn(w, b);
            return;
        }

        case BattleState::Throw:
            if (b.timer > 0) { b.timer--; return; }
            b.state = BattleState::Wobble;
            b.timer = 22;
            return;

        case BattleState::Wobble: {
            if (b.timer > 0) { b.timer--; return; }
            const uint8_t a = catch_value(b.foe, k_items[b.ball_item].param);
            const uint8_t threshold = shake_threshold(a);
            if (uint8_t(next_random(w)) > threshold) {
                push_msg(b, Msg::BrokeFree, b.foe.species);
                b.state = BattleState::Message;
                w.sfx = Sfx::Click;
                foe_turn(w, b);
                return;
            }
            if (++b.wobbles < 3) { b.timer = 22; w.sfx = Sfx::Click; return; }
            b.caught = true;
            dex_set(w.caught, b.foe.species);
            if (w.party_count < k_max_party) w.party[w.party_count++] = b.foe;
            push_msg(b, Msg::CaughtIt, b.foe.species);
            b.state = BattleState::Caught;
            b.timer = 24;
            w.sfx = Sfx::Caught;
            return;
        }

        case BattleState::Caught:
            if (b.timer > 0) { b.timer--; return; }
            b.state = BattleState::Message;
            return;
    }
}

}  // namespace

// -------------------------------------------------------------- overworld

namespace {

const WarpDef* warp_at(const Zone& z, int x, int y) {
    for (int i = 0; i < z.warp_count; i++) {
        if (z.warps[i].x == x && z.warps[i].y == y) return &z.warps[i];
    }
    return nullptr;
}

const NpcDef* npc_at(const World& w, int x, int y, uint8_t* index_out) {
    const Zone& z = zone_of(w);
    for (int i = 0; i < z.npc_count; i++) {
        const NpcDef& n = z.npcs[i];
        if (n.x != x || n.y != y || !npc_present(w, n)) continue;
        if (index_out) *index_out = uint8_t(i);
        return &n;
    }
    return nullptr;
}

const EventDef* event_at(const World& w, int x, int y, EventKind kind) {
    const Zone& z = zone_of(w);
    for (int i = 0; i < z.event_count; i++) {
        const EventDef& e = z.events[i];
        if (e.x == x && e.y == y && EventKind(e.kind) == kind) return &e;
    }
    return nullptr;
}

// The challenge line comes first and the fight starts when it finishes, which
// is why this only arms the battle rather than starting it.
void begin_trainer_battle(World& w, uint8_t npc_index) {
    const NpcDef& n = zone_of(w).npcs[npc_index];
    w.battle.trainer_npc = npc_index;
    w.battle.foe.max_hp = 0;      // nothing is out yet
    say(w, n.say_first, n.say_count, npc_index);
}

// A trainer sees the player when they are in front of it, within its sight,
// with nothing solid in between. Drawn on the ground while unbeaten, so the
// trap is avoidable rather than a gotcha.
bool trainer_sees(const World& w, const NpcDef& n) {
    if (NpcKind(n.kind) != NpcKind::Trainer || n.sight == 0) return false;
    if (n.flag != k_no_flag && flag_get(w, n.flag)) return false;
    if (!npc_present(w, n)) return false;
    const Zone& z = zone_of(w);
    for (int i = 1; i <= n.sight; i++) {
        const int x = n.x + k_dx[n.facing] * i;
        const int y = n.y + k_dy[n.facing] * i;
        if (!tile_walkable(z, x, y)) return false;
        if (w.tx == x && w.ty == y) return true;
    }
    return false;
}

void enter_zone(World& w, uint8_t zone, uint8_t x, uint8_t y, uint8_t facing) {
    w.zone = zone;
    w.tx = x;
    w.ty = y;
    w.facing = facing;
    w.step = 0;
    w.fade = 10;
    // An armed but unstarted trainer battle does not travel. trainer_npc is
    // an index into the zone the challenge was issued in, so carrying it
    // across a warp points it at a different zone's array, or at nothing.
    // Nothing in normal play can walk away mid challenge, because a dialogue
    // holds the d pad, but the invariant should hold because it is enforced
    // rather than because no path happens to break it today.
    if (w.battle.foe.max_hp == 0) w.battle.trainer_npc = 0xFF;
    w.save_pending = true;
}

void try_encounter(World& w) {
    const Zone& z = zone_of(w);
    const uint8_t t = tile_at(z, w.tx, w.ty);
    if (t == 0xFF || !(k_tiles[t].flags & k_tile_encounter)) return;
    for (int i = 0; i < z.enc_count; i++) {
        const EncTable& tab = z.enc[i];
        if (tab.tile != t) continue;
        if (roll(w, 100) >= tab.rate) return;
        const uint8_t pick = uint8_t(next_random(w));
        for (int s = 0; s < tab.count; s++) {
            const EncSlot& slot = z.enc_slots[tab.first + s];
            if (pick > slot.cumulative && s + 1 < tab.count) continue;
            const int span = slot.max_level - slot.min_level + 1;
            const uint8_t level = uint8_t(slot.min_level + roll(w, span));
            start_battle(w, make_mon(slot.species, level), true, 0xFF, 0);
            return;
        }
        return;
    }
}

void step_landed(World& w) {
    const Zone& z = zone_of(w);
    w.sfx = Sfx::Step;

    if (const WarpDef* wp = warp_at(z, w.tx, w.ty)) {
        enter_zone(w, wp->dest, wp->dx, wp->dy, wp->facing);
        return;
    }
    if (const EventDef* e = event_at(w, w.tx, w.ty, EventKind::Trigger)) {
        if (e->flag == k_no_flag || !flag_get(w, e->flag)) {
            if (e->flag != k_no_flag) flag_set(w, e->flag);
            if (e->say_count) { say(w, e->say_first, e->say_count); return; }
        }
    }
    for (int i = 0; i < z.npc_count; i++) {
        if (!trainer_sees(w, z.npcs[i])) continue;
        begin_trainer_battle(w, uint8_t(i));
        return;
    }
    try_encounter(w);
}

void try_step(World& w, uint8_t dir) {
    const Zone& z = zone_of(w);
    w.facing = dir;
    const int nx = w.tx + k_dx[dir];
    const int ny = w.ty + k_dy[dir];
    const uint8_t t = tile_at(z, nx, ny);
    if (t == 0xFF) { w.sfx = Sfx::Bump; return; }
    const uint8_t flags = k_tiles[t].flags;
    if (!(flags & k_tile_walk)) { w.sfx = Sfx::Bump; return; }

    const uint8_t ledge = flags & k_tile_ledge_mask;
    if (ledge) {
        // A ledge is one way: it may only be entered travelling the way it
        // faces, and doing so carries the player an extra tile. That is what
        // makes a route a loop instead of a corridor walked twice.
        static const uint8_t k_ledge_dir[5] = {0, 0, 1, 2, 3};
        const uint8_t want = k_ledge_dir[ledge >> 5];
        if (dir != want) { w.sfx = Sfx::Bump; return; }
        w.ledge_hop = 1;
    }
    if (npc_at(w, nx, ny, nullptr)) { w.sfx = Sfx::Bump; return; }

    w.step_from_x = w.tx;
    w.step_from_y = w.ty;
    w.tx = uint8_t(nx);
    w.ty = uint8_t(ny);
    w.step = k_step_ticks;
}

void interact(World& w) {
    const int fx = w.tx + k_dx[w.facing];
    const int fy = w.ty + k_dy[w.facing];

    uint8_t index = 0;
    if (const NpcDef* n = npc_at(w, fx, fy, &index)) {
        if (NpcKind(n->kind) == NpcKind::Trainer &&
            (n->flag == k_no_flag || !flag_get(w, n->flag))) {
            begin_trainer_battle(w, index);
            return;
        }
        if (NpcKind(n->kind) == NpcKind::Healer) {
            party_restore(w);
            // Resting here also makes it home. The tile recorded is the one
            // the player is standing on, not the nurse's: an NPC blocks its
            // own tile, so waking up on it would trap them inside her.
            w.home_zone = w.zone;
            w.home_x = w.tx;
            w.home_y = w.ty;
            w.sfx = Sfx::Heal;
            w.save_pending = true;
        }
        say(w, n->say_first, n->say_count, index);
        return;
    }
    if (const EventDef* e = event_at(w, fx, fy, EventKind::Sign)) {
        say(w, e->say_first, e->say_count);
        return;
    }
    for (int pass = 0; pass < 2; pass++) {
        const int x = pass == 0 ? fx : w.tx;
        const int y = pass == 0 ? fy : w.ty;
        const EventDef* e = event_at(w, x, y, EventKind::Item);
        if (!e || (e->flag != k_no_flag && flag_get(w, e->flag))) continue;
        bag_add(w, e->arg0, e->arg1);
        if (e->flag != k_no_flag) flag_set(w, e->flag);
        w.sfx = Sfx::Item;
        w.save_pending = true;
        return;
    }
}

void overworld_tick(World& w, const Input& in) {
    if (w.fade > 0) { w.fade--; return; }
    if (w.step > 0) {
        w.step--;
        if (w.step == 0) {
            w.anim_phase = uint8_t((w.anim_phase + 1) & 3);
            if (w.ledge_hop) {
                w.ledge_hop = 0;
                const int nx = w.tx + k_dx[w.facing];
                const int ny = w.ty + k_dy[w.facing];
                if (tile_walkable(zone_of(w), nx, ny)) {
                    w.step_from_x = w.tx;
                    w.step_from_y = w.ty;
                    w.tx = uint8_t(nx);
                    w.ty = uint8_t(ny);
                    w.step = k_step_ticks;
                    return;
                }
            }
            step_landed(w);
        }
        return;
    }

    if (in.a_pressed) { interact(w); return; }
    if (in.x_pressed) { w.mode = Mode::Bag; w.menu_cursor = 0; w.menu_pocket = 0; return; }
    if (in.y_pressed) { w.mode = Mode::Party; w.menu_cursor = 0; return; }

    if (in.up) try_step(w, 0);
    else if (in.right) try_step(w, 1);
    else if (in.down) try_step(w, 2);
    else if (in.left) try_step(w, 3);
}

// The counter the player is standing at. Read off talking_npc rather than
// stored: the shop is only ever open while its keeper is being talked to, so
// a second copy of that could only ever disagree.
const NpcDef* shop_at(const World& w) {
    if (w.talking_npc == 0xFF) return nullptr;
    const Zone& z = zone_of(w);
    if (w.talking_npc >= z.npc_count) return nullptr;
    const NpcDef& n = z.npcs[w.talking_npc];
    if (NpcKind(n.kind) != NpcKind::Shop || n.stock_count == 0) return nullptr;
    return &n;
}

void dialogue_tick(World& w, const Input& in) {
    if (!in.a_pressed && !in.b_pressed) return;
    if (w.text_page + 1 < w.text_count) { w.text_page++; return; }
    w.mode = Mode::Overworld;
    const uint8_t npc = w.battle.trainer_npc;
    if (npc == 0xFF || w.battle.foe.max_hp != 0) {
        // A shopkeeper greets first and opens the counter afterwards, so the
        // greeting is not something to press through before the list appears
        // and is not a second screen once it has.
        if (shop_at(w)) {
            w.mode = Mode::Shop;
            w.menu_cursor = 0;
        }
        return;
    }
    // trainer_npc is an index into whatever zone the player was in when the
    // challenge started. enter_zone clears it, so it cannot be stale here,
    // and this checks anyway: it is the difference between reading a
    // neighbouring array and doing nothing, and it costs one compare.
    const Zone& z = zone_of(w);
    if (npc >= z.npc_count) { w.battle.trainer_npc = 0xFF; return; }
    const NpcDef& n = z.npcs[npc];
    const PartyEntry& p = k_parties[n.party_first];
    start_battle(w, make_mon(p.species, p.level), false, npc, n.reward);
}

void shop_tick(World& w, const Input& in) {
    const NpcDef* shop = shop_at(w);
    if (!shop) { w.mode = Mode::Overworld; return; }
    const int count = shop->stock_count;

    if (in.up_pressed) w.menu_cursor = uint8_t((w.menu_cursor + count - 1) % count);
    if (in.down_pressed) w.menu_cursor = uint8_t((w.menu_cursor + 1) % count);
    if (in.b_pressed || in.x_pressed) {
        w.mode = Mode::Overworld;
        w.talking_npc = 0xFF;
        w.sfx = Sfx::Cancel;
        return;
    }
    if (!in.a_pressed) return;

    const uint8_t item = k_stock[shop->stock_first + w.menu_cursor % count];
    const uint16_t price = k_items[item].price;
    // Three ways to fail, and all of them are silent refusals rather than a
    // message: the money on the counter says why, and a modal explaining
    // itself on a 120 pixel screen costs more than it teaches.
    if (w.money < price) { w.sfx = Sfx::Cancel; return; }
    if (bag_find(w, item) < 0 && w.bag_count >= k_max_bag) {
        w.sfx = Sfx::Cancel;
        return;
    }
    const int held = bag_find(w, item);
    if (held >= 0 && w.bag[held].count >= 99) { w.sfx = Sfx::Cancel; return; }

    w.money = uint16_t(w.money - price);
    bag_add(w, item, 1);
    w.sfx = Sfx::Buy;
    w.save_pending = true;
}

void bag_tick(World& w, const Input& in) {
    // Rebuilt every frame rather than cached: the bag is at most 24 entries
    // and a cached view is one more thing that can be stale.
    uint8_t slots[k_max_bag];
    uint8_t count = 0;
    for (int i = 0; i < w.bag_count; i++) {
        if (k_items[w.bag[i].item].pocket == w.menu_pocket) slots[count++] = uint8_t(i);
    }
    if (in.left_pressed) { w.menu_pocket = uint8_t((w.menu_pocket + 2) % 3); w.menu_cursor = 0; return; }
    if (in.right_pressed) { w.menu_pocket = uint8_t((w.menu_pocket + 1) % 3); w.menu_cursor = 0; return; }
    if (count) {
        if (in.up_pressed) w.menu_cursor = uint8_t((w.menu_cursor + count - 1) % count);
        if (in.down_pressed) w.menu_cursor = uint8_t((w.menu_cursor + 1) % count);
    }
    const bool in_battle = w.battle.foe.max_hp > 0 &&
                           w.battle.state != BattleState::Over;
    if (in.b_pressed || in.x_pressed) {
        w.mode = in_battle ? Mode::Battle : Mode::Overworld;
        w.sfx = Sfx::Cancel;
        return;
    }
    if (!in.a_pressed || count == 0) return;
    const uint8_t slot = slots[w.menu_cursor % count];
    const Item& it = k_items[w.bag[slot].item];

    switch (ItemEffect(it.effect)) {
        case ItemEffect::Ball: {
            if (!in_battle || !w.battle.wild) { w.sfx = Sfx::Cancel; return; }
            w.battle.ball_item = w.bag[slot].item;
            w.battle.wobbles = 0;
            w.battle.state = BattleState::Throw;
            w.battle.timer = 20;
            bag_take(w, slot);
            w.mode = Mode::Battle;
            w.sfx = Sfx::Throw;
            return;
        }
        case ItemEffect::Heal: {
            Mon& m = w.party[in_battle ? w.battle.active : 0];
            if (m.hp == 0 || m.hp == m.max_hp) { w.sfx = Sfx::Cancel; return; }
            const int healed = m.hp + it.param;
            m.hp = uint8_t(healed > m.max_hp ? m.max_hp : healed);
            bag_take(w, slot);
            w.sfx = Sfx::Heal;
            w.save_pending = true;
            return;
        }
        case ItemEffect::Revive: {
            for (int i = 0; i < w.party_count; i++) {
                if (w.party[i].hp != 0) continue;
                w.party[i].hp = uint8_t(w.party[i].max_hp / 2 + 1);
                bag_take(w, slot);
                w.sfx = Sfx::Heal;
                w.save_pending = true;
                return;
            }
            w.sfx = Sfx::Cancel;
            return;
        }
        default:
            w.sfx = Sfx::Cancel;
            return;
    }
}

void party_tick(World& w, const Input& in) {
    if (w.party_count) {
        if (in.up_pressed) w.menu_cursor = uint8_t((w.menu_cursor + w.party_count - 1) % w.party_count);
        if (in.down_pressed) w.menu_cursor = uint8_t((w.menu_cursor + 1) % w.party_count);
    }
    const bool in_battle = w.battle.foe.max_hp > 0 &&
                           w.battle.state != BattleState::Over;
    if (in.a_pressed && in_battle && w.party[w.menu_cursor].hp > 0 &&
        w.menu_cursor != w.battle.active) {
        w.battle.active = w.menu_cursor;
        w.battle.atk_stage = w.battle.def_stage = w.battle.spd_stage = 0;
        w.mode = Mode::Battle;
        w.battle.state = BattleState::Menu;
        w.sfx = Sfx::Select;
        return;
    }
    if (in.b_pressed || in.y_pressed) {
        w.mode = in_battle ? Mode::Battle : Mode::Overworld;
        w.sfx = Sfx::Cancel;
    }
}

}  // namespace

const NpcDef* shop_of(const World& w) { return shop_at(w); }

int bag_count_of(const World& w, uint8_t item) {
    const int at = bag_find(w, item);
    return at < 0 ? 0 : w.bag[at].count;
}

void world_tick(World& w, const Input& in) {
    w.sfx = Sfx::None;
    switch (w.mode) {
        case Mode::Title:
            if (in.a_pressed || in.b_pressed) { w.mode = Mode::Overworld; w.sfx = Sfx::Select; }
            return;
        case Mode::Overworld: overworld_tick(w, in); return;
        case Mode::Dialogue: dialogue_tick(w, in); return;
        case Mode::Battle: battle_tick(w, in); return;
        case Mode::Bag: bag_tick(w, in); return;
        case Mode::Party: party_tick(w, in); return;
        case Mode::Shop: shop_tick(w, in); return;
        case Mode::Faded: w.mode = Mode::Overworld; return;
    }
}

}  // namespace pm
