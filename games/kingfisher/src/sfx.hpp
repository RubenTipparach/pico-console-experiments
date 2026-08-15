#pragma once

// Tiny beeper voice. One channel, square wave, because on the PicoSystem the
// audio hardware is a piezo and the SDK's beep driver plays the first square
// wave channel it finds. Everything is short: this is punctuation, not music.
//
// And nothing goes below 400 Hz. A piezo falls away steeply under its
// resonance, so the low cues here were inaudible on the device while sounding
// fine on the web, where the full mixer runs through a real speaker: this was
// reported from playing as "the lower tones aren't really working", and they
// were not. The reel ratchet was 72 Hz. A snap and an escape are falls through
// the audible band now rather than low tones, which is what says failure on a
// beeper. tools/tests/test_audio_range.py holds the floor for the whole repo.

#include "sim.hpp"

namespace kfs {

void sfx_init();
void sfx_handle(const kf::Events& events);
void sfx_tick();

// Mute switch, owned by the options screen. Muting also silences anything
// already playing, so toggling mid jingle goes quiet immediately.
void sfx_set_enabled(bool enabled);
bool sfx_enabled();

}  // namespace kfs
