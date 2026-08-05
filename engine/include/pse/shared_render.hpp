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

}  // namespace pse
