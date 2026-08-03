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

    // Refuse to draw outside the statically sized depth buffer rather than
    // corrupting RAM if a caller hands over a bigger surface than the build was
    // configured for.
    if (target_.width > k_render_width) target_.width = k_render_width;
    if (target_.height > k_render_height) target_.height = k_render_height;

    const int pixels = clamp_int(target_.width * target_.height, 0,
                                 k_render_width * k_render_height);
    std::memset(depth_, 0xFF, static_cast<size_t>(pixels));
}

uint8_t* Rasterizer::pixel_at(int x, int y) const {
    return target_.pixels + static_cast<size_t>(y) * target_.row_stride +
           static_cast<size_t>(x) * pse::bytes_per_pixel(target_.format);
}

bool Rasterizer::test_and_set_depth(int x, int y, uint8_t depth) {
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

void Rasterizer::draw(const ScreenTriangle& tri) {
    if (target_.pixels == nullptr) return;
    switch (target_.format) {
        case PixelFormat::rgb565: draw_typed<Rgb565>(tri); break;
        case PixelFormat::bgr555: draw_typed<Bgr555>(tri); break;
        case PixelFormat::rgb888: draw_typed<Rgb888>(tri); break;
        default: draw_typed<Rgba8888>(tri); break;
    }
}

void Rasterizer::clear_gradient(uint8_t top_r, uint8_t top_g, uint8_t top_b,
                                uint8_t bottom_r, uint8_t bottom_g,
                                uint8_t bottom_b) {
    if (target_.pixels == nullptr) return;
    switch (target_.format) {
        case PixelFormat::rgb565:
            clear_gradient_typed<Rgb565>(top_r, top_g, top_b,
                                         bottom_r, bottom_g, bottom_b);
            break;
        case PixelFormat::bgr555:
            clear_gradient_typed<Bgr555>(top_r, top_g, top_b,
                                         bottom_r, bottom_g, bottom_b);
            break;
        case PixelFormat::rgb888:
            clear_gradient_typed<Rgb888>(top_r, top_g, top_b,
                                         bottom_r, bottom_g, bottom_b);
            break;
        default:
            clear_gradient_typed<Rgba8888>(top_r, top_g, top_b,
                                           bottom_r, bottom_g, bottom_b);
            break;
    }
}

template <typename Format>
void Rasterizer::clear_gradient_typed(uint8_t top_r, uint8_t top_g,
                                      uint8_t top_b, uint8_t bottom_r,
                                      uint8_t bottom_g, uint8_t bottom_b) {
    const int width = target_.width;
    const int height = target_.height;
    if (height <= 0 || width <= 0) return;

    for (int y = 0; y < height; y++) {
        // Integer lerp down the screen: one divide per row, none per pixel.
        const int r = top_r + (bottom_r - top_r) * y / height;
        const int g = top_g + (bottom_g - top_g) * y / height;
        const int b = top_b + (bottom_b - top_b) * y / height;

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
void Rasterizer::draw_typed(const ScreenTriangle& tri) {
    const int x0 = tri.x0, y0 = tri.y0;
    const int x1 = tri.x1, y1 = tri.y1;
    const int x2 = tri.x2, y2 = tri.y2;

    // The signed area doubles as the backface test. World space winding is
    // counter clockwise and the projector flips Y, so a front face is positive.
    const int area = (x2 - x0) * (y1 - y0) - (y2 - y0) * (x1 - x0);
    if (area <= 0) return;

    const int min_x = clamp_int(min3(x0, x1, x2), 0, target_.width - 1);
    const int max_x = clamp_int(max3(x0, x1, x2), 0, target_.width - 1);
    const int min_y = clamp_int(min3(y0, y1, y2), 0, target_.height - 1);
    const int max_y = clamp_int(max3(y0, y1, y2), 0, target_.height - 1);
    if (max_x < min_x || max_y < min_y) return;

    // Reciprocal depth, so interpolation across a span is perspective correct.
    // Three divides per triangle rather than one per pixel.
    const int z0 = clamp_int(tri.z0, 1, k_fixed_one);
    const int z1 = clamp_int(tri.z1, 1, k_fixed_one);
    const int z2 = clamp_int(tri.z2, 1, k_fixed_one);
    const int inv_z0 = (k_fixed_one * k_fixed_one) / z0;
    const int inv_z1 = (k_fixed_one * k_fixed_one) / z1;
    const int inv_z2 = (k_fixed_one * k_fixed_one) / z2;

    triangles_drawn_++;

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

            const int r = clamp_int(
                (w0 * tri.r0 + w1 * tri.r1 + w2 * tri.r2) / k_fixed_one, 0, 255);
            const int g = clamp_int(
                (w0 * tri.g0 + w1 * tri.g1 + w2 * tri.g2) / k_fixed_one, 0, 255);
            const int b = clamp_int(
                (w0 * tri.b0 + w1 * tri.b1 + w2 * tri.b2) / k_fixed_one, 0, 255);

            Format::store(row + static_cast<size_t>(x) * Format::k_bytes_per_pixel,
                          static_cast<uint8_t>(r), static_cast<uint8_t>(g),
                          static_cast<uint8_t>(b));
        }
    }
}

}  // namespace pse
