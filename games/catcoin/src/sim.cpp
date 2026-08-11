#include "sim.hpp"

namespace cc {

uint32_t g_pair_tests = 0;

namespace {

uint32_t isqrt(uint64_t value) {
    if (value == 0) return 0;
    uint64_t guess = value;
    uint64_t next = (guess + 1) / 2;
    while (next < guess) {
        guess = next;
        next = (guess + value / guess) / 2;
    }
    return static_cast<uint32_t>(guess);
}

// Q16.16 multiply. The intermediate has to be 64 bit: two Q16.16 values
// multiplied is Q32.32, and doing it in 32 bits overflows at a velocity of
// one pixel a tick, which every coin the pusher shoves exceeds.
int32_t fmul(int32_t a, int32_t b) {
    return static_cast<int32_t>((static_cast<int64_t>(a) * b) >> k_fp);
}

const int32_t k_spc_cost[k_num_specials] = {30, 25, 40, 20, 15, 35, 30, 25, 20, 45, 50};

void add_particle(World& w, int32_t x, int32_t y, uint8_t colour, int life) {
    if (w.particle_count >= k_max_particles) return;
    Particle& p = w.particles[w.particle_count++];
    p.x = x;
    p.y = y;
    p.vx = static_cast<int32_t>(rand_below(w, 2 * k_one)) - k_one;
    p.vy = static_cast<int32_t>(rand_below(w, 2 * k_one)) - k_one;
    p.life = static_cast<uint8_t>(life);
    p.max_life = static_cast<uint8_t>(life);
    p.colour = colour;
}

void add_popup(World& w, int32_t x, int32_t y, int32_t value) {
    if (w.popup_count >= k_max_popups) return;
    Popup& p = w.popups[w.popup_count++];
    p.x = x;
    p.y = y;
    p.value = value;
    p.timer = 75;
}

Coin* add_coin(World& w, int32_t x, int32_t y, uint8_t stype) {
    if (w.coin_count >= k_max_coins) return nullptr;
    Coin& c = w.coins[w.coin_count++];
    c.x = x;
    c.y = y;
    c.vx = 0;
    c.vy = 0;
    c.fuse = 0;
    c.stype = stype;
    c.flags = 0;
    return &c;
}

void reset_round_state(World& w) {
    w.round_score = 0;
    w.combo = 0;
    w.combo_timer = 0;
    w.combo_best = 0;
    w.target = target_for(w.round);
    // The cart's 30 * 1.3^(round-1), in integers.
    int32_t left = 30 * 1000;
    for (int i = 1; i < w.round; i++) left = left * 13 / 10;
    w.coins_left = left / 1000;
    w.score_per_gold = (w.target + 99) / 100;
    if (w.score_per_gold < 1) w.score_per_gold = 1;
    w.round_gold_given = 0;
    w.dropping_count = 0;
    w.falling_count = 0;
    w.particle_count = 0;
    w.popup_count = 0;
    w.buy_count = 0;
    w.buy_cost = k_buy_coin_base;
    w.spinner_pending = false;
    w.sel = 0;
}

// Seeding.
//
// A jittered hexagonal lattice taken in random order, rather than the cart's
// rejection sampler. Same loose carpet, but the count asked for is the count
// placed, so the game can be balanced against a real number. See sim.hpp.
struct Site {
    int16_t x, y;
};

int lattice(Site* out, int max_sites) {
    const int sx = k_coin_d + 1;
    const int sy = 9;                       // 1.8 coin radii, the cart's row pitch
    int n = 0;
    int row = 0;
    for (int y = k_push_max + k_coin_r + 2; y <= k_fbot - k_coin_r - 1; y += sy, row++) {
        const int offset = (row & 1) ? sx / 2 : 0;
        for (int x = k_fl + k_coin_r + 1 + offset; x <= k_fr - k_coin_r - 1; x += sx) {
            if (n >= max_sites) return n;
            out[n].x = static_cast<int16_t>(x);
            out[n].y = static_cast<int16_t>(y);
            n++;
        }
    }
    return n;
}

void seed_coins(World& w, int wanted) {
    Site sites[128];
    int n = lattice(sites, 128);
    for (int i = n - 1; i > 0; i--) {
        const int j = static_cast<int>(rand_below(w, static_cast<uint32_t>(i + 1)));
        const Site t = sites[i];
        sites[i] = sites[j];
        sites[j] = t;
    }
    if (wanted > n) wanted = n;
    for (int i = 0; i < wanted; i++) {
        const int32_t jx = static_cast<int32_t>(rand_below(w, 2 * k_one)) - k_one;
        const int32_t jy = static_cast<int32_t>(rand_below(w, 2 * k_one)) - k_one;
        add_coin(w, sites[i].x * k_one + jx, sites[i].y * k_one + jy, spc_none);
    }
}

void start_round(World& w, bool seed_field) {
    w.state = State::play;
    w.ticks = 0;
    reset_round_state(w);
    if (seed_field) {
        w.coin_count = 0;
        w.push_y = (k_push_min + 8) * k_one;
        w.push_dir = 1;
        w.push_frozen = 0;
        w.disp_x = ((k_fl + k_fr) / 2) * k_one;
        w.disp_dir = 1;
        seed_coins(w, seed_count(w.round));
    }
}

void start_run(World& w) {
    w.round = 1;
    w.gold = 0;
    w.inv_count = 0;
    w.score_mult = 1;
    w.mult_timer = 0;
    w.combo_mult = 1;
    w.combo_buff_timer = 0;
    start_round(w, true);
}

void game_over(World& w) {
    w.state = State::over;
    w.ticks = 0;
    int32_t total = 0;
    for (int r = 1; r < w.round; r++) total += target_for(r);
    total += w.round_score;
    if (total > w.high_score) w.high_score = total;
}

void game_win(World& w) {
    w.state = State::win;
    w.ticks = 0;
    int32_t total = 0;
    for (int r = 1; r <= k_max_rounds; r++) total += target_for(r);
    total += w.round_score - w.target;
    if (total > w.high_score) w.high_score = total;
}

void gen_shop(World& w) {
    bool used[k_num_specials + 1] = {false};
    for (int i = 0; i < 3; i++) {
        uint8_t st;
        do {
            st = static_cast<uint8_t>(rand_below(w, k_num_specials) + 1);
        } while (used[st]);
        used[st] = true;
        w.shop[i].stype = st;
        w.shop[i].cost = static_cast<uint16_t>(k_spc_cost[st - 1] + w.round * 5);
        w.shop[i].sold = false;
    }
}

void open_shop(World& w) {
    w.state = State::shop;
    w.ticks = 0;
    w.shop_sel = 0;
    w.refresh_cost = 10 + w.round * 5;
    gen_shop(w);
}

void open_spinner(World& w, int combo_mult) {
    w.state = State::spinner;
    w.ticks = 0;
    // Capped, and this is a deviation from the cart worth stating.
    //
    // A combo only ends when 1.5 seconds pass with nothing crossing the lip,
    // and under sustained dropping nothing ever does: measured, the counter
    // reaches 1448 over two minutes of continuous play. The cart has the same
    // rule and the same runaway, and it fed the raw count straight into the
    // prize as "how many times over the threshold", which at 1448 is a
    // multiplier of 289 and a prize of 4335 coins. Scoring is unaffected, the
    // cart already capped that at eight; this caps only the prize.
    int m = combo_mult < 1 ? 1 : combo_mult;
    if (m > k_spin_mult_cap) m = k_spin_mult_cap;
    w.spin_mult = static_cast<uint8_t>(m);
    w.spin[0].kind = 0;
    w.spin[0].amount = static_cast<uint16_t>(15 * m);
    w.spin[0].stype = 0;
    if (rand_below(w, 2) == 0) {
        w.spin[1].kind = 1;
        w.spin[1].amount = static_cast<uint16_t>(10 * m);
    } else {
        w.spin[1].kind = 0;
        w.spin[1].amount = static_cast<uint16_t>(10 * m);
    }
    w.spin[1].stype = 0;
    w.spin[2].kind = 2;
    w.spin[2].amount = 0;
    w.spin[2].stype = static_cast<uint8_t>(rand_below(w, k_num_specials) + 1);
    for (int i = 2; i > 0; i--) {
        const int j = static_cast<int>(rand_below(w, static_cast<uint32_t>(i + 1)));
        const SpinItem t = w.spin[i];
        w.spin[i] = w.spin[j];
        w.spin[j] = t;
    }
    w.spin_pos = 0;
    w.spin_speed = fx(0.09) + static_cast<int32_t>(rand_below(w, fx(0.03)));
    w.spin_timer = static_cast<int16_t>(150 + rand_below(w, 67));
    w.spin_result = -1;
    w.spin_done = false;
}

void finish_round(World& w) {
    if (w.round_score >= w.target) {
        if (w.round >= k_max_rounds) game_win(w);
        else open_shop(w);
    } else {
        game_over(w);
    }
}

// The eleven specials. Every radius scaled by 1.875, every impulse by 1.125,
// every timer by 100/60, and none of them retuned.
void activate(World& w, Coin& c) {
    if ((c.flags & k_activated) || c.stype == spc_none) return;
    c.flags |= k_activated;
    w.flash = 6;

    switch (c.stype) {
        case spc_bomb: {
            const int32_t r = fx(28 * 1.875);
            for (uint16_t i = 0; i < w.coin_count; i++) {
                Coin& o = w.coins[i];
                if (&o == &c) continue;
                const int32_t dx = o.x - c.x, dy = o.y - c.y;
                const int64_t dd = static_cast<int64_t>(dx) * dx + static_cast<int64_t>(dy) * dy;
                if (dd >= static_cast<int64_t>(r) * r) continue;
                const int32_t d = static_cast<int32_t>(isqrt(static_cast<uint64_t>(dd))) + 1;
                o.vx += static_cast<int32_t>((static_cast<int64_t>(dx) * fx(4 * 1.125)) / d);
                o.vy += static_cast<int32_t>((static_cast<int64_t>(dy) * fx(4 * 1.125)) / d);
            }
            for (int i = 0; i < 8; i++) {
                add_particle(w, c.x, c.y, 8, 42);
                add_particle(w, c.x, c.y, 10, 33);
            }
            break;
        }
        case spc_whirl: {
            const int32_t r = fx(25 * 1.875);
            for (uint16_t i = 0; i < w.coin_count; i++) {
                Coin& o = w.coins[i];
                if (&o == &c) continue;
                const int32_t dx = o.x - c.x, dy = o.y - c.y;
                const int64_t dd = static_cast<int64_t>(dx) * dx + static_cast<int64_t>(dy) * dy;
                if (dd >= static_cast<int64_t>(r) * r) continue;
                const int32_t d = static_cast<int32_t>(isqrt(static_cast<uint64_t>(dd))) + 1;
                // A quarter turn of the offset, which is a swap and a negate:
                // the cart's atan2 plus 0.25 turns, without the trig.
                const int32_t tx = -dy, ty = dx;
                o.vx += static_cast<int32_t>((static_cast<int64_t>(tx) * fx(1.5 * 1.125)) / d);
                o.vy += static_cast<int32_t>((static_cast<int64_t>(ty) * fx(1.5 * 1.125)) / d) +
                        fx(0.5 * 1.125);
            }
            for (int i = 0; i < 6; i++) add_particle(w, c.x, c.y, 12, 42);
            break;
        }
        case spc_hole:
        case spc_magnet: {
            const bool hole = c.stype == spc_hole;
            const int32_t r = hole ? fx(30 * 1.875) : fx(22 * 1.875);
            const int32_t pull = hole ? fx(2 * 1.125) : fx(1.5 * 1.125);
            for (uint16_t i = 0; i < w.coin_count; i++) {
                Coin& o = w.coins[i];
                if (&o == &c) continue;
                const int32_t dx = c.x - o.x, dy = c.y - o.y;
                const int64_t dd = static_cast<int64_t>(dx) * dx + static_cast<int64_t>(dy) * dy;
                if (dd >= static_cast<int64_t>(r) * r) continue;
                const int32_t d = static_cast<int32_t>(isqrt(static_cast<uint64_t>(dd))) + 1;
                o.vx += static_cast<int32_t>((static_cast<int64_t>(dx) * pull) / d);
                o.vy += static_cast<int32_t>((static_cast<int64_t>(dy) * pull) / d);
            }
            for (int i = 0; i < 5; i++) add_particle(w, c.x, c.y, hole ? 1 : 12, 30);
            break;
        }
        case spc_gold:
            break;                                  // passive, worth 50 when it scores
        case spc_multi:
            w.score_mult = 2;
            w.mult_timer = k_mult_time;
            for (int i = 0; i < 4; i++) add_particle(w, c.x, c.y, 9, 33);
            break;
        case spc_quake:
            for (uint16_t i = 0; i < w.coin_count; i++) {
                Coin& o = w.coins[i];
                o.vx += static_cast<int32_t>(rand_below(w, 2 * fx(1.125))) - fx(1.125);
                o.vy += static_cast<int32_t>(rand_below(w, 2 * fx(1.125))) - fx(1.125);
            }
            for (int i = 0; i < 5; i++) add_particle(w, c.x, c.y, 4, 25);
            break;
        case spc_ice:
            w.push_frozen = k_ice_time;
            for (int i = 0; i < 5; i++) {
                add_particle(w, c.x, c.y, 12, 33);
                add_particle(w, c.x, c.y, 7, 25);
            }
            break;
        case spc_clone:
            for (int i = 0; i < 5; i++) {
                Coin* nc = add_coin(w,
                                    c.x + static_cast<int32_t>(rand_below(w, fx(10 * 1.875))) -
                                        fx(5 * 1.875),
                                    c.y + static_cast<int32_t>(rand_below(w, fx(10 * 1.875))) -
                                        fx(5 * 1.875),
                                    spc_none);
                if (nc == nullptr) break;
                nc->vy = fx(0.5 * 1.125) + static_cast<int32_t>(rand_below(w, fx(0.5 * 1.125)));
                nc->vx = static_cast<int32_t>(rand_below(w, fx(1.125))) - fx(0.5 * 1.125);
            }
            for (int i = 0; i < 4; i++) add_particle(w, c.x, c.y, 11, 25);
            break;
        case spc_crown:
            w.combo_mult = 3;
            w.combo_buff_timer = k_crown_time;
            for (int i = 0; i < 5; i++) add_particle(w, c.x, c.y, 10, 33);
            break;
        case spc_cash: {
            // Teleport everything nearby past the scoring edge, which is how
            // the cart did it: they score on the next check like any coin.
            const int32_t r = fx(22 * 1.875);
            for (uint16_t i = 0; i < w.coin_count; i++) {
                Coin& o = w.coins[i];
                if (&o == &c) continue;
                const int32_t dx = c.x - o.x, dy = c.y - o.y;
                const int64_t dd = static_cast<int64_t>(dx) * dx + static_cast<int64_t>(dy) * dy;
                if (dd >= static_cast<int64_t>(r) * r) continue;
                o.y = (k_fbot + k_coin_r + 1) * k_one;
                o.flags &= static_cast<uint8_t>(~k_on_pusher);
                add_particle(w, o.x, o.y - 5 * k_one, 14, 33);
            }
            for (int i = 0; i < 6; i++) {
                add_particle(w, c.x, c.y, 14, 42);
                add_particle(w, c.x, c.y, 7, 33);
            }
            break;
        }
        default:
            break;
    }
}

void trigger_all_fused(World& w) {
    for (uint16_t i = 0; i < w.coin_count; i++) {
        Coin& c = w.coins[i];
        if (c.stype != spc_none && c.fuse > 0 && !(c.flags & k_activated)) {
            c.fuse = 0;
            activate(w, c);
        }
    }
}

void update_pusher(World& w) {
    const int32_t prev = w.push_y;
    if (w.push_frozen > 0) {
        w.push_frozen--;
        w.push_y = k_push_max * k_one;
    } else {
        w.push_y += w.push_dir * k_push_speed;
        if (w.push_y >= k_push_max * k_one) {
            w.push_y = k_push_max * k_one;
            w.push_dir = -1;
        }
        if (w.push_y <= k_push_min * k_one) {
            w.push_y = k_push_min * k_one;
            w.push_dir = 1;
        }
    }
    const int32_t dy = w.push_y - prev;
    const int32_t ptop = w.push_y - k_push_h * k_one;
    const int32_t r = k_coin_r * k_one;

    for (uint16_t i = 0; i < w.coin_count; i++) {
        Coin& c = w.coins[i];
        if (c.flags & k_on_pusher) {
            c.y += dy;
            if (w.push_dir == -1 && c.y - r <= k_ft * k_one) {
                c.y = k_ft * k_one + r;
                c.vy = fx(0.5 * 1.125) + static_cast<int32_t>(rand_below(w, fx(0.3 * 1.125)));
                c.vx = static_cast<int32_t>(rand_below(w, fx(0.3 * 1.125))) - fx(0.15 * 1.125);
                c.flags &= static_cast<uint8_t>(~k_on_pusher);
            }
            if (c.y + r > w.push_y) {
                c.flags &= static_cast<uint8_t>(~k_on_pusher);
                c.y = w.push_y + r;
                c.vy = fx(0.3 * 1.125);
            }
            if (c.y - r < ptop) {
                c.flags &= static_cast<uint8_t>(~k_on_pusher);
                c.y = ptop - r;
            }
        } else {
            if (w.push_dir == 1 && dy > 0) {
                if (c.y - r < w.push_y && c.y + r > w.push_y - fx(4 * 1.875) &&
                    c.x > k_fl * k_one && c.x < k_fr * k_one) {
                    c.vy += (dy * 8) / 10;
                    c.y = w.push_y + r;
                    c.vx += static_cast<int32_t>(rand_below(w, fx(0.2 * 1.125))) - fx(0.1 * 1.125);
                }
            }
            if (c.y > ptop && c.y < w.push_y && c.x > k_fl * k_one && c.x < k_fr * k_one) {
                const int32_t avy = c.vy < 0 ? -c.vy : c.vy;
                if (avy < fx(0.3 * 1.125)) {
                    c.flags |= k_on_pusher;
                    c.vx /= 2;
                    c.vy = 0;
                }
            }
        }
    }
}

void collide(Coin& a, Coin& b) {
    const int32_t dx = b.x - a.x, dy = b.y - a.y;
    const int32_t m = k_coin_d * k_one;
    const int64_t dd = static_cast<int64_t>(dx) * dx + static_cast<int64_t>(dy) * dy;
    // Reject on the square before any square root, which is what the cart did
    // and what makes the pair count affordable.
    if (dd >= static_cast<int64_t>(m) * m || dd <= 1024) return;
    const int32_t d = static_cast<int32_t>(isqrt(static_cast<uint64_t>(dd)));
    if (d == 0) return;
    const int32_t overlap = m - d;
    const int32_t nx = static_cast<int32_t>((static_cast<int64_t>(dx) << k_fp) / d);
    const int32_t ny = static_cast<int32_t>((static_cast<int64_t>(dy) << k_fp) / d);
    const int32_t push_x = fmul(nx, overlap) / 2;
    const int32_t push_y = fmul(ny, overlap) / 2;
    a.x -= push_x;
    a.y -= push_y;
    b.x += push_x;
    b.y += push_y;
    const int32_t dot = fmul(a.vx - b.vx, nx) + fmul(a.vy - b.vy, ny);
    if (dot > 0) {
        const int32_t j = (dot * 45) / 100;
        a.vx -= fmul(nx, j);
        a.vy -= fmul(ny, j);
        b.vx += fmul(nx, j);
        b.vy += fmul(ny, j);
    }
}

void update_coins(World& w) {
    const int32_t r = k_coin_r * k_one;
    const int32_t ptop = w.push_y - k_push_h * k_one;
    const int32_t pmid = w.push_y - (k_push_h / 2) * k_one;

    for (uint16_t i = 0; i < w.coin_count; i++) {
        Coin& c = w.coins[i];
        if (c.flags & k_on_pusher) continue;
        c.x += c.vx;
        c.y += c.vy;
        c.vx = fmul(c.vx, k_coin_damp);
        c.vy = fmul(c.vy, k_coin_damp);
        if (c.x < k_fl * k_one + r) {
            c.x = k_fl * k_one + r;
            c.vx = (c.vx < 0 ? -c.vx : c.vx) * 3 / 10;
        }
        if (c.x > k_fr * k_one - r) {
            c.x = k_fr * k_one - r;
            c.vx = -((c.vx < 0 ? -c.vx : c.vx) * 3 / 10);
        }
        if (c.y < k_ft * k_one + r) {
            c.y = k_ft * k_one + r;
            c.vy = (c.vy < 0 ? -c.vy : c.vy) * 3 / 10;
        }
        if (c.y - r < w.push_y && c.y + r > ptop && c.x > k_fl * k_one &&
            c.x < k_fr * k_one) {
            if (c.y > pmid) {
                c.y = w.push_y + r;
                if (c.vy < fx(0.1 * 1.125)) c.vy = fx(0.1 * 1.125);
            }
        }
    }

    // Spatial hash: same cell plus four forward neighbours. A fixed array with
    // a head index per cell and a next index per coin, so nothing allocates.
    for (int i = 0; i < k_grid_cells; i++) w.grid_head[i] = -1;
    for (uint16_t i = 0; i < w.coin_count; i++) {
        const Coin& c = w.coins[i];
        int gx = ((c.x >> k_fp) - k_fl) / k_cell;
        int gy = ((c.y >> k_fp) - k_ft) / k_cell;
        if (gx < 0) gx = 0;
        if (gx >= k_grid_w) gx = k_grid_w - 1;
        if (gy < 0) gy = 0;
        if (gy >= k_grid_h) gy = k_grid_h - 1;
        const int k = gy * k_grid_w + gx;
        w.grid_next[i] = w.grid_head[k];
        w.grid_head[k] = static_cast<int16_t>(i);
    }
    const int neighbours[4] = {1, k_grid_w - 1, k_grid_w, k_grid_w + 1};
    for (int gy = 0; gy < k_grid_h; gy++) {
        for (int gx = 0; gx < k_grid_w; gx++) {
            const int k = gy * k_grid_w + gx;
            for (int16_t i = w.grid_head[k]; i >= 0; i = w.grid_next[i]) {
                for (int16_t j = w.grid_next[i]; j >= 0; j = w.grid_next[j]) {
                    g_pair_tests++;
                    collide(w.coins[i], w.coins[j]);
                }
                for (int n = 0; n < 4; n++) {
                    const int nk = k + neighbours[n];
                    if (nk >= k_grid_cells) continue;
                    const int nx = nk % k_grid_w;
                    if (nx - gx > 1 || gx - nx > 1) continue;
                    for (int16_t j = w.grid_head[nk]; j >= 0; j = w.grid_next[j]) {
                        g_pair_tests++;
                        collide(w.coins[i], w.coins[j]);
                    }
                }
            }
        }
    }
}

void check_scored(World& w) {
    for (int i = static_cast<int>(w.coin_count) - 1; i >= 0; i--) {
        Coin& c = w.coins[i];
        if ((c.flags & k_on_pusher) || c.y <= k_fbot * k_one) continue;

        int32_t value = (c.stype == spc_gold) ? 50 : 10;
        w.combo++;
        w.combo_timer = k_combo_hold;
        if (w.combo > w.combo_best) w.combo_best = w.combo;
        const int32_t cmult = (w.combo > 8 ? 8 : w.combo) * w.combo_mult;
        if (w.combo > 1) value *= cmult;
        value *= w.score_mult;
        w.round_score += value;
        const int32_t new_gold = w.round_score / w.score_per_gold;
        const int32_t delta = new_gold - w.round_gold_given;
        if (delta > 0) {
            w.gold += delta;
            w.round_gold_given = new_gold;
        }
        add_popup(w, c.x, (k_fbot - 10) * k_one, value);
        if (w.falling_count < k_max_falling) {
            Falling& f = w.falling[w.falling_count++];
            f.x = c.x;
            f.y = (k_fbot + 3) * k_one;
            f.vy = fx(0.8 * 1.125) + static_cast<int32_t>(rand_below(w, fx(0.5 * 1.125)));
            f.vx = static_cast<int32_t>(rand_below(w, fx(0.6 * 1.125))) - fx(0.3 * 1.125);
            f.h = 0;
            f.vh = fx(0.3 * 1.125);
            f.life = 33;
        }
        for (int k = 0; k < 3; k++) {
            add_particle(w, c.x, k_fbot * k_one, c.stype > 0 ? 14 : 9, 30);
        }
        w.coins[i] = w.coins[w.coin_count - 1];
        w.coin_count--;
    }
}

void update_shop(World& w, const Input& in) {
    if (in.up_pressed && w.shop_sel > 0) w.shop_sel--;
    if (in.down_pressed && w.shop_sel < 4) w.shop_sel++;
    if (!(in.drop_pressed || in.use_pressed)) return;

    if (w.shop_sel <= 2) {
        ShopItem& item = w.shop[w.shop_sel];
        if (!item.sold && w.gold >= item.cost && w.inv_count < k_inv_max) {
            w.gold -= item.cost;
            w.inv[w.inv_count++] = item.stype;
            item.sold = true;
        }
    } else if (w.shop_sel == 3) {
        if (w.gold >= w.refresh_cost) {
            w.gold -= w.refresh_cost;
            w.refresh_cost = w.refresh_cost * 3 / 2;
            gen_shop(w);
        }
    } else {
        w.round++;
        start_round(w, false);
    }
}

void update_spinner(World& w, const Input& in) {
    if (!w.spin_done) {
        w.spin_timer--;
        w.spin_pos += w.spin_speed;
        if (w.spin_pos >= 3 * k_one) w.spin_pos = 0;
        if (w.spin_timer < 67) w.spin_speed = (w.spin_speed * 97) / 100;
        if (w.spin_timer <= 0 || w.spin_speed < fx(0.012)) {
            w.spin_done = true;
            int result = w.spin_pos >> k_fp;
            if (result < 0) result = 0;
            if (result > 2) result = 2;
            w.spin_result = static_cast<int8_t>(result);
            const SpinItem& prize = w.spin[result];
            if (prize.kind == 0) {
                w.coins_left += prize.amount;
            } else if (prize.kind == 1) {
                w.gold += prize.amount;
            } else if (w.inv_count < k_inv_max) {
                w.inv[w.inv_count++] = prize.stype;
            } else {
                w.gold += 20;
            }
        }
    } else if (in.any_pressed) {
        w.state = State::play;
    }
}

void update_play(World& w, const Input& in) {
    // The dispenser tracks on its own, as the cart's did.
    w.disp_x += w.disp_dir * k_disp_speed;
    if (w.disp_x >= (k_fr - 12) * k_one) {
        w.disp_dir = -1;
        w.disp_x = (k_fr - 12) * k_one;
    }
    if (w.disp_x <= (k_fl + 12) * k_one) {
        w.disp_dir = 1;
        w.disp_x = (k_fl + 12) * k_one;
    }

    Slot slots[k_inv_max + 2];
    int slot_count = build_slots(w, slots, k_inv_max + 2);
    if (w.sel >= slot_count) w.sel = static_cast<uint8_t>(slot_count > 0 ? slot_count - 1 : 0);
    if (in.left_pressed && w.sel > 0) w.sel--;
    if (in.right_pressed && w.sel + 1 < slot_count) w.sel++;

    // A drops a coin. That is the verb of the game, so it keeps its own button.
    if (in.drop_pressed && w.coins_left > 0 && w.dropping_count < k_max_dropping) {
        Dropping& d = w.dropping[w.dropping_count++];
        d.x = w.disp_x;
        d.y = (k_ft - 8) * k_one;
        d.target_y = k_ft * k_one + fx(4 * 1.875) +
                     static_cast<int32_t>(rand_below(w, fx(4 * 1.875)));
        d.h = fx(8 * 1.875);
        d.timer = k_drop_time;
        d.stype = spc_none;
        w.coins_left--;
    }

    // B uses whatever the row has selected.
    if (in.use_pressed && slot_count > 0) {
        const Slot& s = slots[w.sel];
        if (s.kind == SlotKind::end) {
            finish_round(w);
            return;
        }
        if (s.kind == SlotKind::buy) {
            if (w.gold >= w.buy_cost) {
                w.gold -= w.buy_cost;
                w.buy_count++;
                w.buy_cost = k_buy_coin_base + w.buy_count * k_buy_coin_base;
                w.coins_left += k_buy_coin_amount;
                trigger_all_fused(w);
            }
        } else if (s.kind == SlotKind::item && w.dropping_count < k_max_dropping) {
            Dropping& d = w.dropping[w.dropping_count++];
            d.x = w.disp_x;
            d.y = (k_ft - 8) * k_one;
            d.target_y = k_ft * k_one + fx(4 * 1.875) +
                         static_cast<int32_t>(rand_below(w, fx(4 * 1.875)));
            d.h = fx(8 * 1.875);
            d.timer = k_drop_time;
            d.stype = w.inv[s.index];
            for (int i = s.index; i + 1 < w.inv_count; i++) w.inv[i] = w.inv[i + 1];
            w.inv_count--;
            slot_count = build_slots(w, slots, k_inv_max + 2);
            if (w.sel >= slot_count) {
                w.sel = static_cast<uint8_t>(slot_count > 0 ? slot_count - 1 : 0);
            }
        }
    }

    for (int i = static_cast<int>(w.dropping_count) - 1; i >= 0; i--) {
        Dropping& d = w.dropping[i];
        d.y += fmul(d.target_y - d.y, k_drop_lerp);
        d.h = fmul(d.h, k_drop_shrink);
        d.timer--;
        if (d.timer == 0) {
            Coin* c = add_coin(w, d.x, d.target_y, d.stype);
            if (c != nullptr) {
                if (d.target_y < w.push_y && d.target_y > w.push_y - k_push_h * k_one) {
                    c->flags |= k_on_pusher;
                }
                // Specials get the cart's two second fuse. The goldfish is
                // passive, so it never gets one.
                if (c->stype != spc_none && c->stype != spc_gold) c->fuse = k_fuse;
            }
            w.dropping[i] = w.dropping[w.dropping_count - 1];
            w.dropping_count--;
        }
    }

    for (uint16_t i = 0; i < w.coin_count; i++) {
        Coin& c = w.coins[i];
        if (c.fuse > 0) {
            c.fuse--;
            if (c.fuse == 0) activate(w, c);
        }
    }

    if (w.mult_timer > 0 && --w.mult_timer == 0) w.score_mult = 1;
    if (w.combo_buff_timer > 0 && --w.combo_buff_timer == 0) w.combo_mult = 1;

    update_pusher(w);
    update_coins(w);
    check_scored(w);

    if (w.combo_timer > 0) {
        w.combo_timer--;
        if (w.combo_timer == 0) {
            if (w.combo >= k_combo_threshold && !w.spinner_pending) {
                w.spinner_pending = true;
                open_spinner(w, w.combo / k_combo_threshold);
                w.combo = 0;
                return;
            }
            w.combo = 0;
        }
    }
}

}  // namespace

uint32_t rand_next(World& w) {
    w.rng = w.rng * 1664525u + 1013904223u;
    return w.rng;
}

uint32_t rand_below(World& w, uint32_t bound) {
    if (bound == 0) return 0;
    return (rand_next(w) >> 8) % bound;
}

int32_t target_for(int round) {
    // The cart's 150 * 1.5^(round-1), in integers.
    int32_t t = 150 * 1000;
    for (int i = 1; i < round; i++) t = t * 3 / 2;
    return t / 1000;
}

bool can_end_round(const World& w) {
    return w.round_score >= w.target || (w.coins_left <= 0 && w.dropping_count == 0);
}

int build_slots(const World& w, Slot* out, int max_slots) {
    int n = 0;
    for (int i = 0; i < w.inv_count && n < max_slots; i++) {
        out[n].kind = SlotKind::item;
        out[n].index = static_cast<uint8_t>(i);
        n++;
    }
    if (n < max_slots) {
        out[n].kind = SlotKind::buy;
        out[n].index = 0;
        n++;
    }
    if (can_end_round(w) && n < max_slots) {
        out[n].kind = SlotKind::end;
        out[n].index = 0;
        n++;
    }
    return n;
}

int seed_capacity() {
    Site sites[128];
    return lattice(sites, 128);
}

int seed_count(int round) {
    // Two thirds of the shelf at round 1, a full shelf by round 10, which is
    // the curve the cart's saturating sampler actually produced.
    const int capacity = seed_capacity();
    int permille = 660 + 38 * (round - 1);
    if (permille > 1000) permille = 1000;
    return capacity * permille / 1000;
}

void world_init(World& w, uint32_t seed) {
    w.rng = seed ? seed : 1u;
    w.ticks = 0;
    w.coin_count = 0;
    w.dropping_count = 0;
    w.falling_count = 0;
    w.particle_count = 0;
    w.popup_count = 0;
    w.push_y = (k_push_min + 8) * k_one;
    w.push_dir = 1;
    w.push_frozen = 0;
    w.disp_x = ((k_fl + k_fr) / 2) * k_one;
    w.disp_dir = 1;
    w.round = 1;
    w.round_score = 0;
    w.target = target_for(1);
    w.gold = 0;
    w.coins_left = 0;
    w.score_per_gold = 1;
    w.round_gold_given = 0;
    w.score_mult = 1;
    w.mult_timer = 0;
    w.combo_mult = 1;
    w.combo_buff_timer = 0;
    w.combo = 0;
    w.combo_timer = 0;
    w.combo_best = 0;
    w.spinner_pending = false;
    w.inv_count = 0;
    w.sel = 0;
    w.buy_count = 0;
    w.buy_cost = k_buy_coin_base;
    w.shop_sel = 0;
    w.refresh_cost = 10;
    w.spin_mult = 1;
    w.spin_pos = 0;
    w.spin_speed = 0;
    w.spin_timer = 0;
    w.spin_result = -1;
    w.spin_done = false;
    w.cat_blink = 0;
    w.cat_twitch = 0;
    w.flash = 0;
    w.high_score = 0;
    w.state = State::title;
}

void world_tick(World& w, const Input& in) {
    w.ticks++;
    if (w.flash > 0) w.flash--;
    if (rand_below(w, 333) == 0) w.cat_blink = 17;
    if (w.cat_blink > 0) w.cat_blink--;
    if (rand_below(w, 500) == 0) w.cat_twitch = 25;
    if (w.cat_twitch > 0) w.cat_twitch--;

    switch (w.state) {
        case State::title:
            w.disp_x += w.disp_dir * fx(0.5 * 1.875 * 0.6);
            if (w.disp_x > (k_fr - 12) * k_one) w.disp_dir = -1;
            if (w.disp_x < (k_fl + 12) * k_one) w.disp_dir = 1;
            if (in.any_pressed) start_run(w);
            break;
        case State::play: update_play(w, in); break;
        case State::shop: update_shop(w, in); break;
        case State::spinner: update_spinner(w, in); break;
        case State::over:
        case State::win:
            if (w.ticks > 100 && in.any_pressed) {
                const int32_t high = w.high_score;
                world_init(w, w.rng);
                w.high_score = high;
            }
            break;
    }

    for (int i = static_cast<int>(w.particle_count) - 1; i >= 0; i--) {
        Particle& p = w.particles[i];
        p.x += p.vx;
        p.y += p.vy;
        p.vy += k_particle_gravity;
        if (--p.life == 0) {
            w.particles[i] = w.particles[w.particle_count - 1];
            w.particle_count--;
        }
    }
    for (int i = static_cast<int>(w.popup_count) - 1; i >= 0; i--) {
        Popup& p = w.popups[i];
        p.y -= fx(0.6 * 1.125);
        if (--p.timer == 0) {
            w.popups[i] = w.popups[w.popup_count - 1];
            w.popup_count--;
        }
    }
    for (int i = static_cast<int>(w.falling_count) - 1; i >= 0; i--) {
        Falling& f = w.falling[i];
        f.y += f.vy;
        f.x += f.vx;
        f.vy += fx(0.08 * 1.875 * 0.36);
        f.h += f.vh;
        f.vh += fx(0.15 * 1.875 * 0.36);
        if (--f.life == 0) {
            w.falling[i] = w.falling[w.falling_count - 1];
            w.falling_count--;
        }
    }
}

}  // namespace cc
