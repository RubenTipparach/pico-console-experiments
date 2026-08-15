#pragma once

#include "pse/raster.hpp"

namespace pse {

// The renderer's scratch space, one copy per program.
//
// A Rasterizer carries a 14,400 byte depth buffer and a FrameQueue 15,360
// bytes of triangles: together they are by far the largest RAM in this repo
// after the framebuffer, and the SDK has already spent 115,200 bytes of the
// PicoSystem's 264 KB on that. When every game was its own binary, one copy
// each was the only possibility. In the console they are linked together, and
// three games with a file scope Rasterizer apiece is 89 KB of a 149 KB budget
// spent on scratch space that only one game can be using: only one game runs
// at a time, and no state survives leaving it.
//
// So a 3D game asks the engine for these instead of declaring its own. The
// standalone build of that same game gets exactly what it had before, one
// instance, because it is the only caller.
//
// This lives in a translation unit of its own so that a 2D game which never
// calls it never links it (rule 7: a 2D game must not be forced to carry the
// depth buffer). Nothing pulls it in but a direct call.
Rasterizer& shared_rasterizer();
FrameQueue& shared_queue();

// How many bytes of depth buffer this build has. tools/depth_arena.py works
// it out from the `render:` block of every game.yml in the build and
// engine/CMakeLists.txt passes it in, so it is the largest window any game
// here asks for and never smaller than the default square.
#ifndef PSE_DEPTH_ARENA_BYTES
#define PSE_DEPTH_ARENA_BYTES (PSE_RENDER_WIDTH * PSE_RENDER_HEIGHT)
#endif

// The shared rasterizer, pointed at a window of the caller's own shape.
//
// This is what a game drawing something other than the default square asks
// for. jokerreels renders a 240x112 band: it could not use a 120x120 buffer,
// so it carried its own 26,880 byte one NEXT TO the shared one, and the
// console paid for both while only ever running one of them. Same buffer now,
// different dimensions over it.
//
// Safe for the same reason the shared rasterizer is safe at all: one game runs
// at a time, and begin_frame clears the depth buffer, so nothing of the last
// game's frame can be read by this one.
//
// A template so the fit is checked when it is written rather than when it is
// run. Getting this wrong is not a smaller picture, it is a rasterizer writing
// past the end of the arena.
Rasterizer& bind_shared_depth(int width, int height);

template <int Width, int Height>
Rasterizer& shared_windowed_rasterizer() {
    static_assert(Width * Height <= PSE_DEPTH_ARENA_BYTES,
                  "this window is bigger than the build's shared depth buffer: "
                  "declare it in the game's game.yml under render: so "
                  "tools/depth_arena.py can size the buffer for it");
    return bind_shared_depth(Width, Height);
}

}  // namespace pse
