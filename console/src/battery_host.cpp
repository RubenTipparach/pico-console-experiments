// The desktop console has no cell, so it says so.
//
// Not a stub that invents a level: the menu draws no icon when the percent is
// -1, and a desktop window showing "72%" of a battery that does not exist is
// the kind of detail that gets believed and then debugged on hardware.

#include "battery.hpp"

namespace console {

Battery battery_sample(uint32_t) { return Battery{}; }

}  // namespace console
