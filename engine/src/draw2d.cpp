#include "pse/draw2d.hpp"

#include <cstddef>

namespace pse {
namespace {

// Store one pixel in whichever layout the target is in.
//
// A switch per pixel rather than a template, for the reason text.cpp gives:
// the rasterizer picks its format once a frame because it fills triangles, and
// this fills panels and sprites. A 240x240 screen of sprite work is tens of
// thousands of pixels, not millions, and specialising every entry point here
// would multiply the code size of a module whose whole point is being small.
inline void store(uint8_t* dst, PixelFormat format, uint8_t r, uint8_t g,
                  uint8_t b) {
    switch (format) {
        case PixelFormat::rgb565: Rgb565::store(dst, r, g, b); break;
        case PixelFormat::bgr555: Bgr555::store(dst, r, g, b); break;
        case PixelFormat::rgb888: Rgb888::store(dst, r, g, b); break;
        default: Rgba8888::store(dst, r, g, b); break;
    }
}

// And read one back, which nothing else in the engine does. See blend_rect's
// comment in the header for why the rasterizer must not learn this trick.
inline void load(const uint8_t* src, PixelFormat format, uint8_t& r, uint8_t& g,
                 uint8_t& b) {
    switch (format) {
        case PixelFormat::rgb565: Rgb565::load(src, r, g, b); break;
        case PixelFormat::bgr555: Bgr555::load(src, r, g, b); break;
        case PixelFormat::rgb888: Rgb888::load(src, r, g, b); break;
        default: Rgba8888::load(src, r, g, b); break;
    }
}

// One channel of `src` over `dst` at `a`, rounded rather than truncated.
//
// The rounding is not a nicety. Truncating, a half blend of a colour with
// itself comes back one darker than it went in, so a panel drawn over its own
// colour drifts. `+ 128` costs one add and makes the identity hold.
inline uint8_t mix(uint8_t dst, uint8_t src, uint8_t a) {
    const int v = dst * (255 - a) + src * a + 128;
    return static_cast<uint8_t>((v + (v >> 8)) >> 8);
}

inline void order(int& a, int& b) {
    if (a > b) {
        const int t = a;
        a = b;
        b = t;
    }
}

}  // namespace

void plot_pixel(const RenderTarget& target, int x, int y, uint8_t r, uint8_t g,
                uint8_t b) {
    if (x < 0 || y < 0 || x >= target.width || y >= target.height) return;
    uint8_t* dst = target.pixels + static_cast<size_t>(y) * target.row_stride +
                   static_cast<size_t>(x) * bytes_per_pixel(target.format);
    store(dst, target.format, r, g, b);
}

void fill_rect(const RenderTarget& target, int x, int y, int w, int h,
               uint8_t r, uint8_t g, uint8_t b) {
    if (w <= 0 || h <= 0) return;
    int x0 = x < 0 ? 0 : x;
    int y0 = y < 0 ? 0 : y;
    int x1 = x + w > target.width ? target.width : x + w;
    int y1 = y + h > target.height ? target.height : y + h;
    const int bpp = bytes_per_pixel(target.format);
    for (int py = y0; py < y1; py++) {
        uint8_t* dst = target.pixels +
                       static_cast<size_t>(py) * target.row_stride +
                       static_cast<size_t>(x0) * bpp;
        for (int px = x0; px < x1; px++) {
            store(dst, target.format, r, g, b);
            dst += bpp;
        }
    }
}

void blend_rect(const RenderTarget& target, int x, int y, int w, int h,
                uint8_t r, uint8_t g, uint8_t b, uint8_t alpha) {
    if (w <= 0 || h <= 0 || alpha == 0) return;
    // Opaque is a fill, and this line is PURELY the read being skipped rather
    // than done and thrown away. It is not what makes the two agree: the
    // rounding in mix() already gives exactly fill_rect's pixels at 255, which
    // is why deleting this line changes no output and is caught by nothing.
    // Said the other way round, it is safe to delete and there is no reason to.
    if (alpha == 255) { fill_rect(target, x, y, w, h, r, g, b); return; }
    int x0 = x < 0 ? 0 : x;
    int y0 = y < 0 ? 0 : y;
    int x1 = x + w > target.width ? target.width : x + w;
    int y1 = y + h > target.height ? target.height : y + h;
    const int bpp = bytes_per_pixel(target.format);
    for (int py = y0; py < y1; py++) {
        uint8_t* dst = target.pixels +
                       static_cast<size_t>(py) * target.row_stride +
                       static_cast<size_t>(x0) * bpp;
        for (int px = x0; px < x1; px++) {
            uint8_t dr = 0, dg = 0, db = 0;
            load(dst, target.format, dr, dg, db);
            store(dst, target.format, mix(dr, r, alpha), mix(dg, g, alpha),
                  mix(db, b, alpha));
            dst += bpp;
        }
    }
}

void draw_rect(const RenderTarget& target, int x, int y, int w, int h,
               uint8_t r, uint8_t g, uint8_t b) {
    if (w <= 0 || h <= 0) return;
    h_line(target, x, x + w - 1, y, r, g, b);
    h_line(target, x, x + w - 1, y + h - 1, r, g, b);
    v_line(target, x, y, y + h - 1, r, g, b);
    v_line(target, x + w - 1, y, y + h - 1, r, g, b);
}

void h_line(const RenderTarget& target, int x0, int x1, int y, uint8_t r,
            uint8_t g, uint8_t b) {
    if (y < 0 || y >= target.height) return;
    order(x0, x1);
    if (x1 < 0 || x0 >= target.width) return;
    if (x0 < 0) x0 = 0;
    if (x1 >= target.width) x1 = target.width - 1;
    const int bpp = bytes_per_pixel(target.format);
    uint8_t* dst = target.pixels + static_cast<size_t>(y) * target.row_stride +
                   static_cast<size_t>(x0) * bpp;
    for (int x = x0; x <= x1; x++) {
        store(dst, target.format, r, g, b);
        dst += bpp;
    }
}

void v_line(const RenderTarget& target, int x, int y0, int y1, uint8_t r,
            uint8_t g, uint8_t b) {
    if (x < 0 || x >= target.width) return;
    order(y0, y1);
    if (y1 < 0 || y0 >= target.height) return;
    if (y0 < 0) y0 = 0;
    if (y1 >= target.height) y1 = target.height - 1;
    const int bpp = bytes_per_pixel(target.format);
    uint8_t* dst = target.pixels + static_cast<size_t>(y0) * target.row_stride +
                   static_cast<size_t>(x) * bpp;
    for (int y = y0; y <= y1; y++) {
        store(dst, target.format, r, g, b);
        dst += target.row_stride;
    }
}

void draw_line(const RenderTarget& target, int x0, int y0, int x1, int y1,
               uint8_t r, uint8_t g, uint8_t b) {
    // The axis aligned cases are the common ones and they have a loop with no
    // error term, so they are worth taking before the general case.
    if (y0 == y1) {
        h_line(target, x0, x1, y0, r, g, b);
        return;
    }
    if (x0 == x1) {
        v_line(target, x0, y0, y1, r, g, b);
        return;
    }
    int dx = x1 - x0;
    if (dx < 0) dx = -dx;
    int dy = y1 - y0;
    if (dy > 0) dy = -dy;
    const int sx = x0 < x1 ? 1 : -1;
    const int sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        plot_pixel(target, x0, y0, r, g, b);
        if (x0 == x1 && y0 == y1) return;
        const int e2 = err * 2;
        if (e2 >= dy) {
            err += dy;
            x0 += sx;
        }
        if (e2 <= dx) {
            err += dx;
            y0 += sy;
        }
    }
}

