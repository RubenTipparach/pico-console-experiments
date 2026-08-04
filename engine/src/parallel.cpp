#include "pse/parallel.hpp"

namespace pse {

void render_rows(Rasterizer& rasterizer, const FrameQueue& queue,
                 const SkyGradient& sky, int row_begin, int row_end) {
    rasterizer.clear_depth_rows(row_begin, row_end);
    rasterizer.clear_gradient_rows(sky.top_r, sky.top_g, sky.top_b,
                                   sky.bottom_r, sky.bottom_g, sky.bottom_b,
                                   row_begin, row_end);
    for (uint16_t i = 0; i < queue.count; i++) {
        rasterizer.draw_rows(queue.tris[i], row_begin, row_end);
    }
}

}  // namespace pse
