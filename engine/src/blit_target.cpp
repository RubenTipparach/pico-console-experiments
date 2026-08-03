#include "pse/blit_target.hpp"

#include "32blit.hpp"

namespace pse {
namespace {

// Map the SDK's format enum onto ours explicitly.
//
// RGB565 and BGR555 are both two bytes per pixel, so deriving this from
// `pixel_stride` would silently swap red and blue on a BGR555 board. Paletted
// and mask surfaces are not drawable targets for a software rasterizer that
// writes colours directly, so they fall back to the most common screen format
// rather than pretending to work.
PixelFormat translate(blit::PixelFormat format) {
    switch (format) {
        case blit::PixelFormat::RGB565: return PixelFormat::rgb565;
        case blit::PixelFormat::BGR555: return PixelFormat::bgr555;
        case blit::PixelFormat::RGB: return PixelFormat::rgb888;
        case blit::PixelFormat::RGBA: return PixelFormat::rgba8888;
        default: return PixelFormat::rgb565;
    }
}

}  // namespace

RenderTarget target_from_screen() {
    const blit::Surface& surface = blit::screen;

    RenderTarget target;
    target.pixels = surface.data;
    target.width = surface.bounds.w;
    target.height = surface.bounds.h;
    target.row_stride = surface.row_stride;
    target.format = translate(surface.format);
    return target;
}

}  // namespace pse
