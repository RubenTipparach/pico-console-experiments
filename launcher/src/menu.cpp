#include "menu.hpp"

namespace launcher {

void Menu::reset(int count) {
    index_ = 0;
    (void)count;
}

bool Menu::update(const Input& input, int count) {
    if (count <= 0) return false;

    // Clamp rather than wrap. With one game per screen a wrap makes the last
    // game and the first look adjacent, and there is no way to tell from the
    // screen that the list ended, so paging past the end reads as a fault.
    if (input.left && index_ > 0) index_--;
    if (input.right && index_ + 1 < count) index_++;

    // A library that shrank between boots must not leave the cursor pointing
    // at a game that is no longer there.
    if (index_ >= count) index_ = count - 1;

    return input.select;
}

}  // namespace launcher
