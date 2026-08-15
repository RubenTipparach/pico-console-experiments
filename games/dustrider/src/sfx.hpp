#pragma once

// The noise a dirt bike makes.
//
// Two voices, and which channel each one sits on is a decision about the
// HARDWARE rather than about the music. On the PicoSystem the audio output is
// a piezo, reached through a square wave channel, and the driver takes the
// lowest numbered one it can use: on the device there is one voice, not eight,
// and everything else here is a web and desktop luxury.
//
//   channel 0  SQUARE  the cues: away, a hundred metres, a wreck, a record.
//   channel 1  SQUARE  the engine, held, its pitch tracking forward speed.
//   channel 2  NOISE   sand under the wheels, and the warning when the window
//                      is closing. Silent on the device by design.
//
// The cues take channel 0 because that is the one the device plays, and a
// piezo is good at punctuation. The engine cannot go there: a held note on the
// device's voice owns the piezo for the whole run and no cue is ever heard
// again.
//
// NOTHING HERE GOES BELOW 400 Hz. A piezo falls away steeply below its
// resonance, so a low tone is silent on hardware while sounding fine on the
// web, where the full mixer runs through a real speaker. Low reads as bad
// news, so every game in this repo reached for a low failure sound and every
// one of them was inaudible; what replaces it is a FALL through the audible
// band. tools/tests/test_audio_range.py holds the floor for the whole repo.

#include "sim.hpp"

namespace drs {

void sfx_init();

// Once a FRAME, with the events of every sim tick in that frame merged
// together by dr::merge_events. Not once a tick: a frame can step the sim more
// than once, and the extra calls overwrite each other before anything sounds.
void sfx_handle(const dr::Events& events);

// Once a frame, after sfx_handle, and on EVERY update including the title. It
// drives the sequencer one step, so skipping it freezes a cue mid note.
void sfx_tick();

// Everything off, now. The engine is a held note, so anything that stops the
// run has to say so.
void sfx_silence();

void sfx_set_enabled(bool enabled);
bool sfx_enabled();

}  // namespace drs