void fill_circle(const RenderTarget& target, int cx, int cy, int radius,
                 uint8_t r, uint8_t g, uint8_t b) {
    if (radius < 0) return;
    // One span per row, with the half width walked rather than square rooted:
    // there is no FPU here and this runs per particle per frame. The walk is
    // monotonic, so the whole loop costs O(radius) steps in total and not
    // O(radius) per row.
    const int rr = radius * radius;
    int x = 0;
    for (int dy = -radius; dy <= radius; dy++) {
        const int ady = dy < 0 ? -dy : dy;
        while (x > 0 && x * x + ady * ady > rr) x--;
        while (x < radius && (x + 1) * (x + 1) + ady * ady <= rr) x++;
        h_line(target, cx - x, cx + x, cy + dy, r, g, b);
    }
}

void draw_circle(const RenderTarget& target, int cx, int cy, int radius,
                 uint8_t r, uint8_t g, uint8_t b) {
    if (radius < 0) return;
    if (radius == 0) {
        plot_pixel(target, cx, cy, r, g, b);
        return;
    }
    int x = radius;
    int y = 0;
    int err = 1 - x;
    while (x >= y) {
        plot_pixel(target, cx + x, cy + y, r, g, b);
        plot_pixel(target, cx + y, cy + x, r, g, b);
        plot_pixel(target, cx - y, cy + x, r, g, b);
        plot_pixel(target, cx - x, cy + y, r, g, b);
        plot_pixel(target, cx - x, cy - y, r, g, b);
        plot_pixel(target, cx - y, cy - x, r, g, b);
        plot_pixel(target, cx + y, cy - x, r, g, b);
        plot_pixel(target, cx + x, cy - y, r, g, b);
        y++;
        if (err < 0) {
            err += 2 * y + 1;
        } else {
            x--;
            err += 2 * (y - x) + 1;
        }
    }
}

