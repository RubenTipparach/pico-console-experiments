#include "pse/shared_render.hpp"

namespace pse {
namespace {

// File scope rather than function local statics: a function local static of a
// non trivially constructible type costs a guard variable and a call into
// __cxa_guard_acquire on every access, and this is reached per frame.
Rasterizer g_rasterizer;
FrameQueue g_queue;

}  // namespace

Rasterizer& shared_rasterizer() { return g_rasterizer; }

FrameQueue& shared_queue() { return g_queue; }

}  // namespace pse
