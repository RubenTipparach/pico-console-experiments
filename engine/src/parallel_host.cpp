// Host, desktop, and web implementation of run_split: both bands run
// sequentially on the calling thread.
//
// It still runs as two bands rather than one full pass, so every platform
// exercises the exact code path the device uses, and a band seam bug shows up
// in the host tests rather than only on hardware.

#include "pse/parallel.hpp"

namespace pse {

void run_split(Rasterizer& rasterizer, const FrameQueue& queue,
               const SkyGradient& sky_top, const SkyGradient& sky_bottom) {
    const SplitPlan plan = plan_split(rasterizer, queue);
    render_band(rasterizer, queue, sky_top, 0, plan.mid,
                plan.top_tri_begin, plan.top_tri_end, 0, plan.top_grad_end);
    render_band(rasterizer, queue, sky_bottom, plan.mid, plan.height,
                plan.bottom_tri_begin, plan.bottom_tri_end,
                plan.bottom_grad_begin, plan.height);
}

void run_split(Rasterizer& rasterizer, const FrameQueue& queue,
               const SkyGradient& sky) {
    run_split(rasterizer, queue, sky, sky);
}

}  // namespace pse
