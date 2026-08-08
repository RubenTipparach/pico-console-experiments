#pragma once

#include <cstdint>

#include "battery.hpp"
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
    //
    // `time_ms` is the SDK's clock, and the menu uses it for one thing: a name
    // too wide for its row starts sliding from its left edge when the cursor
    // arrives, rather than partway through wherever a free running animation
    // happened to be. A caller with no clock passes nothing.
    int update(const Input& input, uint32_t time_ms = 0);

    void draw(const pse::RenderTarget& target, uint32_t time_ms = 0) const;

    // The cell, drawn in the header. Handed in rather than read here: the menu
    // draws on a host too, where there is no ADC to read, and the preview
    // harness wants to ask for a level rather than have one. A percent of -1
    // draws no icon at all.
    void set_battery(Battery battery) { battery_ = battery; }

    int cursor() const { return cursor_; }
    int scroll() const { return scroll_; }

    // Marks the row the console last handed the machine to, so the menu says
    // where you were without saying anything.
    void set_last_played(int index) { last_played_ = index; }

    // Rows visible at once. Exposed for the layout tests, which are the only
    // way to catch a name that prints through the edge of its own row.
    static constexpr int k_rows_visible = 7;

    // Pixels a name has on a row before it reaches the last played dot. A
    // wider name is not a mistake: it slides back and forth while its row is
    // selected, and is clipped to this window at every offset, so nothing has
    // to be renamed to fit. menu.cpp static_asserts this against the layout it
    // draws, and the tests and tools/gen_library.py read it here rather than
    // keeping a copy of the number.
    static constexpr int k_name_room = 177;

    // Pixels the console's own title has in the header before it reaches the
    // battery icon. The title does not scroll (it is one string, set once, and
    // a header that never sits still is a header nobody stops reading), so
    // this one is still a refusal in tools/gen_library.py.
    static constexpr int k_title_room = 202;

private:
    // Next selectable row in `step` direction, wrapping, skipping headings.
    int step_cursor(int from, int step) const;
    void follow_cursor();

    const Entry* entries_;
    int count_;
    int cursor_ = 0;
    int scroll_ = 0;
    int last_played_ = -1;
    // When the cursor last moved, so a long name on the row it landed on
    // starts its slide from the beginning.
    uint32_t marquee_since_ = 0;
    Battery battery_;
};

}  // namespace console
