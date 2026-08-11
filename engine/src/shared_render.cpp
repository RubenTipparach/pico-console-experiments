#include "pse/shared_render.hpp"

namespace pse {
namespace {

// File scope rather than function local statics: a function local static of a
// non trivially constructible type costs a guard variable and a call into
// __cxa_guard_acquire on every access, and this is reached per frame.
// The default sized one, for a game that has not asked for a window of its
// own. OwnedRasterizer carries the storage, so the size is a template
// argument here rather than part of Rasterizer's layout: see
// Rasterizer::set_depth_buffer for why that distinction is load bearing.
OwnedRasterizer<k_render_width, k_render_height> g_rasterizer;
FrameQueue g_queue;

}  // namespace

Rasterizer& shared_rasterizer() { return g_rasterizer; }

FrameQueue& shared_queue() { return g_queue; }

}  // namespace pse
