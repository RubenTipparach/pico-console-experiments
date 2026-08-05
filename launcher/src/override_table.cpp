#include "override_table.hpp"

namespace launcher {

// Magic, then k_max_slots titles of k_override_title_size bytes each, slot n
// at index n - 1. The initializer is shorter than the array on purpose: the
// rest zero-fills, which is "no override for any slot" until PicoFlasher
// patches one in.
const uint8_t g_override_table[k_override_table_size] = {
    'P', 'S', 'E', 'O', 'V', 'R', '0', '1',
};

}  // namespace launcher
