#pragma once

#include <cstdint>

namespace pse {

// Pixel format policies.
//
// The rasterizer is templated on one of these rather than calling a virtual
// per pixel. A virtual call in the inner loop of a software rasterizer on a
// Cortex-M0+ is not a small cost, it is the cost. Policy structs give the same
// substitutability with none of the indirection: the compiler emits one
// specialised loop per format and the format is picked once per frame.
//
// These are the three layouts a 32blit Surface presents. On the pico devices
// `screen` is RGB565, on desktop and Emscripten it is 24 bit RGB.

// The SDK's RGB565 puts RED IN THE LOW BITS. This is not conventional RGB565
// and it is not what the enum name suggests, so it is worth being explicit:
//
//     32blit/graphics/blend.cpp, pack_rgb565():
//         (r >> 3) | ((g >> 2) << 5) | ((b >> 3) << 11)
//
// which is red at bits 0..4, green at 5..10, blue at 11..15. It comes out
// right on the PicoSystem because the panel is put in BGR order: the dbi
// driver always sets MADCTL bit 3 (`MADCTL::RGB = 0b00001000`), and bit 3 on
// an ST7789 selects BGR, so the panel reads the halfword back the other way
// round. The two wrongs are load bearing and cancel exactly.
//
// Writing conventional RGB565 here instead swaps red and blue on hardware and
// nowhere else, because desktop and the browser hand us 24 bit RGB. That is a
// bug that cannot be seen without a device, and it shipped once: keep this in
// step with the SDK, not with what the format is called.
// Widening a truncated channel back to eight bits.
//
// The high bits are REPLICATED into the low ones rather than shifted up and
// padded with zeros, which is the difference between white surviving a round
// trip and drifting to 248. It matters here because these formats are read back
// as well as written now, for the blended fill in draw2d: a panel blended over
// white on a 16 bit target would otherwise come out a shade darker than the
// same panel blended over white on the desktop, and a colour that depends on
// which machine drew it is exactly the class of bug this file's own comments
// are about.
constexpr inline uint8_t widen5(uint8_t v) {
    return static_cast<uint8_t>((v << 3) | (v >> 2));
}
constexpr inline uint8_t widen6(uint8_t v) {
    return static_cast<uint8_t>((v << 2) | (v >> 4));
}

struct Rgb565 {
    static constexpr int k_bytes_per_pixel = 2;

    static inline void store(uint8_t* dst, uint8_t r, uint8_t g, uint8_t b) {
        const uint16_t value = static_cast<uint16_t>(
            (r >> 3) | ((g & 0xFC) << 3) | ((b & 0xF8) << 8));
        // Little endian halfword, written bytewise so the target does not need
        // to be 2 byte aligned.
        dst[0] = static_cast<uint8_t>(value & 0xFF);
        dst[1] = static_cast<uint8_t>(value >> 8);
    }

    static inline void load(const uint8_t* src, uint8_t& r, uint8_t& g,
                            uint8_t& b) {
        const uint16_t value = static_cast<uint16_t>(src[0] | (src[1] << 8));
        r = widen5(static_cast<uint8_t>(value & 0x1F));
        g = widen6(static_cast<uint8_t>((value >> 5) & 0x3F));
        b = widen5(static_cast<uint8_t>((value >> 11) & 0x1F));
    }
};

struct Rgb888 {
    static constexpr int k_bytes_per_pixel = 3;

    static inline void store(uint8_t* dst, uint8_t r, uint8_t g, uint8_t b) {
        dst[0] = r;
        dst[1] = g;
        dst[2] = b;
    }

    static inline void load(const uint8_t* src, uint8_t& r, uint8_t& g,
                            uint8_t& b) {
        r = src[0];
        g = src[1];
        b = src[2];
    }
};

struct Bgr555 {
    static constexpr int k_bytes_per_pixel = 2;

    static inline void store(uint8_t* dst, uint8_t r, uint8_t g, uint8_t b) {
        const uint16_t value = static_cast<uint16_t>(
            ((b & 0xF8) << 7) | ((g & 0xF8) << 2) | (r >> 3));
        dst[0] = static_cast<uint8_t>(value & 0xFF);
        dst[1] = static_cast<uint8_t>(value >> 8);
    }

    // Red low, green at 5, blue at 10, matching the store above rather than
    // what the format is called. Same trap as RGB565's: read it the
    // conventional way round and red and blue swap on one board and nowhere
    // else.
    static inline void load(const uint8_t* src, uint8_t& r, uint8_t& g,
                            uint8_t& b) {
        const uint16_t value = static_cast<uint16_t>(src[0] | (src[1] << 8));
        r = widen5(static_cast<uint8_t>(value & 0x1F));
        g = widen5(static_cast<uint8_t>((value >> 5) & 0x1F));
        b = widen5(static_cast<uint8_t>((value >> 10) & 0x1F));
    }
};

struct Rgba8888 {
    static constexpr int k_bytes_per_pixel = 4;

    static inline void store(uint8_t* dst, uint8_t r, uint8_t g, uint8_t b) {
        dst[0] = r;
        dst[1] = g;
        dst[2] = b;
        dst[3] = 255;
    }

    static inline void load(const uint8_t* src, uint8_t& r, uint8_t& g,
                            uint8_t& b) {
        r = src[0];
        g = src[1];
        b = src[2];
    }
};

// Which specialisation to run.
//
// Note that bytes per pixel alone does NOT identify the format: the 32blit SDK
// has two 2 byte screen formats, RGB565 and BGR555. Guessing from the stride
// would put the red and blue channels the wrong way round on any board that
// uses BGR555, which is the sort of bug that only shows up on hardware nobody
// tested on. The adapter maps the SDK enum explicitly instead.
enum class PixelFormat : uint8_t {
    rgb565,
    bgr555,
    rgb888,
    rgba8888,
};

inline int bytes_per_pixel(PixelFormat format) {
    switch (format) {
        case PixelFormat::rgb565: return Rgb565::k_bytes_per_pixel;
        case PixelFormat::bgr555: return Bgr555::k_bytes_per_pixel;
        case PixelFormat::rgb888: return Rgb888::k_bytes_per_pixel;
        default: return Rgba8888::k_bytes_per_pixel;
    }
}

// A borrowed drawing surface. The renderer never owns or allocates pixels: it
// draws into whatever the SDK handed it this frame. `row_stride` is carried
// separately because a Surface's rows are not guaranteed to be packed.
struct RenderTarget {
    uint8_t* pixels;
    int width;
    int height;
    int row_stride;
    PixelFormat format;
};

}  // namespace pse
