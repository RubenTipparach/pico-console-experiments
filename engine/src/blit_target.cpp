#include "pse/blit_target.hpp"

#include "32blit.hpp"

#include "pse/board.hpp"

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

void set_screen_mode(ScreenMode mode) {
    const blit::ScreenMode sdk_mode =
        mode == ScreenMode::hires ? blit::ScreenMode::hires : blit::ScreenMode::lores;

    // (PixelFormat)-1 rather than naming a format, because the platforms do
    // not agree on one: the pico HAL defaults to RGB565 and the SDL HAL to
    // RGB, and each has a good reason. It is what the SDK's own bounds taking
    // overload passes, and picking one here would quietly change the desktop
    // build's pixel format.
    const bool ok = blit::set_screen_mode(sdk_mode, static_cast<blit::PixelFormat>(-1),
                                          blit::Size(k_design_width, k_design_height));
    if (ok) return;

    // A panel that cannot give us the design size. Nothing in this repo builds
    // one today: the PicoSystem is exactly 240x240, the Tufty is larger and
    // its driver centres what it is given, and both SDL builds are launched
    // with --size 240,240. Ask for the platform's own default rather than
    // leaving the game with no screen at all, because the failure mode of
    // returning here is a black window and no message.
    blit::set_screen_mode(sdk_mode);
}

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
