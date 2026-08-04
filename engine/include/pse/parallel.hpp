#pragma once

#include "pse/raster.hpp"

namespace pse {

// The frame's background gradient, passed to the workers so the clear runs in
// parallel with everything else.
struct SkyGradient {
    uint8_t top_r, top_g, top_b;
    uint8_t bottom_r, bottom_g, bottom_b;
};

// Rasterize rows [row_begin, row_end) of a collected frame: clear that band's
// depth, fill its slice of the gradient, then draw every queued triangle
// clipped to the band. Pure code, same on every platform, which is what makes
// the split testable on a host: two bands must reproduce one full-frame pass
// byte for byte.
void render_rows(Rasterizer& rasterizer, const FrameQueue& queue,
                 const SkyGradient& sky, int row_begin, int row_end);

// Execute the whole collected frame.
//
// On the PicoSystem this splits the screen between the cores: core 0 takes the
// top band, core 1 the bottom. The bands are disjoint rows of the framebuffer
// and depth buffer, so there is no locking anywhere. Everywhere else (web,
// desktop, host tests) both bands run sequentially on the calling thread and
// produce the identical image.
//
// Flash safety contract on device: core 1 idles in a loop that lives entirely
// in RAM. Anything that writes flash (write_save) must happen OUTSIDE
// run_split, which is when core 1 is guaranteed to be parked in that RAM loop
// and immune to XIP being disabled mid write. Games uphold this by saving
// between frames, never during rendering.
void run_split(Rasterizer& rasterizer, const FrameQueue& queue,
               const SkyGradient& sky);

}  // namespace pse
