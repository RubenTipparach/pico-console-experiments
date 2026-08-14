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

// The Tufty 2350's five front buttons, standing in for a dpad and four face
// buttons.
//
// Physically there is up, down, A, B and C, and C arrives as blit::Button::X
// because that is the pin the board config wires it to. There is no left and
// no right, and all twelve games read DPAD_LEFT and DPAD_RIGHT, so the two
// buttons to the right of A become the horizontal axis.
//
// Additive, not a permutation, and that is the point. B still reports as B
// and C still reports as X, so a game that reads them keeps them; they simply
// also read as left and right. It means a game whose B does something
// distinct from moving left gets both at once, which is a genuine conflict
// and the honest cost of five buttons. The alternative, moving B onto left
// and taking B away, breaks those games outright instead.
//
// Being additive is also what lets an accessory just work. The board config
// enables the tca9555 driver, so a Qw/ST Pad on the I2C connector ORs a real
// dpad and a real A/B/X/Y into this same word. Real left arrives as left, and
// nothing here fights it.
constexpr uint32_t map_button_word(uint32_t word) {
    uint32_t mapped = word;
    if (word & blit::Button::B) mapped |= blit::Button::DPAD_LEFT;
    if (word & blit::Button::X) mapped |= blit::Button::DPAD_RIGHT;
    return mapped;
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

MappedButtons::MappedButtons() {
    if constexpr (!k_remaps_buttons) return;

    saved_state_ = blit::buttons.state;
    saved_pressed_ = blit::buttons.pressed;
    saved_released_ = blit::buttons.released;

    // The same word transform on all three, which is why no history is kept
    // here: pressed and released are already the SDK's edges for this tick,
    // and a pure function of the button word maps an edge exactly as it maps
    // a state.
    blit::buttons.state = map_button_word(saved_state_);
    blit::buttons.pressed = map_button_word(saved_pressed_);
    blit::buttons.released = map_button_word(saved_released_);
}

MappedButtons::~MappedButtons() {
    if constexpr (!k_remaps_buttons) return;

    blit::buttons.state = saved_state_;
    blit::buttons.pressed = saved_pressed_;
    blit::buttons.released = saved_released_;
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
