#pragma once

// Compile time budget for the software 3D renderer.
//
// The depth buffer is a statically sized array because dynamic allocation on a
// 264 KB device is a way to fail at the worst possible moment. Raising these
// costs RAM immediately: the depth buffer is width * height bytes.
//
// 120x120 is the PicoSystem's lores mode under the 32blit SDK, and it costs
// 14,400 bytes of depth buffer. Do not raise this to 240x240 without checking
// the map file: that alone would be 57.6 KB on top of a 115 KB hires
// framebuffer.

#ifndef PSE_RENDER_WIDTH
#define PSE_RENDER_WIDTH 120
#endif

#ifndef PSE_RENDER_HEIGHT
#define PSE_RENDER_HEIGHT 120
#endif

// Capacity of the split rasterization queue, in triangles.
//
// This is a RAM ceiling and a drop threshold. It is NOT a performance budget,
// and reading it as one is a mistake that has already been made here: a frame
// sitting at 229 of 640 was called "36 percent of budget, so triangles are
// cheap", when all that measured was how much of a fixed size array was in
// use. Nothing in this number knows what an RP2040 can rasterize in a frame.
//
// The real budget is time on a 250 MHz M0+ with no FPU and no divide
// instruction, and a triangle spends it twice over: a software float vertex
// transform per corner, then per triangle setup (bounding box, three edge
// functions, three divides for the reciprocal depths) before a single pixel is
// filled. A sliver covering twenty pixels pays all of that to fill almost
// nothing, which is why trading thirty sliver triangles for one textured quad
// is a win even when the queue was nowhere near full.
//
// Nobody has measured the time budget. It needs a device, or at least an ARM
// toolchain and a cycle counter, and until someone does the honest statement
// is that the ceiling below is the only limit written down anywhere.
//
// Overflow drops triangles and counts them rather than growing. sizeof
// (ScreenTriangle) is 34 bytes, so the default costs 21,766 bytes of RAM
// wherever a FrameQueue is instantiated. That figure moves whenever
// ScreenTriangle gains a field: this comment said 24 bytes and 15,360 for a
// long time after it was already 28 and 17,926, which is exactly how a budget
// comment stops being a measurement and becomes a wish.
#ifndef PSE_MAX_QUEUE
#define PSE_MAX_QUEUE 640
#endif

namespace pse {

constexpr int k_render_width = PSE_RENDER_WIDTH;
constexpr int k_render_height = PSE_RENDER_HEIGHT;

// Fixed point scale shared by projection and rasterization. The RP2040 has no
// FPU, so anything running per vertex or per pixel stays in integers.
constexpr int k_fixed_shift = 10;
constexpr int k_fixed_one = 1 << k_fixed_shift;   // 1024

}  // namespace pse
