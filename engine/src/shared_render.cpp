#include "pse/shared_render.hpp"

namespace pse {
namespace {

// File scope rather than function local statics: a function local static of a
// non trivially constructible type costs a guard variable and a call into
// __cxa_guard_acquire on every access, and this is reached per frame.
//
// The depth buffer is an array here rather than an OwnedRasterizer's member,
// which is the change that lets two differently shaped games share it. An
// OwnedRasterizer<W,H> fixes the window in the type, so a game wanting another
// shape had to bring a second buffer; the arena is just bytes, and
// set_depth_buffer lays whatever window is being drawn over it. Sized by
// tools/depth_arena.py to the largest any game in this build asks for, so a
// build containing only square games is exactly the size it always was.
alignas(4) uint8_t g_depth[PSE_DEPTH_ARENA_BYTES];

Rasterizer g_rasterizer;
FrameQueue g_queue;

}  // namespace

Rasterizer& shared_rasterizer() {
    // Bound on first use rather than at static init. Rasterizer's members are
    // all constant initialised so there is no order problem to dodge, but a
    // game that wants a window of its own binds a different shape over the
    // same bytes, and doing that from an initialiser would race this one.
    // depth_width() is zero only before anybody has bound anything.
    if (g_rasterizer.depth_width() == 0) {
        g_rasterizer.set_depth_buffer(g_depth, k_render_width, k_render_height);
    }
    return g_rasterizer;
}

Rasterizer& bind_shared_depth(int width, int height) {
    g_rasterizer.set_depth_buffer(g_depth, width, height);
    return g_rasterizer;
}

FrameQueue& shared_queue() { return g_queue; }

}  // namespace pse
