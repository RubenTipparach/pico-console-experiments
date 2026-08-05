// Host tests for the launcher's view of flash.
//
// The device reads its library out of raw flash and then jumps into it, which
// are the two things that cannot be debugged on hardware without a wire. So
// both run here against synthetic images: a well formed one, an empty slot, a
// corrupt block, and an erased slot that must never be jumped into.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "library.hpp"
#include "menu.hpp"
#include "override_table.hpp"

namespace {

int g_checks = 0;
int g_failures = 0;

void check(bool ok, const std::string& what) {
    g_checks++;
    if (ok) return;
    g_failures++;
    std::printf("FAIL: %s\n", what.c_str());
}

// Build a metadata block exactly as tools/game_meta.py writes one.
std::vector<uint8_t> make_block(const std::string& slug,
                                const std::string& title,
                                const std::string& version,
                                uint16_t icon_w = launcher::k_icon_w,
                                uint16_t icon_h = launcher::k_icon_h) {
    std::vector<uint8_t> block(launcher::k_meta_size, 0);
    std::memcpy(block.data(), "PSEGAME1", 8);
    const uint16_t size = static_cast<uint16_t>(launcher::k_meta_size);
    block[8] = size & 0xFF;
    block[9] = (size >> 8) & 0xFF;
    block[10] = icon_w & 0xFF;
    block[11] = (icon_w >> 8) & 0xFF;
    block[12] = icon_h & 0xFF;
    block[13] = (icon_h >> 8) & 0xFF;
    block[14] = 1;   // RGB565
    std::memcpy(block.data() + 16, slug.data(), slug.size());
    std::memcpy(block.data() + 40, title.data(), title.size());
    std::memcpy(block.data() + 72, version.data(), version.size());
    // A recognisable icon: first pixel red, last pixel blue.
    block[launcher::k_meta_header + 0] = 0x00;
    block[launcher::k_meta_header + 1] = 0xF8;
    block[launcher::k_meta_size - 2] = 0x1F;
    block[launcher::k_meta_size - 1] = 0x00;
    return block;
}

// A slot image: boot2, a vector table, some code, then the block, the way a
// linked game actually lays out.
std::vector<uint8_t> make_slot(const std::vector<uint8_t>& block,
                               uint32_t sp = 0x20042000,
                               uint32_t pc = 0x10100109) {
    std::vector<uint8_t> image(launcher::k_slot_size, 0xFF);
    for (int i = 0; i < 4; i++) {
        image[launcher::k_vector_offset + i] =
            static_cast<uint8_t>((sp >> (8 * i)) & 0xFF);
        image[launcher::k_vector_offset + 4 + i] =
            static_cast<uint8_t>((pc >> (8 * i)) & 0xFF);
    }
    // Put the block a long way in, 4 byte aligned, as the linker would.
    const size_t at = 40000;
    std::memcpy(image.data() + at, block.data(), block.size());
    return image;
}

launcher::Span span_of(const std::vector<uint8_t>& image) {
    return launcher::Span{image.data(), image.size()};
}

// The launcher's own image: the override table (with whatever titles are
// given, slot n at [n - 1]) sitting a long way in, the way the compiler
// would actually place it among everything else in the launcher.
std::vector<uint8_t> make_overrides(
    const std::vector<std::pair<int, std::string>>& titles) {
    std::vector<uint8_t> table(launcher::k_override_table_size, 0);
    std::memcpy(table.data(), "PSEOVR01", 8);
    for (const auto& [slot, title] : titles) {
        uint8_t* dest = table.data() + 8 +
            static_cast<size_t>(slot - 1) * launcher::k_override_title_size;
        std::memcpy(dest, title.data(), title.size());
    }

    std::vector<uint8_t> image(launcher::k_slot_size, 0xFF);
    const size_t at = 60000;
    std::memcpy(image.data() + at, table.data(), table.size());
    return image;
}

void test_reads_a_game_out_of_a_slot() {
    const auto image = make_slot(make_block("kingfisher", "Kingfisher", "v1.4.0"));
    launcher::Entry entry{};
    check(launcher::read_slot(span_of(image), 3, entry), "a slot with a game reads");
    check(std::string(entry.slug) == "kingfisher", "slug survives");
    check(std::string(entry.title) == "Kingfisher", "title survives");
    check(std::string(entry.version) == "v1.4.0", "version survives");
    check(entry.slot == 3, "the slot index is recorded");
    check(entry.icon != nullptr, "the icon is found");
    check(entry.icon[0] == 0x00 && entry.icon[1] == 0xF8,
          "the icon points at the pixels, not the header");
}

void test_an_empty_slot_is_not_a_game() {
    std::vector<uint8_t> image(launcher::k_slot_size, 0xFF);
    launcher::Entry entry{};
    check(!launcher::read_slot(span_of(image), 1, entry),
          "an erased slot holds no game");

    std::vector<uint8_t> zeros(launcher::k_slot_size, 0x00);
    check(!launcher::read_slot(span_of(zeros), 1, entry),
          "a zeroed slot holds no game");
}

void test_a_nameless_or_oversized_block_is_refused() {
    // No title: an unlabelled card the player could not identify.
    const auto nameless = make_slot(make_block("x", "", "v1"));
    launcher::Entry entry{};
    check(!launcher::read_slot(span_of(nameless), 1, entry),
          "a block with no title is not shown");

    // An icon this build cannot draw must come back as no icon, never as a
    // read past the end of the block.
    const auto big = make_slot(make_block("x", "Big Icon", "v1", 64, 64));
    check(launcher::read_slot(span_of(big), 1, entry),
          "an unknown icon size still yields the game");
    check(entry.icon == nullptr, "an icon of the wrong size is dropped");
}

void test_override_names_a_slot_with_no_block_of_its_own() {
    std::vector<uint8_t> empty(launcher::k_slot_size, 0xFF);
    const auto overrides = make_overrides({{2, "Celeste"}});
    launcher::Entry entry{};

    check(!launcher::read_slot(span_of(empty), 2, entry),
          "with no overrides given, an empty slot is still not a game");
    check(launcher::read_slot(span_of(empty), 2, entry, span_of(overrides)),
          "an override names a slot that carries no block of its own");
    check(std::string(entry.title) == "Celeste", "the override's title is used");
    check(entry.slug[0] == '\0', "an override carries no slug");
    check(entry.version[0] == '\0', "an override carries no version");
    check(entry.icon == nullptr, "an override carries no icon");
    check(entry.slot == 2, "the slot index is still recorded");

    check(!launcher::read_slot(span_of(empty), 5, entry, span_of(overrides)),
          "the override only applies to the slot it names");
}

void test_override_wins_when_both_are_present() {
    // The override is patched in for every game PicoFlasher places, not
    // just a forced one, so it reflects what was actually composed. Its
    // title wins over the slot's own block when the two disagree, because
    // trusting a fresh scan of this slot's raw flash as the identity check
    // is exactly the failure mode the override table exists to route
    // around; the block still enriches the entry with the picture, slug,
    // and version the override never carries.
    const auto image = make_slot(make_block("kingfisher", "Kingfisher", "v1"));
    const auto overrides = make_overrides({{3, "Some Other Name"}});
    launcher::Entry entry{};

    check(launcher::read_slot(span_of(image), 3, entry, span_of(overrides)),
          "a slot with both a block and an override still reads");
    check(std::string(entry.title) == "Some Other Name",
          "the override's title wins: it is what the flasher actually placed here");
    check(entry.icon != nullptr, "the game's own icon still enriches the entry");
    check(std::string(entry.slug) == "kingfisher",
          "the game's own slug still enriches the entry");
}

void test_override_table_needs_its_own_magic() {
    // A slot sized buffer with no PSEOVR01 anywhere in it: nothing to find,
    // not a crash and not a false match on unrelated bytes.
    std::vector<uint8_t> junk(launcher::k_slot_size, 0x42);
    launcher::Entry entry{};
    check(!launcher::read_slot(span_of(junk), 1, entry, span_of(junk)),
          "a span with no override magic yields no override");

    // Too short to hold a whole table, even with the magic right at the
    // front: must not read past the end of it.
    std::vector<uint8_t> short_span(launcher::k_override_table_size - 1, 0);
    std::memcpy(short_span.data(), "PSEOVR01", 8);
    check(!launcher::read_slot(span_of(junk), 1, entry, span_of(short_span)),
          "a truncated override table is refused, not read out of bounds");
}

void test_scan_keeps_flash_order_and_skips_gaps() {
    const auto a = make_slot(make_block("alpha", "Alpha", "v1"));
    std::vector<uint8_t> empty(launcher::k_slot_size, 0xFF);
    const auto b = make_slot(make_block("beta", "Beta", "v2"));

    const launcher::Span slots[3] = {span_of(a), span_of(empty), span_of(b)};
    launcher::Entry found[8]{};
    const int count = launcher::scan(slots, 3, found, 8);

    check(count == 2, "two games among three slots");
    check(std::string(found[0].title) == "Alpha", "first game is first");
    check(std::string(found[1].title) == "Beta", "second game follows");
    check(found[0].slot == 1 && found[1].slot == 3,
          "slot indices skip the gap: got " + std::to_string(found[0].slot) +
              " and " + std::to_string(found[1].slot));
}

void test_scan_fills_gaps_from_overrides() {
    const auto a = make_slot(make_block("alpha", "Alpha", "v1"));
    std::vector<uint8_t> forced(launcher::k_slot_size, 0xFF);  // no block of its own
    std::vector<uint8_t> empty(launcher::k_slot_size, 0xFF);   // truly nothing
    const auto overrides = make_overrides({{2, "Celeste"}});

    const launcher::Span slots[3] = {span_of(a), span_of(forced), span_of(empty)};
    launcher::Entry found[8]{};
    const int count = launcher::scan(slots, 3, found, 8, span_of(overrides));

    check(count == 2, "the real game and the overridden slot both show, the empty one does not");
    check(std::string(found[0].title) == "Alpha", "the real game keeps its own title");
    check(std::string(found[1].title) == "Celeste", "the gap is named from the override");
    check(found[1].slot == 2, "the overridden entry keeps its own slot index");
}

void test_scan_respects_its_output_bound() {
    const auto a = make_slot(make_block("a", "A", "v1"));
    const launcher::Span slots[3] = {span_of(a), span_of(a), span_of(a)};
    launcher::Entry found[2]{};
    check(launcher::scan(slots, 3, found, 2) == 2,
          "scan never writes past the array it was given");
}

void test_boot_vectors_refuse_rubbish() {
    uint32_t sp = 0, pc = 0;

    const auto good = make_slot(make_block("a", "A", "v1"), 0x20042000,
                                0x10100109);
    check(launcher::boot_vectors(span_of(good), sp, pc), "a real image boots");
    check(sp == 0x20042000 && pc == 0x10100109, "both vectors come back");

    // An erased slot: all ones. Jumping there is unrecoverable without a
    // reflash, so it has to be refused.
    std::vector<uint8_t> erased(launcher::k_slot_size, 0xFF);
    check(!launcher::boot_vectors(span_of(erased), sp, pc),
          "an erased slot is not bootable");

    // A stack pointer outside SRAM.
    const auto bad_sp = make_slot(make_block("a", "A", "v1"), 0x10000000,
                                  0x10100109);
    check(!launcher::boot_vectors(span_of(bad_sp), sp, pc),
          "a stack pointer outside RAM is refused");

    // An entry point with no thumb bit: jumping there faults on M0+.
    const auto bad_pc = make_slot(make_block("a", "A", "v1"), 0x20042000,
                                  0x10100108);
    check(!launcher::boot_vectors(span_of(bad_pc), sp, pc),
          "an entry point without the thumb bit is refused");

    // An entry point outside flash.
    const auto off_pc = make_slot(make_block("a", "A", "v1"), 0x20042000,
                                  0x00000101);
    check(!launcher::boot_vectors(span_of(off_pc), sp, pc),
          "an entry point outside flash is refused");
}

void test_slot_addresses_match_the_linker() {
    // cmake/slot.cmake links slot n at base + n * 512 KB, and the bundle tool
    // writes images there. All three have to agree or a game boots into
    // whatever is next door.
    check(launcher::slot_address(1) == 0x10080000u,
          "slot 1 is at 0x10080000");
    check(launcher::slot_address(2) == 0x10100000u,
          "slot 2 is at 0x10100000");
    check(launcher::slot_address(23) == 0x10000000u + 23u * 512u * 1024u,
          "the last slot lands where the map says");
}

void test_menu_pages_and_stops_at_the_ends() {
    launcher::Menu menu;
    menu.reset(3);
    check(menu.index() == 0, "the menu starts at the first game");

    launcher::Input left{true, false, false};
    launcher::Input right{false, true, false};
    launcher::Input pick{false, false, true};
    launcher::Input none{false, false, false};

    check(!menu.update(left, 3), "left at the start selects nothing");
    check(menu.index() == 0, "left at the start stays put, it does not wrap");

    menu.update(right, 3);
    menu.update(right, 3);
    check(menu.index() == 2, "right pages forward");
    menu.update(right, 3);
    check(menu.index() == 2, "right at the end stays put, it does not wrap");

    check(menu.update(pick, 3), "A selects the game on screen");
    check(!menu.update(none, 3), "no press selects nothing");
}

void test_menu_survives_an_empty_or_shrunken_library() {
    launcher::Menu menu;
    menu.reset(0);
    launcher::Input pick{false, false, true};
    check(!menu.update(pick, 0),
          "A with no games installed must not select anything");

    // Three games at boot, one after a reflash: the cursor cannot be left
    // pointing at a game that is no longer there.
    menu.reset(3);
    launcher::Input right{false, true, false};
    menu.update(right, 3);
    menu.update(right, 3);
    check(menu.index() == 2, "cursor is on the third game");
    menu.update(launcher::Input{false, false, false}, 1);
    check(menu.index() == 0, "a shrunken library pulls the cursor back");
}

}  // namespace

int main() {
    test_reads_a_game_out_of_a_slot();
    test_an_empty_slot_is_not_a_game();
    test_a_nameless_or_oversized_block_is_refused();
    test_override_names_a_slot_with_no_block_of_its_own();
    test_override_wins_when_both_are_present();
    test_override_table_needs_its_own_magic();
    test_scan_keeps_flash_order_and_skips_gaps();
    test_scan_fills_gaps_from_overrides();
    test_scan_respects_its_output_bound();
    test_boot_vectors_refuse_rubbish();
    test_slot_addresses_match_the_linker();
    test_menu_pages_and_stops_at_the_ends();
    test_menu_survives_an_empty_or_shrunken_library();

    std::printf("launcher library: %d checks, %d failures\n",
                g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
