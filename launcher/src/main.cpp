// The games launcher: pick one, hand the machine over to it.
//
// The library it browses is read straight out of flash (library.cpp), and the
// handoff is the four instructions boot2 would have run if this game had been
// the one at the base of flash.

#include <cstdio>

#include "32blit.hpp"

#include "library.hpp"
#include "menu.hpp"

#ifdef PICO_ON_DEVICE
#include "hardware/regs/m0plus.h"
#include "hardware/structs/scb.h"
#include "hardware/resets.h"
#include "hardware/irq.h"
#include "pico/platform.h"
#endif

using namespace blit;

namespace {

launcher::Entry g_games[launcher::k_max_slots];
int g_count = 0;
launcher::Menu g_menu;
bool g_boot_refused = false;

// Drawing the icon costs 2304 pen changes a frame, which is fine at this size
// and on a screen nobody is playing a game on.
void draw_icon(const uint8_t* icon, int x, int y, int scale) {
    if (icon == nullptr) {
        screen.pen = Pen(30, 34, 44);
        screen.rectangle(Rect(x, y, launcher::k_icon_w * scale,
                              launcher::k_icon_h * scale));
        screen.pen = Pen(90, 98, 115);
        screen.text("?", minimal_font,
                    Point(x + launcher::k_icon_w * scale / 2 - 2,
                          y + launcher::k_icon_h * scale / 2 - 3));
        return;
    }

    for (int iy = 0; iy < launcher::k_icon_h; iy++) {
        for (int ix = 0; ix < launcher::k_icon_w; ix++) {
            const int index = (iy * launcher::k_icon_w + ix) * 2;
            const uint16_t value = static_cast<uint16_t>(
                icon[index] | (icon[index + 1] << 8));
            // RGB565 out to 8 bits a channel, replicating the high bits so
            // white stays white instead of drifting grey.
            const uint8_t r = static_cast<uint8_t>(((value >> 11) & 0x1F) * 255 / 31);
            const uint8_t g = static_cast<uint8_t>(((value >> 5) & 0x3F) * 255 / 63);
            const uint8_t b = static_cast<uint8_t>((value & 0x1F) * 255 / 31);
            screen.pen = Pen(r, g, b);
            screen.rectangle(Rect(x + ix * scale, y + iy * scale, scale, scale));
        }
    }
}

void draw_centred(const char* text, int y, Pen pen) {
    // minimal_font is 4 pixels wide with a pixel of spacing.
    int width = 0;
    for (const char* c = text; *c; c++) width += 4;
    screen.pen = pen;
    screen.text(text, minimal_font, Point(60 - width / 2, y));
}

#ifdef PICO_ON_DEVICE

// Hand the machine to the game in `slot` and never come back.
//
// This is what boot2 does at the end of a normal boot, except pointed at the
// slot instead of the base of flash: set the vector table, install the game's
// stack pointer, jump to its reset handler. The game's own boot2 is skipped
// deliberately, and it has to be: boot2 ends by vectoring through
// XIP_BASE + 0x100, which is this launcher's vector table, so running a
// slot's boot2 would jump straight back here forever.
//
// A watchdog reboot is not used for the same class of reason. The bootrom's
// watchdog vector path skips flash boot entirely, so the QSPI setup this
// launcher's own boot2 performed would be lost and the game would run with
// flash in its slow default mode. Jumping in place keeps that setup.
//
// Nothing here can fail softly: if the vectors were rubbish the jump would
// land nowhere, so library.cpp validates them before this is ever called.
void __attribute__((noreturn)) boot_slot(int slot) {
    const uint32_t base = launcher::slot_address(slot);
    const uint32_t vectors = base + launcher::k_vector_offset;
    const uint32_t stack_pointer = *reinterpret_cast<uint32_t*>(vectors);
    const uint32_t reset_handler = *reinterpret_cast<uint32_t*>(vectors + 4);

    // Stop everything that could still fire while the game is bringing itself
    // up. An interrupt into this launcher's handler after its vector table is
    // gone is a hard fault with no explanation attached.
    __asm volatile("cpsid i");
    for (int irq = 0; irq < 32; irq++) {
        irq_set_enabled(static_cast<uint>(irq), false);
    }
    nvic_hw->icpr = 0xFFFFFFFFu;

    // Put the peripherals back the way a fresh boot would find them, so the
    // game does not inherit a display DMA or an audio alarm this launcher set
    // up. IO_QSPI, PADS_QSPI and the PLLs are deliberately excluded: resetting
    // those would take flash or the system clock away mid jump, and the code
    // doing the resetting is executing from flash.
    reset_block(RESETS_RESET_ADC_BITS | RESETS_RESET_DMA_BITS |
                RESETS_RESET_I2C0_BITS | RESETS_RESET_I2C1_BITS |
                RESETS_RESET_PIO0_BITS | RESETS_RESET_PIO1_BITS |
                RESETS_RESET_PWM_BITS | RESETS_RESET_SPI0_BITS |
                RESETS_RESET_SPI1_BITS | RESETS_RESET_TIMER_BITS |
                RESETS_RESET_UART0_BITS | RESETS_RESET_UART1_BITS |
                RESETS_RESET_USBCTRL_BITS);
    unreset_block_wait(RESETS_RESET_ADC_BITS | RESETS_RESET_DMA_BITS |
                       RESETS_RESET_I2C0_BITS | RESETS_RESET_I2C1_BITS |
                       RESETS_RESET_PIO0_BITS | RESETS_RESET_PIO1_BITS |
                       RESETS_RESET_PWM_BITS | RESETS_RESET_SPI0_BITS |
                       RESETS_RESET_SPI1_BITS | RESETS_RESET_TIMER_BITS |
                       RESETS_RESET_UART0_BITS | RESETS_RESET_UART1_BITS |
                       RESETS_RESET_USBCTRL_BITS);

    scb_hw->vtor = vectors;
    __asm volatile("msr msp, %0\n"
                   "bx %1\n"
                   :
                   : "r"(stack_pointer), "r"(reset_handler)
                   : "memory");
    __builtin_unreachable();
}

launcher::Span span_for(int slot) {
    return launcher::Span{
        reinterpret_cast<const uint8_t*>(launcher::slot_address(slot)),
        launcher::k_slot_size};
}

#else

// On desktop and web there is no flash to scan and nowhere to jump, so the
// launcher runs as a viewer: the menu, the real icons, no handoff. That is
// what makes the layout reviewable without a device in hand.
void boot_slot(int) {}

launcher::Span span_for(int) { return launcher::Span{nullptr, 0}; }

#endif

}  // namespace

