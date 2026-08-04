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

// Capacity of the split rasterization queue, in triangles. One queued triangle
// is 24 bytes, so the default costs 15,360 bytes of RAM wherever a FrameQueue
// is instantiated. Overflow drops triangles and counts them rather than
// growing.
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
