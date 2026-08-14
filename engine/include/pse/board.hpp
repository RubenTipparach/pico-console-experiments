#pragma once

// Which device this build is for, and the one thing a game needs to know
// about it.
//
// There are two: the PicoSystem (240x240) and the Tufty 2350 (320x240). They
// are not the same shape, and the games were all written against the first
// one. Rather than teach twelve games about a screen size, every game asks
// for a surface of the size it was designed for and the SDK centres that on
// whatever panel is actually fitted. On a PicoSystem the design size IS the
// panel, so nothing happens and the build is unchanged. On a Tufty the same
// picture sits in the middle with a black column either side.
//
// That is a deliberate choice of correctness over screen area. A game that
// wants the extra 80 columns has to be written to want them, and none of
// them are: they measure against a literal 120 in enough places (kingfisher
// most of all) that a wider surface would not be a wider view, it would be a
// misplaced one.
//
// The other consequence is the useful one: PSE_RENDER_WIDTH and
// PSE_RENDER_HEIGHT stay 120x120 on both boards, so the depth buffer, the
// triangle queue and every budget in config.hpp are unchanged, and none of
// this costs a byte of RAM on the PicoSystem.

namespace pse {

// The screen every game in this repo was written against, and the surface
// each one asks the SDK for. Not the panel: see k_letterboxed below.
constexpr int k_design_width = 240;
constexpr int k_design_height = 240;

// True when the fitted panel is bigger than the design size, so the picture
// is centred with a border rather than filling the screen.
//
// PSE_BOARD_TUFTY2350 is set by engine/CMakeLists.txt from PICO_BOARD, not
// read from the SDK: the SDK's own board define travels with BlitHalPico,
// which the engine deliberately does not link (see engine/CMakeLists.txt on
// pico_multicore_headers for the same distinction).
#if defined(PSE_BOARD_TUFTY2350)
constexpr bool k_letterboxed = true;
#else
constexpr bool k_letterboxed = false;
#endif

// True when the board's physical buttons are not the ones the games read, so
// something has to stand in for the missing ones. The mapping itself, and why
// it is the one it is, lives in blit_target.cpp.
#if defined(PSE_BOARD_TUFTY2350)
constexpr bool k_remaps_buttons = true;
#else
constexpr bool k_remaps_buttons = false;
#endif

}  // namespace pse
