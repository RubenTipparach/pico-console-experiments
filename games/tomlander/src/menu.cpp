#include "menu.hpp"

namespace tl {

// One table, read both ways, so the two directions cannot disagree. Adding a
// mission is adding a row here and raising k_mission_count, and the title menu,
// the unlock, the press on path and the names all follow.
namespace {

struct MissionRow {
    uint8_t number;
    Mission mission;
    const char* name;
};

const MissionRow k_missions[] = {
    {1, Mission::Hop,      "1 HOP"},
    {2, Mission::Delivery, "2 DELIVERY"},
    {3, Mission::Salvage,  "3 SALVAGE"},
};

constexpr int k_row_count = static_cast<int>(sizeof(k_missions) /
                                             sizeof(k_missions[0]));

}  // namespace

Mission mission_for(uint8_t number) {
    for (int i = 0; i < k_row_count; i++) {
        if (k_missions[i].number == number) return k_missions[i].mission;
    }
    // A number from a corrupt save or a future build. The hop is the one
    // mission that is always unlocked and always winnable, so it is the safe
    // place to land rather than refusing to start at all.
    return Mission::Hop;
}

uint8_t number_of(Mission mission) {
    for (int i = 0; i < k_row_count; i++) {
        if (k_missions[i].mission == mission) return k_missions[i].number;
    }
    return 1;
}

uint8_t progress_after(uint8_t progress, Mission finished) {
    const uint8_t next = next_mission(finished);
    return next > progress ? next : progress;
}

uint8_t next_mission(Mission finished) {
    const uint8_t here = number_of(finished);
    return here < k_mission_count ? static_cast<uint8_t>(here + 1) : here;
}

const char* mission_name(uint8_t number) {
    for (int i = 0; i < k_row_count; i++) {
        if (k_missions[i].number == number) return k_missions[i].name;
    }
    return k_missions[0].name;
}

}  // namespace tl
