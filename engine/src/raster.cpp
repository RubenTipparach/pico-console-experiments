#include "pse/raster.hpp"

#include <cstring>

namespace pse {
namespace {

inline int clamp_int(int value, int low, int high) {
    if (value < low) return low;
    if (value > high) return high;
    return value;
}

inline int min3(int a, int b, int c) {
    const int m = a < b ? a : b;
    return m < c ? m : c;
}

inline int max3(int a, int b, int c) {
    const int m = a > b ? a : b;
    return m > c ? m : c;
}

}  // namespace

void Rasterizer::begin_frame(const RenderTarget& target) {
    target_ = target;
    triangles_drawn_ = 0;
    queue_ = nullptr;

    // Refuse to draw outside the statically sized depth buffer rather than
    // corrupting RAM if a caller hands over a bigger surface than the build was
    // configured for.
    if (target_.width > depth_w_) target_.width = depth_w_;
    if (target_.height > depth_h_) target_.height = depth_h_;

    if (depth_ == nullptr) return;
    const int pixels = clamp_int(target_.width * target_.height, 0,
                                 depth_w_ * depth_h_);
    std::memset(depth_, 0xFF, static_cast<size_t>(pixels));
}

void Rasterizer::set_depth_buffer(uint8_t* buffer, int width, int height) {
    depth_ = buffer;
    depth_w_ = buffer ? width : 0;
    depth_h_ = buffer ? height : 0;
}

void Rasterizer::begin_frame_collect(const RenderTarget& target,
                                     FrameQueue& queue) {
    target_ = target;
    triangles_drawn_ = 0;

    if (target_.width > depth_w_) target_.width = depth_w_;
    if (target_.height > depth_h_) target_.height = depth_h_;

    queue.reset();
    queue_ = &queue;
    // No depth clear here. The split workers clear their own rows, so the
    // clear itself runs on both cores.
}

void Rasterizer::end_collect() {
    queue_ = nullptr;
}

void Rasterizer::clear_depth_rows(int row_begin, int row_end) {
    if (depth_ == nullptr) return;
    row_begin = clamp_int(row_begin, 0, target_.height);
    row_end = clamp_int(row_end, row_begin, target_.height);
    std::memset(depth_ + static_cast<size_t>(row_begin) * target_.width, 0xFF,
                static_cast<size_t>(row_end - row_begin) * target_.width);
}

uint8_t* Rasterizer::pixel_at(int x, int y) const {
    return target_.pixels + static_cast<size_t>(y) * target_.row_stride +
           static_cast<size_t>(x) * pse::bytes_per_pixel(target_.format);
}

bool Rasterizer::test_and_set_depth(int x, int y, uint8_t depth) {
    if (depth_ == nullptr) return false;
    if (x < 0 || x >= target_.width || y < 0 || y >= target_.height) return false;
    const int index = y * target_.width + x;
    if (depth >= depth_[index]) return false;
    depth_[index] = depth;
    return true;
}

void Rasterizer::plot(int x, int y, uint8_t r, uint8_t g, uint8_t b) {
    if (target_.pixels == nullptr) return;
    if (x < 0 || x >= target_.width || y < 0 || y >= target_.height) return;
    uint8_t* dst = pixel_at(x, y);
    switch (target_.format) {
        case PixelFormat::rgb565: Rgb565::store(dst, r, g, b); break;
        case PixelFormat::bgr555: Bgr555::store(dst, r, g, b); break;
        case PixelFormat::rgb888: Rgb888::store(dst, r, g, b); break;
        default: Rgba8888::store(dst, r, g, b); break;
    }
}

bool Rasterizer::rejected(const ScreenTriangle& tri) const {
    // The signed area doubles as the backface test. World space winding is
    // counter clockwise and the projector flips Y, so a front face is positive.
    const int area = (tri.x2 - tri.x0) * (tri.y1 - tri.y0) -
                     (tri.y2 - tri.y0) * (tri.x1 - tri.x0);
    if (area <= 0) return true;

    if (max3(tri.x0, tri.x1, tri.x2) < 0) return true;
    if (min3(tri.x0, tri.x1, tri.x2) >= target_.width) return true;
    if (max3(tri.y0, tri.y1, tri.y2) < 0) return true;
    if (min3(tri.y0, tri.y1, tri.y2) >= target_.height) return true;
    return false;
}

void Rasterizer::draw(const ScreenTriangle& tri) {
    if (target_.pixels == nullptr || depth_ == nullptr) return;
    if (rejected(tri)) return;

    if (queue_ != nullptr) {
        if (queue_->push(tri)) triangles_drawn_++;
        return;
    }

    triangles_drawn_++;
    draw_rows(tri, 0, target_.height);
}

void Rasterizer::draw_rows(const ScreenTriangle& tri, int row_begin,
                           int row_end) {
    if (target_.pixels == nullptr || depth_ == nullptr) return;
    switch (target_.format) {
        case PixelFormat::rgb565: draw_typed<Rgb565>(tri, row_begin, row_end); break;
        case PixelFormat::bgr555: draw_typed<Bgr555>(tri, row_begin, row_end); break;
        case PixelFormat::rgb888: draw_typed<Rgb888>(tri, row_begin, row_end); break;
        default: draw_typed<Rgba8888>(tri, row_begin, row_end); break;
    }
}

void Rasterizer::clear_gradient(uint8_t top_r, uint8_t top_g, uint8_t top_b,
                                uint8_t bottom_r, uint8_t bottom_g,
                                uint8_t bottom_b) {
    clear_gradient_rows(top_r, top_g, top_b, bottom_r, bottom_g, bottom_b,
                        0, target_.height);
}

void Rasterizer::clear_gradient_rows(uint8_t top_r, uint8_t top_g,
                                     uint8_t top_b, uint8_t bottom_r,
                                     uint8_t bottom_g, uint8_t bottom_b,
                                     int row_begin, int row_end) {
    clear_gradient_span(top_r, top_g, top_b, bottom_r, bottom_g, bottom_b,
                        row_begin, row_end, 0, target_.height);
}

void Rasterizer::clear_gradient_span(uint8_t top_r, uint8_t top_g,
                                     uint8_t top_b, uint8_t bottom_r,
                                     uint8_t bottom_g, uint8_t bottom_b,
                                     int row_begin, int row_end,
                                     int span_begin, int span_end) {
    if (target_.pixels == nullptr) return;
    row_begin = clamp_int(row_begin, 0, target_.height);
    row_end = clamp_int(row_end, row_begin, target_.height);
    if (span_end <= span_begin) return;
    switch (target_.format) {
        case PixelFormat::rgb565:
            clear_gradient_typed<Rgb565>(top_r, top_g, top_b,
                                         bottom_r, bottom_g, bottom_b,
                                         row_begin, row_end,
                                         span_begin, span_end);
            break;
        case PixelFormat::bgr555:
            clear_gradient_typed<Bgr555>(top_r, top_g, top_b,
                                         bottom_r, bottom_g, bottom_b,
                                         row_begin, row_end,
                                         span_begin, span_end);
            break;
        case PixelFormat::rgb888:
            clear_gradient_typed<Rgb888>(top_r, top_g, top_b,
                                         bottom_r, bottom_g, bottom_b,
                                         row_begin, row_end,
                                         span_begin, span_end);
            break;
        default:
            clear_gradient_typed<Rgba8888>(top_r, top_g, top_b,
                                           bottom_r, bottom_g, bottom_b,
                                           row_begin, row_end,
                                           span_begin, span_end);
            break;
    }
}

template <typename Format>
void Rasterizer::clear_gradient_typed(uint8_t top_r, uint8_t top_g,
                                      uint8_t top_b, uint8_t bottom_r,
                                      uint8_t bottom_g, uint8_t bottom_b,
                                      int row_begin, int row_end,
                                      int span_begin, int span_end) {
    const int width = target_.width;
    const int height = target_.height;
    if (height <= 0 || width <= 0) return;
    const int span = span_end - span_begin;

    for (int y = row_begin; y < row_end; y++) {
        // Integer lerp down the gradient span, so bands rendered by different
        // workers join seamlessly when they share one span. One divide per
        // row, none per pixel.
        const int t = clamp_int(y - span_begin, 0, span);
        const int r = top_r + (bottom_r - top_r) * t / span;
        const int g = top_g + (bottom_g - top_g) * t / span;
        const int b = top_b + (bottom_b - top_b) * t / span;

        uint8_t* row = target_.pixels + static_cast<size_t>(y) * target_.row_stride;
        // Convert once, then replicate. Cheaper than running the format
        // conversion for every pixel in the row.
        Format::store(row, static_cast<uint8_t>(r), static_cast<uint8_t>(g),
                      static_cast<uint8_t>(b));
        for (int x = 1; x < width; x++) {
            std::memcpy(row + static_cast<size_t>(x) * Format::k_bytes_per_pixel,
                        row, Format::k_bytes_per_pixel);
        }
    }
}

template <typename Format>
void Rasterizer::draw_typed(const ScreenTriangle& tri, int row_begin,
                            int row_end) {
    const int x0 = tri.x0, y0 = tri.y0;
    const int x1 = tri.x1, y1 = tri.y1;
    const int x2 = tri.x2, y2 = tri.y2;

    const int area = (x2 - x0) * (y1 - y0) - (y2 - y0) * (x1 - x0);
    if (area <= 0) return;

    row_begin = clamp_int(row_begin, 0, target_.height);
    row_end = clamp_int(row_end, row_begin, target_.height);

    const int min_x = clamp_int(min3(x0, x1, x2), 0, target_.width - 1);
    const int max_x = clamp_int(max3(x0, x1, x2), 0, target_.width - 1);
    const int min_y = clamp_int(min3(y0, y1, y2), row_begin, row_end - 1);
    const int max_y = clamp_int(max3(y0, y1, y2), row_begin, row_end - 1);
    if (max_x < min_x || max_y < min_y) return;
    if (row_end <= row_begin) return;

    // Reciprocal depth, so interpolation across a span is perspective correct.
    // Three divides per triangle rather than one per pixel.
    const int z0 = clamp_int(tri.z0, 1, k_fixed_one);
    const int z1 = clamp_int(tri.z1, 1, k_fixed_one);
    const int z2 = clamp_int(tri.z2, 1, k_fixed_one);
    const int inv_z0 = (k_fixed_one * k_fixed_one) / z0;
    const int inv_z1 = (k_fixed_one * k_fixed_one) / z1;
    const int inv_z2 = (k_fixed_one * k_fixed_one) / z2;

    // Texturing rides on work this loop already does. u and v have to be
    // interpolated in 1/z to be perspective correct, and the reciprocal depths
    // above plus the z recovered per pixel are exactly what that needs, so the
    // inner loop gains multiplies and NOT a divide. On a core with no divide
    // instruction that is the difference between affordable and not.
    //
    // Premultiplied per triangle here, three multiplies each, so the per pixel
    // cost is three multiplies and an add per coordinate.
    // Scaled down by k_fixed_shift on the way in, which is what keeps every
    // step of this in int32 on a 32 bit core:
    //   inv_z    <= 2^20         (k_fixed_one^2 / 1)
    //   u * inv_z >> 10          <= 255 * 2^10, about 2^18
    //   w * that                 <= 2^10 * 2^18 = 2^28, and three of them sum
    //                               to under 2^30
    //   that * z                 is bounded at 255 * 2^20, about 2^28, because
    //                               uz and z are reciprocal: where one is large
    //                               the other is small.
    // Without the shift the third line reaches 2^38 and wraps, which shows up
    // as texture coordinates tearing on whichever surface happens to be
    // closest to the camera.
    const Texture* tex = texture(tri.tex);
    const int u0z = (tri.u0 * inv_z0) >> k_fixed_shift;
    const int u1z = (tri.u1 * inv_z1) >> k_fixed_shift;
    const int u2z = (tri.u2 * inv_z2) >> k_fixed_shift;
    const int v0z = (tri.v0 * inv_z0) >> k_fixed_shift;
    const int v1z = (tri.v1 * inv_z1) >> k_fixed_shift;
    const int v2z = (tri.v2 * inv_z2) >> k_fixed_shift;

    for (int y = min_y; y <= max_y; y++) {
        uint8_t* row = target_.pixels + static_cast<size_t>(y) * target_.row_stride;
        uint8_t* depth_row = depth_ + static_cast<size_t>(y) * target_.width;
        bool inside_run = false;

        for (int x = min_x; x <= max_x; x++) {
            const int e0 = (x - x1) * (y2 - y1) - (y - y1) * (x2 - x1);
            if (e0 < 0) { if (inside_run) break; continue; }
            const int e1 = (x - x2) * (y0 - y2) - (y - y2) * (x0 - x2);
            if (e1 < 0) { if (inside_run) break; continue; }
            const int e2 = (x - x0) * (y1 - y0) - (y - y0) * (x1 - x0);
            if (e2 < 0) { if (inside_run) break; continue; }

            // Once the span has been entered, leaving it ends the row.
            inside_run = true;

            const int w0 = (k_fixed_one * e0) / area;
            const int w1 = (k_fixed_one * e1) / area;
            const int w2 = k_fixed_one - (w0 + w1);

            const int inv_z = w0 * inv_z0 + w1 * inv_z1 + w2 * inv_z2;
            if (inv_z <= 0) continue;
            const int z = (k_fixed_one * k_fixed_one * k_fixed_one) / inv_z;

            const int scaled = z * 255 / k_fixed_one;
            const uint8_t depth = static_cast<uint8_t>(clamp_int(scaled, 0, 255));

            if (depth >= depth_row[x]) continue;
            depth_row[x] = depth;

            int r = clamp_int(
                (w0 * tri.r0 + w1 * tri.r1 + w2 * tri.r2) / k_fixed_one, 0, 255);
            int g = clamp_int(
                (w0 * tri.g0 + w1 * tri.g1 + w2 * tri.g2) / k_fixed_one, 0, 255);
            int b = clamp_int(
                (w0 * tri.b0 + w1 * tri.b1 + w2 * tri.b2) / k_fixed_one, 0, 255);

            if (tex != nullptr) {
                // Interpolate u/z and v/z, then multiply back by the z this
                // pixel already had to recover u and v. No divide: z came out
                // of the depth test three lines up.
                const int uz = w0 * u0z + w1 * u1z + w2 * u2z;
                const int vz = w0 * v0z + w1 * v1z + w2 * v2z;
                const int u = (uz * z) >> (2 * k_fixed_shift);
                const int v = (vz * z) >> (2 * k_fixed_shift);
                uint8_t tr, tg, tb;
                texture_fetch(*tex, u, v, tr, tg, tb);
                // Modulate, so the texel is lit by whatever the vertex colours
                // carried. >> 8 rather than / 255: one count darker at the top
                // of the range, and no divide in the inner loop.
                r = (r * tr) >> 8;
                g = (g * tg) >> 8;
                b = (b * tb) >> 8;
            }

            Format::store(row + static_cast<size_t>(x) * Format::k_bytes_per_pixel,
                          static_cast<uint8_t>(r), static_cast<uint8_t>(g),
                          static_cast<uint8_t>(b));
        }
    }
}

}  // namespace pse
