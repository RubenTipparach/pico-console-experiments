#pragma once

#include <cstdint>

#include "pse/pixel.hpp"

namespace pse {

// The only place in the engine that knows the 32blit SDK exists.
//
// Everything else (Rasterizer, Renderer3D, MeshData) is plain C++ against a
// RenderTarget, which is why the same renderer compiles for the device, for
// desktop, and for Emscripten without a single conditional. Swapping SDKs means
// replacing this one file.
RenderTarget target_from_screen();

// Which of the two screen modes a game wants. Mirrors blit::ScreenMode, and
// exists so a game does not have to name the SDK's enum to pick one: the
// board specific part of asking for a mode lives in blit_target.cpp, on the
// SDK side of rule 6's line.
enum class ScreenMode { lores, hires };

// Ask for a screen of the size this game was written for.
//
// Games call this instead of the SDK's set_screen_mode. The difference is one
// argument: this passes the design bounds from board.hpp explicitly, where
// the bare SDK call takes whatever the fitted panel happens to be. On a
// PicoSystem, on desktop and on the web those are the same thing and this is
// exactly the call that was there before. On a Tufty 2350 it is the whole
// port: the dbi driver allows any surface that fits and centres it on the
// panel itself, so a 240x240 game lands in the middle of a 320x240 screen
// with its geometry untouched.
void set_screen_mode(ScreenMode mode);

// Presents the buttons this board actually has as the buttons the games
// expect, for as long as it is in scope.
//
// The PicoSystem has a dpad and four face buttons. The Tufty has five: up,
// down, A, B and C, with no left and no right at all, and every game in this
// repo reads DPAD_LEFT and DPAD_RIGHT. This maps the ones that exist onto the
// ones that are read.
//
// It is a guard rather than a function because blit::buttons is the SDK's own
// state, and the SDK derives the next tick's pressed and released edges from
// it: engine.cpp takes its `last_state` snapshot before calling update(), and
// a catch up tick runs update() twice off one snapshot, so a word left
// modified would read as a fresh press the second time round. Mapping on the
// way in and putting the SDK's own values back on the way out means the games
// see the mapping and the SDK never does.
//
// On a board that needs no mapping this is empty and costs nothing.
class MappedButtons {
public:
    MappedButtons();
    ~MappedButtons();

    MappedButtons(const MappedButtons&) = delete;
    MappedButtons& operator=(const MappedButtons&) = delete;

private:
    uint32_t saved_state_ = 0;
    uint32_t saved_pressed_ = 0;
    uint32_t saved_released_ = 0;
};

}  // namespace pse
