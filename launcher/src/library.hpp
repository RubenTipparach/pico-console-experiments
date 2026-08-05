#pragma once

// What is installed, read out of flash itself.
//
// Every game this project builds compiles a metadata block into a
// `.pse_meta` section (see tools/game_meta.py): magic, slug, title, version,
// and a 48x48 RGB565 icon, sitting somewhere in its own slot.
//
// What actually decides whether a slot is listed is override_table.hpp:
// PicoFlasher patches a title straight into the launcher's own image for
// every game it places there, not only a game that cannot describe itself,
// because a fresh scan of a slot's raw flash for that magic is not proof of
// absence, only proof the scan failed to find it. The override answers "is
// a game here, and what is it called" without depending on that scan at
// all; the slot's own block, when the scan does find it, still supplies the
// picture, slug, and version the override never carries. A slot with
// neither an override nor a block of its own is what "nothing installed
// here" actually looks like. See read_slot below.
//
// This file is pure: it takes a span of bytes and reports what it found, so
// the host tests can drive it with a synthetic flash image.

#include <cstddef>
#include <cstdint>

namespace launcher {

// Flash map. Slot 0 is the launcher at the base; games run from 1 upward.
// Kept in step with cmake/slot.cmake, which links each game at its slot.
constexpr uint32_t k_flash_base = 0x10000000u;
constexpr uint32_t k_slot_size = 512u * 1024u;
constexpr int k_max_slots = 23;

// The metadata block, as tools/game_meta.py writes it.
constexpr int k_icon_w = 48;
constexpr int k_icon_h = 48;
constexpr size_t k_meta_header = 96;
constexpr size_t k_meta_size = k_meta_header + k_icon_w * k_icon_h * 2;

// A game's image carries its own boot2 in the first 256 bytes, so its vector
// table starts right after. The launcher reads two words from there: the
// stack pointer to install, then the reset handler to jump to.
constexpr uint32_t k_vector_offset = 0x100;

struct Entry {
    char slug[24];
    char title[32];
    char version[16];
    int slot;                    // 1..k_max_slots
    const uint8_t* icon;         // RGB565, k_icon_w * k_icon_h, or nullptr
};

// How far into a slot the search goes before giving up. The block lands
// wherever the linker put it, but it is const data in a small image, so a
// scan of the whole slot is both correct and fast enough to run once at boot.
struct Span {
    const uint8_t* data;
    size_t size;
};

// Reads one slot. Returns false when there is no game there.
//
// `overrides` (the launcher's own image, override_table.hpp) is checked
// first: a title PicoFlasher patches in directly for every game it placed,
// so a slot is listed on its say so without this needing to find that
// slot's own PSEGAME1 block at all. When the block is also found, it fills
// in the icon, slug, and version the override never carries, but not the
// title: the override wins that if the two ever disagree. A slot with no
// override falls back to reading its own block outright, the way a slot
// flashed by hand outside PicoFlasher still works. `overrides` defaults to
// empty, which finds nothing, for callers (most host tests) that only care
// about a slot's own block.
bool read_slot(Span slot, int slot_index, Entry& out, Span overrides = Span{});

// Reads every slot into `out`, returning how many games were found. Games
// keep their slot order, so the menu order is the flash order and a game does
// not move around between boots.
int scan(const Span* slots, int slot_count, Entry* out, int max_entries,
        Span overrides = Span{});

// Where slot n starts in the address space. Split out so the tests can use
// the same arithmetic the device does.
uint32_t slot_address(int slot);

// The two words the handoff needs, read from a live slot: [0] is the stack
// pointer, [1] the reset handler. Returns false if the slot holds nothing
// that looks like a vector table.
bool boot_vectors(Span slot, uint32_t& stack_pointer, uint32_t& reset_handler);

}  // namespace launcher
