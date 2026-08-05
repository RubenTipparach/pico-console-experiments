#pragma once

#include <cstdint>

#include "library.hpp"
#include "pse/pixel.hpp"

namespace console {

// The menu, drawn into a RenderTarget and knowing nothing else.
//
// No SDK, no globals, no drawing calls it did not make itself, and the table
// it shows is handed to it rather than reached for. That is what makes it
// testable and screenshottable on a host: console/tests renders the exact
// frames the PicoSystem draws into a buffer and writes them out as pictures,
// and it can do it against a made up library of thirty entries to prove the
// scrolling. The launcher this replaces could only be looked at by flashing
// it.
class Menu {
public:
    // Buttons, already reduced to edges by the caller. The menu never sees
    // the SDK's button state.
    struct Input {
        bool up = false;      // just pressed, or a repeat
        bool down = false;
        bool select = false;  // just pressed
    };

    Menu(const Entry* entries, int count) : entries_(entries), count_(count) {}

    // Put the cursor on `start_slug`, which is how the console reopens on the
    // last thing played. nullptr or an unknown slug starts at the first
    // selectable row.
    void reset(const char* start_slug = nullptr);

    // One frame of menu. Returns the index of the chosen entry, or -1.
    int update(const Input& input);

    void draw(const pse::RenderTarget& target) const;

    int cursor() const { return cursor_; }
    int scroll() const { return scroll_; }

    // Marks the row the console last handed the machine to, so the menu says
    // where you were without saying anything.
    void set_last_played(int index) { last_played_ = index; }

    // Rows visible at once. Exposed for the layout tests, which are the only
    // way to catch a name that prints through the edge of its own row.
    static constexpr int k_rows_visible = 7;

private:
    // Next selectable row in `step` direction, wrapping, skipping headings.
    int step_cursor(int from, int step) const;
    void follow_cursor();

    const Entry* entries_;
    int count_;
    int cursor_ = 0;
    int scroll_ = 0;
    int last_played_ = -1;
};

}  // namespace console
