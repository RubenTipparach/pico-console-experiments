#pragma once

// A name for a game that cannot name itself.
//
// Every game this project builds compiles its own PSEGAME1 metadata block
// (library.hpp), and read_slot() finds that first. A game this project did
// not build carries no such block, ever, and there is no way to add one to
// its own image without recompiling it from source. What CAN be done instead
// is give the launcher its own, separate place to hold a title for a slot,
// so a game that cannot describe itself can still be named by whoever put it
// there.
//
// This table is reserved inside the launcher's own image, all zero, and
// PicoFlasher patches a slot's entry directly into the flashed bytes when
// composing a bundle that force adds a game with no metadata block of its
// own (tools/flasher/Bundle.cs). Nothing on this side ever writes it, only
// reads it, and only when a slot's own scan for PSEGAME1 finds nothing.
//
// Bytes, not a struct: the launcher's own image is scanned for the magic the
// same way a game's slot is scanned for PSEGAME1, so this stays exactly as
// testable with a synthetic byte buffer as everything else in this file, and
// the layout PicoFlasher patches is a byte offset contract, not a struct
// layout one compiler could lay out differently from another.

#include <cstddef>
#include <cstdint>

#include "library.hpp"  // k_max_slots

namespace launcher {

constexpr int k_override_title_size = 32;
constexpr size_t k_override_magic_size = 8;
constexpr size_t k_override_table_size =
    k_override_magic_size + static_cast<size_t>(k_max_slots) * k_override_title_size;

// The reserved table, compiled into every launcher build. All zero titles:
// PicoFlasher is the only thing that ever writes a non-zero one in.
extern const uint8_t g_override_table[k_override_table_size];

}  // namespace launcher
