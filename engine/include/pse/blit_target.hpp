#pragma once

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

}  // namespace pse
