#pragma once

#include <cstdint>

namespace console {

// What the cell is doing, as the menu header draws it.
//
// `percent` is -1 for "no idea", which is the honest answer on the desktop
// build: there is no cell there, and a made up number would look exactly like
// a measurement. The menu draws no icon at all in that case rather than an
// empty one, because an empty battery is a thing a player would act on.
struct Battery {
    int percent = -1;
    bool charging = false;
};

// Reads the cell, at most every so often, returning the previous reading in
// between. Call it once a frame with the SDK's clock: there is no clock in
// here, which is what lets the host build link without a time source.
//
// Two implementations, picked in console/CMakeLists.txt the same way the
// engine picks run_split: battery_pico.cpp on the device, battery_host.cpp
// everywhere else.
Battery battery_sample(uint32_t time_ms);

}  // namespace console
