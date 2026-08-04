#pragma once

// Tiny beeper voice. One channel, square wave, because on the PicoSystem the
// audio hardware is a piezo and the SDK's beep driver plays the first square
// wave channel it finds. Everything is short: this is punctuation, not music.

#include "sim.hpp"

namespace kfs {

void sfx_init();
void sfx_handle(const kf::Events& events);
void sfx_tick();

}  // namespace kfs
