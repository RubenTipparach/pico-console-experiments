#pragma once

#include <cstdint>

#include "pse/pixel.hpp"

namespace pse {

// A 5x7 bitmap font drawn straight into a RenderTarget.
//
// The SDK has fonts, and the games use them. The console does not, for one
// reason: the menu is the screen a player meets first and the only screen with
// no gameplay to explain a mistake in it, so it has to be viewable without a
// device. Drawing through a RenderTarget means the same code that runs on the
// PicoSystem renders the menu into a buffer on a host, which is where the
// screenshots and the layout tests come from. Nothing here includes the SDK.
//
// The glyphs come from engine/font/console5x7.txt, which is ASCII art, one
// picture per character. tools/gen_font.py packs it into a const table at
// build time. Rule 11's reasoning applied to a font: art belongs in a file
// that can be looked at, not in hand written hex.

constexpr int k_glyph_w = 5;
constexpr int k_glyph_h = 7;

// One blank column between glyphs. A string of n characters is
// n * k_glyph_advance - 1 pixels wide at scale 1.
constexpr int k_glyph_advance = k_glyph_w + 1;

// Width in pixels of `text` drawn at `scale`. Rule 9: measure text, never
// place it by eye.
int text_width(const char* text, int scale = 1);

constexpr int text_height(int scale = 1) { return k_glyph_h * scale; }

// Draw `text` with its top left corner at (x, y). Characters the font does
// not carry are drawn as a blank, never as a wrong glyph.
void draw_text(const RenderTarget& target, const char* text, int x, int y,
               uint8_t r, uint8_t g, uint8_t b, int scale = 1);

// Same, centred on `centre_x`.
void draw_text_centred(const RenderTarget& target, const char* text,
                       int centre_x, int y, uint8_t r, uint8_t g, uint8_t b,
                       int scale = 1);

// True when every character of `text` is in the font. The library generator
// enforces the same charset at build time, so this is the belt to that
// braces: a name that would draw as blanks fails the build instead.
bool text_is_drawable(const char* text);

// Solid rectangle, clipped to the target. The menu is rectangles and text.
void fill_rect(const RenderTarget& target, int x, int y, int w, int h,
               uint8_t r, uint8_t g, uint8_t b);

// One pixel, clipped. No depth test, no blending.
void plot_pixel(const RenderTarget& target, int x, int y, uint8_t r, uint8_t g,
                uint8_t b);

}  // namespace pse
