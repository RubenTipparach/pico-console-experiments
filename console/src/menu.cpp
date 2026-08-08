#include "menu.hpp"

#include <cstddef>

#include "pse/text.hpp"

namespace console {
namespace {

// Laid out for the 240x240 hires screen the console puts the menu in. Every
// draw call clips, so a smaller target loses the bottom of the list rather
// than writing off the end of it.
constexpr int k_header_h = 22;
constexpr int k_list_top = 24;
constexpr int k_row_h = 28;
constexpr int k_icon_x = 8;
constexpr int k_name_x = 38;
constexpr int k_name_scale = 2;
constexpr int k_scrollbar_x = 233;
constexpr int k_scrollbar_w = 3;
constexpr int k_title_x = 8;

// Where you left off, marked at the right end of its row.
constexpr int k_dot_x = k_scrollbar_x - 12;
constexpr int k_dot_w = 4;

// Room a row's name has before it reaches that dot. A wider name is drawn
// sliding, not refused: see marquee_offset. The dot's column is reserved even
// on rows that do not have one, because a name that slides is regularly as
// wide as this allows, and a name and a dot sharing pixels reads as neither.
constexpr int k_name_room = k_dot_x - 6 - k_name_x;
static_assert(k_name_room == Menu::k_name_room,
              "menu.hpp publishes this number to the tests and to "
              "tools/gen_library.py, so the two have to agree");

// The battery sits at the right end of the header, its right edge lined up
// with the scrollbar's so the screen has one right margin.
constexpr int k_batt_w = 20;
constexpr int k_batt_h = 13;
constexpr int k_batt_x = k_scrollbar_x + k_scrollbar_w - k_batt_w;
constexpr int k_batt_y = (k_header_h - k_batt_h) / 2;

// What is left of the header for the console's own title.
constexpr int k_title_room = k_batt_x - 6 - k_title_x;
static_assert(k_title_room == Menu::k_title_room,
              "menu.hpp publishes this number too");

// A name too wide for its row slides back and forth while its row is
// selected, so a long title can be read instead of having to be renamed to
// fit. Only the selected row moves: seven names sliding at once is a menu
// nobody can read, and the row the cursor is on is the one being asked about.
constexpr uint32_t k_marquee_hold_ms = 1100;   // still at each end
constexpr int k_marquee_px_per_s = 34;

struct Rgb {
    uint8_t r, g, b;
};

constexpr Rgb k_bg{14, 16, 24};
constexpr Rgb k_header{36, 42, 60};
constexpr Rgb k_header_text{232, 238, 250};
constexpr Rgb k_row_text{198, 206, 222};
constexpr Rgb k_row_selected{58, 104, 178};
constexpr Rgb k_row_selected_text{255, 255, 255};
constexpr Rgb k_accent{250, 176, 84};
constexpr Rgb k_dim{92, 100, 120};

// The battery's ramp, from PicoCrystal-GBC's menu header: red empty, amber at
// half, mint at full, mixed between. Its colours are 4 bit per channel, so
// these are those values widened, not new ones picked by eye.
constexpr Rgb k_batt_shell{136, 140, 152};
constexpr Rgb k_batt_full{68, 221, 136};
constexpr Rgb k_batt_half{238, 187, 17};
constexpr Rgb k_batt_empty{238, 34, 17};

// A window onto part of a target: same pixels, narrower bounds.
//
// pse::draw_text clips to the target it is given and to nothing else, and a
// sliding name has to stop at the edge of its row rather than at the edge of
// the screen. Handing it a target that is only as wide as the row does that
// with no per pixel clip test and no second drawing path.
pse::RenderTarget columns(const pse::RenderTarget& target, int x, int w) {
    int x0 = x < 0 ? 0 : x;
    if (x0 > target.width) x0 = target.width;
    int x1 = x + w;
    if (x1 < x0) x1 = x0;
    if (x1 > target.width) x1 = target.width;

    pse::RenderTarget window = target;
    window.pixels = target.pixels +
                    static_cast<size_t>(x0) * pse::bytes_per_pixel(target.format);
    window.width = x1 - x0;
    return window;
}

// How far left a name has slid, `elapsed_ms` after its row was selected.
// Still at the left, out to the end, still there, and back: a name that only
// ever wrapped round would be read as two halves of two different names.
int marquee_offset(uint32_t elapsed_ms, int overflow) {
    if (overflow <= 0) return 0;
    const uint32_t travel_ms =
        static_cast<uint32_t>(overflow) * 1000u / k_marquee_px_per_s;
    if (travel_ms == 0) return 0;

    uint32_t t = elapsed_ms % (2 * (k_marquee_hold_ms + travel_ms));
    if (t < k_marquee_hold_ms) return 0;
    t -= k_marquee_hold_ms;
    if (t < travel_ms) {
        return static_cast<int>(static_cast<uint32_t>(overflow) * t / travel_ms);
    }
    t -= travel_ms;
    if (t < k_marquee_hold_ms) return overflow;
    t -= k_marquee_hold_ms;
    return overflow -
           static_cast<int>(static_cast<uint32_t>(overflow) * t / travel_ms);
}

void draw_icon(const pse::RenderTarget& target, const uint16_t* icon, int x,
               int y) {
    if (icon == nullptr) {
        // A game with no picture gets a plain tile rather than a hole, so a
        // missing thumbnail reads as "no art yet" and not as a broken menu.
        pse::fill_rect(target, x, y, k_icon_w, k_icon_h, 34, 38, 52);
        pse::draw_text_centred(target, "?", x + k_icon_w / 2,
                               y + (k_icon_h - pse::text_height()) / 2, 90, 98,
                               115);
        return;
    }
    for (int iy = 0; iy < k_icon_h; iy++) {
        for (int ix = 0; ix < k_icon_w; ix++) {
            const uint16_t px = icon[iy * k_icon_w + ix];
            // Five and six bit channels widened by replicating the high bits,
            // so full scale stays full scale instead of drifting grey.
            const uint8_t r = static_cast<uint8_t>(icon_red(px) * 255 / 31);
            const uint8_t g = static_cast<uint8_t>(icon_green(px) * 255 / 63);
            const uint8_t b = static_cast<uint8_t>(icon_blue(px) * 255 / 31);
            pse::plot_pixel(target, x + ix, y + iy, r, g, b);
        }
    }
}

// The charging bolt, knocked into the icon's interior: 5x9, the full height
// of the fill, two down-left strokes joined by a crossbar. The crossbar is
// what makes the zigzag legible when the strokes are two pixels thick.
constexpr int k_bolt_w = 5;
constexpr int k_bolt_x = 4;  // inset from the interior's left edge
constexpr uint8_t k_bolt[9] = {
    0b00011,  // ...##
    0b00110,  // ..##.
    0b01100,  // .##..
    0b11000,  // ##...
    0b11111,  // #####
    0b00011,  // ...##
    0b00110,  // ..##.
    0b01100,  // .##..
    0b11000,  // ##...
};

constexpr uint8_t mix(uint8_t a, uint8_t b, int t, int n) {
    return static_cast<uint8_t>(static_cast<int>(a) +
                                (static_cast<int>(b) - static_cast<int>(a)) * t / n);
}

constexpr Rgb mix(const Rgb& a, const Rgb& b, int t, int n) {
    return Rgb{mix(a.r, b.r, t, n), mix(a.g, b.g, t, n), mix(a.b, b.b, t, n)};
}

constexpr Rgb battery_colour(int percent) {
    return percent >= 100 ? k_batt_full
         : percent >= 50  ? mix(k_batt_half, k_batt_full, percent - 50, 50)
                          : mix(k_batt_empty, k_batt_half, percent, 50);
}

bool bolt_pixel(int x, int y) {
    if (x < 0 || x >= k_bolt_w || y < 0 || y >= 9) return false;
    return (k_bolt[y] & (1 << (k_bolt_w - 1 - x))) != 0;
}

// The battery, taken from PicoCrystal-GBC's header: a 17x13 body with a 3x5
// terminal nub, a 13x9 interior filled proportionally in the ramp above, and a
// bolt when the cable is in. Same geometry, same colours; what changed is that
// it draws through pse::fill_rect instead of into a picosystem framebuffer, so
// the preview harness renders the real icon on a host.
void draw_battery(const pse::RenderTarget& target, const Battery& battery,
                  int x, int y) {
    // No cell to report (the desktop build) draws nothing at all. An outline
    // with no fill is a flat battery, which is a thing a player would act on.
    if (battery.percent < 0) return;
    const int percent = battery.percent > 100 ? 100 : battery.percent;

    const Rgb shell = k_batt_shell;
    pse::fill_rect(target, x + 1, y, 15, 1, shell.r, shell.g, shell.b);
    pse::fill_rect(target, x + 1, y + 12, 15, 1, shell.r, shell.g, shell.b);
    pse::fill_rect(target, x, y + 1, 1, 11, shell.r, shell.g, shell.b);
    pse::fill_rect(target, x + 16, y + 1, 1, 11, shell.r, shell.g, shell.b);
    pse::fill_rect(target, x + 17, y + 4, 3, 5, shell.r, shell.g, shell.b);

    // Rounded, 0..13 wide, which leaves a pixel of margin inside the body at
    // full charge rather than the fill touching the outline.
    const int fill_w = (percent * 13 + 50) / 100;
    const Rgb fill = battery_colour(percent);
    pse::fill_rect(target, x + 2, y + 2, fill_w, 9, fill.r, fill.g, fill.b);

    if (!battery.charging) return;

    // The bolt straddles the fill edge, so it sits on two colours at once at
    // most charge levels. Inverting whatever is under it comes out two tone,
    // split down that edge, and stops reading as a shape. Instead: knock a
    // header coloured halo out of the interior around the bolt, then draw the
    // bolt flat on top. One silhouette, full contrast at 0% and at 100%.
    for (int by = 0; by < 9; by++) {
        for (int bx = -1; bx <= k_bolt_w; bx++) {
            bool halo = false;
            for (int sy = by - 1; sy <= by + 1 && !halo; sy++) {
                for (int sx = bx - 1; sx <= bx + 1; sx++) {
                    if (bolt_pixel(sx, sy)) {
                        halo = true;
                        break;
                    }
                }
            }
            if (halo) {
                pse::plot_pixel(target, x + 2 + k_bolt_x + bx, y + 2 + by,
                                k_header.r, k_header.g, k_header.b);
            }
        }
    }
    for (int by = 0; by < 9; by++) {
        for (int bx = 0; bx < k_bolt_w; bx++) {
            if (bolt_pixel(bx, by)) {
                pse::plot_pixel(target, x + 2 + k_bolt_x + bx, y + 2 + by, 255,
                                255, 255);
            }
        }
    }
}

}  // namespace

void Menu::reset(const char* start_slug) {
    cursor_ = step_cursor(-1, 1);
    if (start_slug != nullptr) {
        for (int i = 0; i < count_; i++) {
            if (entries_[i].game == nullptr) continue;
            const char* a = entries_[i].slug;
            const char* b = start_slug;
            while (*a != '\0' && *a == *b) {
                a++;
                b++;
            }
            if (*a == '\0' && *b == '\0') {
                cursor_ = i;
                last_played_ = i;
                break;
            }
        }
    }
    scroll_ = 0;
    follow_cursor();
}

int Menu::step_cursor(int from, int step) const {
    if (count_ <= 0) return 0;
    int index = from;
    for (int tried = 0; tried < count_; tried++) {
        index += step;
        if (index < 0) index = count_ - 1;
        if (index >= count_) index = 0;
        if (entries_[index].game != nullptr) return index;
    }
    // Every row is a heading: nothing is selectable, so stay put.
    return from < 0 ? 0 : from;
}

void Menu::follow_cursor() {
    if (cursor_ < scroll_) scroll_ = cursor_;
    if (cursor_ >= scroll_ + k_rows_visible) scroll_ = cursor_ - k_rows_visible + 1;
    const int max_scroll = count_ - k_rows_visible;
    if (scroll_ > max_scroll) scroll_ = max_scroll;
    if (scroll_ < 0) scroll_ = 0;
}

int Menu::update(const Input& input, uint32_t time_ms) {
    const int was = cursor_;
    if (input.down) cursor_ = step_cursor(cursor_, 1);
    if (input.up) cursor_ = step_cursor(cursor_, -1);
    // A long name on the row the cursor just landed on starts from its left
    // edge. Without this it picks up wherever a free running slide had got to,
    // and the first thing a player sees of a name can be its middle.
    if (cursor_ != was) marquee_since_ = time_ms;
    follow_cursor();

    if (input.select && cursor_ >= 0 && cursor_ < count_ &&
        entries_[cursor_].game != nullptr) {
        return cursor_;
    }
    return -1;
}

void Menu::draw(const pse::RenderTarget& target, uint32_t time_ms) const {
    pse::fill_rect(target, 0, 0, target.width, target.height, k_bg.r, k_bg.g,
                   k_bg.b);

    pse::fill_rect(target, 0, 0, target.width, k_header_h, k_header.r,
                   k_header.g, k_header.b);
    {
        // Clipped to the room left of the battery. The generator refuses a
        // title that does not fit, so this is the belt to that braces: a
        // title printing through the icon would be a header that looks
        // broken rather than a title that looks long.
        const pse::RenderTarget window = columns(target, k_title_x, k_title_room);
        pse::draw_text(window, k_console_title, 0, 4, k_header_text.r,
                       k_header_text.g, k_header_text.b, 2);
    }
    draw_battery(target, battery_, k_batt_x, k_batt_y);

    for (int row = 0; row < k_rows_visible; row++) {
        const int index = scroll_ + row;
        if (index >= count_) break;
        const Entry& entry = entries_[index];
        const int y = k_list_top + row * k_row_h;

        if (entry.game == nullptr) {
            // A heading: its name, then a rule running out to the edge so the
            // groups read as groups without costing a second line. Drawn into
            // the row's own window, so a long heading is cut off at the
            // scrollbar instead of running under it: a heading is not
            // selectable, so it can never be the row that slides.
            const int text_y = y + (k_row_h - pse::text_height()) / 2;
            const pse::RenderTarget window =
                columns(target, k_icon_x, k_scrollbar_x - 6 - k_icon_x);
            pse::draw_text(window, entry.name, 0, text_y, k_accent.r, k_accent.g,
                           k_accent.b);
            const int rule_x = pse::text_width(entry.name) + 6;
            pse::fill_rect(window, rule_x, text_y + 3, window.width - rule_x, 1,
                           60, 66, 84);
            continue;
        }

        const bool selected = index == cursor_;
        if (selected) {
            pse::fill_rect(target, 4, y, k_scrollbar_x - 8, k_row_h - 2,
                           k_row_selected.r, k_row_selected.g,
                           k_row_selected.b);
        }

        draw_icon(target, entry.icon, k_icon_x, y + (k_row_h - 2 - k_icon_h) / 2);

        const Rgb text = selected ? k_row_selected_text : k_row_text;
        const int name_h = pse::text_height(k_name_scale);
        const int name_w = pse::text_width(entry.name, k_name_scale);
        // Wider than the row: slide it while the row is selected, and clip it
        // to the row either way. An unselected long name sits at its start,
        // which is enough to tell the rows apart, and the one under the cursor
        // is the one being read.
        const int offset = selected ? marquee_offset(time_ms - marquee_since_,
                                                     name_w - k_name_room)
                                    : 0;
        const pse::RenderTarget window = columns(target, k_name_x, k_name_room);
        pse::draw_text(window, entry.name, -offset,
                       y + (k_row_h - 2 - name_h) / 2, text.r, text.g, text.b,
                       k_name_scale);

        // Where you left off, as a dot rather than a word.
        if (index == last_played_) {
            pse::fill_rect(target, k_dot_x, y + k_row_h / 2 - 3, k_dot_w, k_dot_w,
                           k_accent.r, k_accent.g, k_accent.b);
        }
    }

    // Scrollbar, only when there is something off screen. A track with a full
    // length thumb tells the player nothing and takes up the same room.
    if (count_ > k_rows_visible) {
        const int track_top = k_list_top;
        const int track_h = k_row_h * k_rows_visible;
        pse::fill_rect(target, k_scrollbar_x, track_top, k_scrollbar_w, track_h,
                       32, 36, 50);
        int thumb_h = track_h * k_rows_visible / count_;
        if (thumb_h < 8) thumb_h = 8;
        const int span = count_ - k_rows_visible;
        const int thumb_y = track_top + (track_h - thumb_h) * scroll_ / span;
        pse::fill_rect(target, k_scrollbar_x, thumb_y, k_scrollbar_w, thumb_h,
                       k_dim.r, k_dim.g, k_dim.b);
    }

    // Nothing else. There used to be a line here explaining the gesture that
    // left a game, and there is no gesture now: the power switch restarts the
    // console into this menu, which is a control the device already has and
    // nothing has to say out loud (rule 9).
}

}  // namespace console
