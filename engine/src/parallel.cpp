#include "pse/parallel.hpp"

namespace pse {

void render_band(Rasterizer& rasterizer, const FrameQueue& queue,
                 const SkyGradient& sky, int row_begin, int row_end,
                 int tri_begin, int tri_end,
                 int gradient_begin, int gradient_end) {
    rasterizer.clear_depth_rows(row_begin, row_end);
    rasterizer.clear_gradient_span(sky.top_r, sky.top_g, sky.top_b,
                                   sky.bottom_r, sky.bottom_g, sky.bottom_b,
                                   row_begin, row_end,
                                   gradient_begin, gradient_end);
    if (tri_begin < 0) tri_begin = 0;
    if (tri_end > queue.count) tri_end = queue.count;
    for (int i = tri_begin; i < tri_end; i++) {
        rasterizer.draw_rows(queue.tris[i], row_begin, row_end);
    }
}

void render_rows(Rasterizer& rasterizer, const FrameQueue& queue,
                 const SkyGradient& sky, int row_begin, int row_end) {
    render_band(rasterizer, queue, sky, row_begin, row_end, 0, queue.count,
                0, rasterizer.target().height);
}

}  // namespace pse
