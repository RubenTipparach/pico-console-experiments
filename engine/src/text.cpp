#include "pse/text.hpp"

#include <cstddef>

#include "pse_font.hpp"

namespace pse {
namespace {

// Store one pixel in whichever layout the target is in. The rasterizer picks
// its format once per frame and specialises; the menu draws a few thousand
// pixels a frame, so a switch per pixel is not worth a template here.
inline void store(uint8_t* dst, PixelFormat format, uint8_t r, uint8_t g,
                  uint8_t b) {
    switch (format) {
        case PixelFormat::rgb565: Rgb565::store(dst, r, g, b); break;
        case PixelFormat::bgr555: Bgr555::store(dst, r, g, b); break;
        case PixelFormat::rgb888: Rgb888::store(dst, r, g, b); break;
        default: Rgba8888::store(dst, r, g, b); break;
    }
}

// Glyph slot for a character, or -1 when the font does not carry it.
inline int slot_of(char c) {
    const unsigned char code = static_cast<unsigned char>(c);
    if (code >= 128) return -1;
    const int slot = font::k_index[code];
    return slot == 0 ? -1 : slot - 1;
}

}  // namespace

int text_width(const char* text, int scale) {
    if (text == nullptr || *text == '\0') return 0;
    int n = 0;
    for (const char* p = text; *p != '\0'; p++) n++;
    // The trailing gap after the last glyph is not part of the string.
    return (n * k_glyph_advance - 1) * scale;
}

bool text_is_drawable(const char* text) {
    if (text == nullptr) return false;
    for (const char* p = text; *p != '\0'; p++) {
        if (slot_of(*p) < 0) return false;
    }
    return true;
}

void draw_text(const RenderTarget& target, const char* text, int x, int y,
               uint8_t r, uint8_t g, uint8_t b, int scale) {
    if (text == nullptr || scale <= 0) return;
    int pen_x = x;
    for (const char* p = text; *p != '\0'; p++, pen_x += k_glyph_advance * scale) {
        const int slot = slot_of(*p);
        if (slot < 0) continue;  // A character with no picture is a blank.
        const uint8_t* rows = font::k_rows[slot];
        for (int gy = 0; gy < k_glyph_h; gy++) {
            const uint8_t bits = rows[gy];
            if (bits == 0) continue;
            for (int gx = 0; gx < k_glyph_w; gx++) {
                if ((bits & (1 << (k_glyph_w - 1 - gx))) == 0) continue;
                if (scale == 1) {
                    plot_pixel(target, pen_x + gx, y + gy, r, g, b);
                } else {
                    fill_rect(target, pen_x + gx * scale, y + gy * scale, scale,
                              scale, r, g, b);
                }
            }
        }
    }
}

void draw_text_centred(const RenderTarget& target, const char* text,
                       int centre_x, int y, uint8_t r, uint8_t g, uint8_t b,
                       int scale) {
    draw_text(target, text, centre_x - text_width(text, scale) / 2, y, r, g, b,
              scale);
}

}  // namespace pse
