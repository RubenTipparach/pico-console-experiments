#pragma once

// The names of what is actually in each slot, as recorded by whoever put it
// there, rather than re-derived by scanning that slot's raw flash for a
// magic string every time the launcher boots.
//
// Every game this project builds also compiles its own PSEGAME1 metadata
// block (library.hpp) into its slot. That used to be the only source
// read_slot() trusted, but a scan finding nothing is not proof nothing is
// there, only proof the scan failed, and there is no way to tell those
// apart from flash alone. PicoFlasher knows for a fact what it placed in
// each slot when it composes a bundle (tools/flasher/Bundle.cs), so it
// patches every game's title in here directly, not only a forced game with
// no block of its own. read_slot() checks this table first.
//
// This table is reserved inside the launcher's own image, all zero, and
// PicoFlasher patches a slot's entry directly into the flashed bytes.
// Nothing on this side ever writes it, only reads it.
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