void init() {
    set_screen_mode(ScreenMode::lores);

    launcher::Span slots[launcher::k_max_slots];
    for (int i = 0; i < launcher::k_max_slots; i++) {
        slots[i] = span_for(i + 1);
    }
    // Slot 0 is the launcher's own image, where its override table lives.
    g_count = launcher::scan(slots, launcher::k_max_slots, g_games,
                             launcher::k_max_slots, span_for(0));
    g_menu.reset(g_count);
}

void update(uint32_t time) {
    if (g_count == 0) return;

    launcher::Input input;
    input.left = (buttons.pressed & Button::DPAD_LEFT) != 0;
    input.right = (buttons.pressed & Button::DPAD_RIGHT) != 0;
    input.select = (buttons.pressed & Button::A) != 0;

    if (input.left || input.right) g_boot_refused = false;

    if (g_menu.update(input, g_count)) {
        // A slot listing a title is not proof its vector table is sane: the
        // block only says a game claims to be there. This is the actual
        // gate boot_slot depends on, checked here rather than left as the
        // comment on boot_slot claimed it already was.
        const int slot = g_games[g_menu.index()].slot;
        uint32_t stack_pointer = 0, reset_handler = 0;
        if (launcher::boot_vectors(span_for(slot), stack_pointer, reset_handler)) {
            boot_slot(slot);
        } else {
            g_boot_refused = true;
        }
    }
}

void render(uint32_t time) {
    screen.pen = Pen(9, 11, 16);
    screen.clear();

    if (g_count == 0) {
        draw_centred("NO GAMES", 50, Pen(220, 226, 236));
        draw_centred("INSTALLED", 60, Pen(120, 130, 150));
        return;
    }

    const launcher::Entry& game = g_games[g_menu.index()];

    // Top bar: which of how many, so paging has somewhere to read from.
    screen.pen = Pen(16, 20, 28);
    screen.rectangle(Rect(0, 0, 120, 9));
    screen.pen = Pen(120, 200, 224);
    screen.text("GAMES", minimal_font, Point(4, 2));
    char counter[12];
    snprintf(counter, sizeof(counter), "%d/%d", g_menu.index() + 1, g_count);
    screen.pen = Pen(150, 160, 178);
    screen.text(counter, minimal_font, Point(100, 2));

    draw_icon(game.icon, 12, 12, 2);

    draw_centred(game.title, 112, Pen(236, 242, 250));

    // Arrows only where there is somewhere to go, so the screen says what the
    // D-pad will actually do.
    screen.pen = Pen(120, 200, 224);
    if (g_menu.index() > 0) screen.text("<", minimal_font, Point(3, 56));
    if (g_menu.index() + 1 < g_count) {
        screen.text(">", minimal_font, Point(114, 56));
    }

    screen.pen = Pen(110, 120, 140);
    screen.text(game.version, minimal_font, Point(4, 103));
    screen.text("A PLAY", minimal_font, Point(88, 103));

    // Only reachable when a slot's vectors failed the check A just ran: a
    // real fault here is a corrupt or half written slot, not a bug to hide.
    // Staying on the menu beats jumping in blind.
    if (g_boot_refused) {
        draw_centred("CANNOT BOOT THIS", 70, Pen(224, 120, 120));
    }
}
