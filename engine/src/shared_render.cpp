#include "pse/shared_render.hpp"

namespace pse {
namespace {

// File scope rather than function local statics: a function local static of a
// non trivially constructible type costs a guard variable and a call into
// __cxa_guard_acquire on every access, and this is reached per frame.
//
// The default sized one, for a game that has not asked for a window of its
// own. OwnedRasterizer carries the storage, so the size is a template
// argument here rather than part of Rasterizer's layout: see
// Rasterizer::set_depth_buffer for why that distinction is load bearing.
//
// This was briefly a bare arena that any game could lay its own window over,
// so a game drawing a different shape could share these bytes instead of
// bringing a second buffer. It saved 14,444 bytes and corrupted the bottom of
// the screen in every 3D game on both boards. The binding happened in each
// game's static initialiser, and jokerreels bound its 240x112 window
// unconditionally while everyone else bound 120x120 only if nothing had bound
// yet, so in a console holding both the winner was link order. Losing it left
// every other game rasterizing with depth_w = 240 on a 120 wide screen, whose
// index y * 240 + x runs off the end of the buffer from about y = 112: the
// bottom band, which is exactly what came back from the device.
//
// The lesson is not "bind more carefully". It is that a shared object whose
// shape is set by whichever game initialised last has no single correct
// state, and a type that fixes the shape cannot be got wrong that way.
OwnedRasterizer<k_render_width, k_render_height> g_rasterizer;
FrameQueue g_queue;

}  // namespace

Rasterizer& shared_rasterizer() { return g_rasterizer; }

FrameQueue& shared_queue() { return g_queue; }

}  // namespace pse
