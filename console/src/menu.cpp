#include "menu.hpp"

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

// Room a row's name has before it reaches the last played dot and the
// scrollbar. tools/gen_library.py refuses a name that does not fit, and
// console/tests/test_menu.cpp asserts the two agree.
constexpr int k_name_room = k_scrollbar_x - 6 - k_name_x;

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

int Menu::update(const Input& input) {
    if (input.down) cursor_ = step_cursor(cursor_, 1);
    if (input.up) cursor_ = step_cursor(cursor_, -1);
    follow_cursor();

    if (input.select && cursor_ >= 0 && cursor_ < count_ &&
        entries_[cursor_].game != nullptr) {
        return cursor_;
    }
    return -1;
}

void Menu::draw(const pse::RenderTarget& target) const {
    pse::fill_rect(target, 0, 0, target.width, target.height, k_bg.r, k_bg.g,
                   k_bg.b);

    pse::fill_rect(target, 0, 0, target.width, k_header_h, k_header.r,
                   k_header.g, k_header.b);
    pse::draw_text(target, k_console_title, 8, 4, k_header_text.r,
                   k_header_text.g, k_header_text.b, 2);

    for (int row = 0; row < k_rows_visible; row++) {
        const int index = scroll_ + row;
        if (index >= count_) break;
        const Entry& entry = entries_[index];
        const int y = k_list_top + row * k_row_h;

        if (entry.game == nullptr) {
            // A heading: its name, then a rule running out to the edge so the
            // groups read as groups without costing a second line.
            const int text_y = y + (k_row_h - pse::text_height()) / 2;
            pse::draw_text(target, entry.name, k_icon_x, text_y, k_accent.r,
                           k_accent.g, k_accent.b);
            const int rule_x = k_icon_x + pse::text_width(entry.name) + 6;
            pse::fill_rect(target, rule_x, text_y + 3, k_scrollbar_x - 6 - rule_x,
                           1, 60, 66, 84);
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
        pse::draw_text(target, entry.name, k_name_x,
                       y + (k_row_h - 2 - name_h) / 2, text.r, text.g, text.b,
                       k_name_scale);

        // Where you left off, as a dot rather than a word.
        if (index == last_played_) {
            pse::fill_rect(target, k_scrollbar_x - 12, y + k_row_h / 2 - 3, 4, 4,
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

    // The one line of instruction in the whole console. Rule 9 bans button
    // prompts for starting and retrying, and this is neither: leaving a game
    // is a gesture with nothing on screen to suggest it, so without this line
    // the way back does not exist for anyone who was not told.
    pse::draw_text_centred(target, "HOLD UP+DOWN TO LEAVE A GAME",
                           target.width / 2, target.height - 12, k_dim.r,
                           k_dim.g, k_dim.b);
}

}  // namespace console
