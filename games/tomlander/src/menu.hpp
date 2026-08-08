#pragma once

// The shell around a flight: which mission a number names, what unlocks what,
// and what rows the two menus have.
//
// This lives apart from game.cpp on purpose. game.cpp is SDK code, it does not
// compile without the 32blit SDK, and nothing in this environment can build or
// run it, so everything that ever lived in there shipped unverified. Three
// player visible bugs came out of that in a row: mission three was unreachable,
// there was no way back to the title from a pause, and there was no way to pick
// a mission at all. The first of those was one wrong comparison that had been
// correct when the game had two missions.
//
// So the rules moved here, where they are pure integer C++ with no SDK behind
// them and test_sim.cpp can hold them to account. game.cpp draws these rows and
// feeds them button presses; it decides nothing.

#include <cstdint>

#include "sim.hpp"

namespace tl {

// ---- missions by number ----

// Which flight a mission NUMBER names. The number is what the player picks and
// what the save carries; Mission is what world_init takes.
//
// This used to be written inline in start_flight as
// `number >= 2 ? Delivery : Hop`. That was true when there were two missions
// and silently wrong the moment there were three: picking mission three flew
// the delivery, so finishing it set progress to three and then nothing ever
// changed again. The gauge went up and the game did not, which is the worst
// shape a progression bug can take, because it looks like it is working.
Mission mission_for(uint8_t number);

// The number a Mission is, which is the same mapping read backwards. Both
// directions exist so neither is ever open coded at a call site again.
uint8_t number_of(Mission mission);

// The furthest mission unlocked once `finished` has been landed. Never counts
// down: a player who goes back and replays the hop does not lose the salvage.
uint8_t progress_after(uint8_t progress, Mission finished);

// The flight that follows this one, for the "landed, press on" path.
//
// The one AFTER the mission just flown, not the furthest unlocked. Replaying
// the hop with everything open should roll into the delivery, not jump to the
// salvage: pressing on means the next one, and the menu is where you go to
// skip about.
uint8_t next_mission(Mission finished);

// A short name for a mission number, for the title menu's rows. Numbered as
// well as named, because the number is what the save and the debrief talk in.
const char* mission_name(uint8_t number);

// ---- the title menu ----
//
// One row per mission unlocked, then SOUND. So a first boot is two rows, and
// the list grows as the player earns it: no locked rows, no greyed out teases,
// nothing on the screen that cannot be pressed. Rule 9's sparse UI comes out of
// the progression for free.

inline uint8_t title_row_count(uint8_t progress) {
    return static_cast<uint8_t>(progress + 1);
}

// Is this row a mission, and which one? Returns 0 for the SOUND row, which is
// never a mission number, so a caller can branch on it directly.
inline uint8_t title_row_mission(uint8_t progress, uint8_t row) {
    return row < progress ? static_cast<uint8_t>(row + 1) : 0;
}

// ---- the pause menu ----
//
// MENU is the row that was missing. Without it the only way out of a flight
// was to finish it or wreck it, which on the web means reloading the page.
enum PauseRow : uint8_t {
    kPauseResume = 0,
    kPauseRestart,
    kPauseMenu,
    kPauseSound,
    kPauseRowCount,
};

// ---- moving through a menu ----

// Where the cursor lands after a step, wrapping both ways. Wrapping rather
// than clamping because the lists are short and the title's grows: a player who
// holds down past the end of a two row menu should come back to the top rather
// than sit against a wall.
inline uint8_t menu_step(uint8_t at, int delta, uint8_t count) {
    if (count == 0) return 0;
    int next = static_cast<int>(at) + delta;
    while (next < 0) next += count;
    return static_cast<uint8_t>(next % count);
}

}  // namespace tl
