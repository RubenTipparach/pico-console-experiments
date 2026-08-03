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

}  // namespace pse