void blit_sprite(const RenderTarget& target, const Sprite& sprite, int sx,
                 int sy, int sw, int sh, int x, int y, bool flip_x) {
    if (sprite.pixels == nullptr || sw <= 0 || sh <= 0) return;

    // Clip the region to the sprite first, so a caller naming a cell that runs
    // off the sheet reads its own picture back rather than whatever is next to
    // it in flash.
    if (sx < 0) {
        sw += sx;
        x -= sx;
        sx = 0;
    }
    if (sy < 0) {
        sh += sy;
        y -= sy;
        sy = 0;
    }
    if (sx + sw > sprite.w) sw = sprite.w - sx;
    if (sy + sh > sprite.h) sh = sprite.h - sy;
    if (sw <= 0 || sh <= 0) return;

    // Then clip to the target. The left edge cut has to move the source too,
    // and under flip_x it moves it from the other end, which is the one thing
    // in here worth reading twice.
    int cut_left = 0, cut_top = 0;
    if (x < 0) {
        cut_left = -x;
        x = 0;
    }
    if (y < 0) {
        cut_top = -y;
        y = 0;
    }
    int w = sw - cut_left;
    int h = sh - cut_top;
    if (x + w > target.width) w = target.width - x;
    if (y + h > target.height) h = target.height - y;
    if (w <= 0 || h <= 0) return;

    const int bpp = bytes_per_pixel(target.format);
    for (int row = 0; row < h; row++) {
        const uint8_t* src =
            sprite.pixels +
            (static_cast<size_t>(sy + cut_top + row) * sprite.w) * 4;
        uint8_t* dst = target.pixels +
                       static_cast<size_t>(y + row) * target.row_stride +
                       static_cast<size_t>(x) * bpp;
        for (int col = 0; col < w; col++) {
            const int src_col =
                flip_x ? (sx + sw - 1 - cut_left - col) : (sx + cut_left + col);
            const uint8_t* p = src + static_cast<size_t>(src_col) * 4;
            if (p[3] >= k_alpha_threshold) {
                store(dst, target.format, p[0], p[1], p[2]);
            }
            dst += bpp;
        }
    }
}

void blit_sprite(const RenderTarget& target, const Sprite& sprite, int x,
                 int y, bool flip_x) {
    blit_sprite(target, sprite, 0, 0, sprite.w, sprite.h, x, y, flip_x);
}

}  // namespace pse
