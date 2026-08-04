#include "library.hpp"

#include <cstring>

namespace launcher {
namespace {

const char k_magic[8] = {'P', 'S', 'E', 'G', 'A', 'M', 'E', '1'};

// Field offsets inside the block, from tools/game_meta.py. Written out rather
// than derived so a change on either side shows up as a mismatch here.
constexpr size_t k_off_size = 8;
constexpr size_t k_off_icon_w = 10;
constexpr size_t k_off_icon_h = 12;
constexpr size_t k_off_slug = 16;
constexpr size_t k_off_title = 40;
constexpr size_t k_off_version = 72;

uint16_t read_u16(const uint8_t* data) {
    return static_cast<uint16_t>(data[0] | (data[1] << 8));
}

uint32_t read_u32(const uint8_t* data) {
    return static_cast<uint32_t>(data[0]) |
           (static_cast<uint32_t>(data[1]) << 8) |
           (static_cast<uint32_t>(data[2]) << 16) |
           (static_cast<uint32_t>(data[3]) << 24);
}

// Copy a NUL padded field, always leaving the destination terminated. The
// block is data from flash, so it is never trusted to be well formed: a
// corrupt image should give a garbled name, not a menu that reads past it.
void copy_field(char* out, size_t out_size, const uint8_t* field,
                size_t field_size) {
    const size_t limit = field_size < out_size - 1 ? field_size : out_size - 1;
    size_t i = 0;
    for (; i < limit && field[i] != 0; i++) out[i] = static_cast<char>(field[i]);
    out[i] = '\0';
}

const uint8_t* find_magic(Span slot) {
    if (slot.size < k_meta_size) return nullptr;
    const size_t last = slot.size - k_meta_size;
    for (size_t i = 0; i <= last; i += 4) {
        // The block is 4 byte aligned by the compiler, so stepping by 4 is
        // safe and makes a whole slot scan cheap.
        if (std::memcmp(slot.data + i, k_magic, sizeof(k_magic)) == 0) {
            return slot.data + i;
        }
    }
    return nullptr;
}

}  // namespace

uint32_t slot_address(int slot) {
    return k_flash_base + static_cast<uint32_t>(slot) * k_slot_size;
}

bool read_slot(Span slot, int slot_index, Entry& out) {
    const uint8_t* block = find_magic(slot);
    if (block == nullptr) return false;

    const uint16_t size = read_u16(block + k_off_size);
    if (size < k_meta_size) return false;

    copy_field(out.slug, sizeof(out.slug), block + k_off_slug, 24);
    copy_field(out.title, sizeof(out.title), block + k_off_title, 32);
    copy_field(out.version, sizeof(out.version), block + k_off_version, 16);
    out.slot = slot_index;

    // Only hand back an icon that is the size this build knows how to draw.
    // A future block with a bigger icon should show up as a missing picture,
    // never as a read past the end of one.
    const bool icon_ok = read_u16(block + k_off_icon_w) == k_icon_w &&
                         read_u16(block + k_off_icon_h) == k_icon_h;
    out.icon = icon_ok ? block + k_meta_header : nullptr;

    // A game with no title is not worth showing: it would be an unlabelled
    // card the player cannot identify.
    return out.title[0] != '\0';
}

int scan(const Span* slots, int slot_count, Entry* out, int max_entries) {
    int found = 0;
    for (int i = 0; i < slot_count && found < max_entries; i++) {
        // Slot 0 is the launcher itself, which is not a menu entry.
        const int slot_index = i + 1;
        if (read_slot(slots[i], slot_index, out[found])) found++;
    }
    return found;
}

bool boot_vectors(Span slot, uint32_t& stack_pointer, uint32_t& reset_handler) {
    if (slot.size < k_vector_offset + 8) return false;
    const uint32_t sp = read_u32(slot.data + k_vector_offset);
    const uint32_t pc = read_u32(slot.data + k_vector_offset + 4);

    // An erased slot reads as all ones, and a half written one can be
    // anything. Both words are checked against the address spaces they must
    // live in, because jumping to rubbish is the one failure with no way back
    // except a reflash.
    const bool sp_ok = (sp & 0xFFFF0000u) == 0x20000000u ||
                       (sp & 0xFFFF0000u) == 0x20040000u;
    const bool pc_ok = pc >= k_flash_base &&
                       pc < k_flash_base + 16u * 1024u * 1024u &&
                       (pc & 1u) == 1u;   // thumb bit, always set on M0+
    if (!sp_ok || !pc_ok) return false;

    stack_pointer = sp;
    reset_handler = pc;
    return true;
}

}  // namespace launcher
