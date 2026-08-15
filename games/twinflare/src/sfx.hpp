#pragma once

// The noise a podracer makes.
//
// Three voices, and WHICH CHANNEL each one sits on is a decision about the
// hardware rather than about the music. On the PicoSystem the audio output is a
// piezo and the SDK's beep driver reaches it through a SQUARE wave channel,
// picking the lowest numbered one it can use: on the device there is one voice,
// not eight, and everything else here is a web and desktop luxury.
//
// So the cues go on channel 0. That much is forced whichever way the driver
// resolves "can use": if it takes the lowest square channel outright, channel 0
// is the device's voice and the cues are what the device plays; if it takes the
// lowest square channel that is not idle, a cue still pre-empts anything above
// it while it sounds. Either reading gives the device its punctuation, which is
// what a piezo is good at.
//
// The engine drone is channel 1 and SQUARE rather than a triangle for the same
// reason: under the second reading it is audible on hardware whenever no cue is
// playing, and under the first it is exactly as inaudible there as a triangle
// would have been. It cannot go on channel 0, under either reading: a held note
// on the device's voice owns the piezo for the whole race and no cue is ever
// heard again.
//
//   channel 0  SQUARE  the cues: countdown, green, lap, flag, boost, damage.
//   channel 1  SQUARE  the engine, held, its pitch tracking speed and charge.
//   channel 2  NOISE   grinding along a wall. Silent on the device by design,
//                      because it is texture and the device has no voice spare.
//
// The consequence of putting the cues on a channel with a non-zero sustain is
// that a sequence which ends without trigger_release parks in SUSTAIN, stays
// not-idle, and mutes the engine permanently. sfx_tick's terminal release is
// load bearing.

#include "sim.hpp"

namespace tfs {

void sfx_init();

// Once a FRAME, with the events of every sim tick in that frame merged
// together. Not once a tick: a frame can step the sim eight times, and calling
// this eight times means seven cues are overwritten before anything is heard.
void sfx_handle(const twinflare::Events& events);

// Once a frame, after sfx_handle, and on EVERY update including the menus and
// the pause screen. It drives the sequencer one step, so skipping it while a
// menu is open freezes a jingle mid note.
void sfx_tick();

// Everything off, now. For the menus, where the engine would otherwise idle on
// under a static picture.
void sfx_silence();

void sfx_set_enabled(bool enabled);
bool sfx_enabled();

}  // namespace tfs
