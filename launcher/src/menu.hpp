#pragma once

// One game per screen, D-pad to page. Kept apart from the drawing so the host
// tests can prove the paging rules without a screen: an empty library, the
// ends of the list, and what a press does when there is nothing to select.

namespace launcher {

struct Input {
    bool left;
    bool right;
    bool select;
};

class Menu {
public:
    void reset(int count);

    // Returns true when the player chose the game at index(). The caller does
    // the launching: this class never knows what a slot is.
    bool update(const Input& input, int count);

    int index() const { return index_; }

private:
    int index_ = 0;
};

}  // namespace launcher
