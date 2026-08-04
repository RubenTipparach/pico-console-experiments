// Host, desktop, and web implementation of run_split: both bands run
// sequentially on the calling thread.
//
// It still runs as two bands rather than one full pass, so every platform
// exercises the exact code path the device uses, and a band seam bug shows up
// in the host tests rather than only on hardware.

#include "pse/parallel.hpp"

namespace pse {

void run_split(Rasterizer& rasterizer, const FrameQueue& queue,
               const SkyGradient& sky) {
    const int height = rasterizer.target().height;
    const int mid = height / 2;
    render_rows(rasterizer, queue, sky, 0, mid);
    render_rows(rasterizer, queue, sky, mid, height);
}

}  // namespace pse
