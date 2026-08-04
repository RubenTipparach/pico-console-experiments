#pragma once

// What is installed, read out of flash itself.
//
// Every game compiles a metadata block into a `.pse_meta` section (see
// tools/game_meta.py): magic, slug, title, version, and a 48x48 RGB565 icon.
// A bundled game sits in a slot of its own, so finding the library is a matter
// of looking at the start of each slot for that magic. Nothing else has to be
// written down: the games describe themselves, and an empty slot is simply one
// with no magic in it.
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
bool read_slot(Span slot, int slot_index, Entry& out);

// Reads every slot into `out`, returning how many games were found. Games
// keep their slot order, so the menu order is the flash order and a game does
// not move around between boots.
int scan(const Span* slots, int slot_count, Entry* out, int max_entries);

// Where slot n starts in the address space. Split out so the tests can use
// the same arithmetic the device does.
uint32_t slot_address(int slot);

// The two words the handoff needs, read from a live slot: [0] is the stack
// pointer, [1] the reset handler. Returns false if the slot holds nothing
// that looks like a vector table.
bool boot_vectors(Span slot, uint32_t& stack_pointer, uint32_t& reset_handler);

}  // namespace launcher
