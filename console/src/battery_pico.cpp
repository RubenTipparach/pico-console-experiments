// Reading the PicoSystem's cell: one ADC channel and two GPIOs.
//
// The 32blit SDK has no battery API at all, on any board, so this is the one
// place the console goes around it and talks to the RP2040 directly. That is
// the pico-sdk, not the Pimoroni picosystem SDK rule 6 bans: `hardware/adc.h`
// is part of the toolchain every device build already links, and the pin
// numbers come from the board header the build already selected
// (`-DPICO_BOARD=pimoroni_picosystem`) rather than from a second SDK.
//
// The numbers are PicoCrystal-GBC's, which took them from Pimoroni's own
// hardware layer: a 3x divider onto ADC0, and a cell that reads as empty at
// 2.8V and full at 4.1V. The charge sense is that project's too, including the
// part that is not obvious (see below), because it was worked out against real
// hardware and the failure it fixes cannot be seen without a charger in hand.

#include "battery.hpp"

#include "pico.h"

#if defined(PICOSYSTEM_BAT_SENSE_PIN)

#include "hardware/adc.h"
#include "hardware/gpio.h"

namespace console {
namespace {

// ADC channel 0 is GPIO 26. The board header names the pin; this is the
// channel it lands on, and the subtraction says why rather than hiding a 0.
constexpr uint32_t k_adc_channel = PICOSYSTEM_BAT_SENSE_PIN - 26;

// The divider on the board is 3:1 into a 3.3V, 12 bit ADC, and the cell runs
// from 2.8V empty to 4.1V full. All integer: this is a menu, and there is no
// FPU on this chip (rule 8).
constexpr uint32_t k_adc_full_scale = 1u << 12;
constexpr uint32_t k_adc_ref_mv = 3300;
constexpr uint32_t k_divider = 3;
constexpr int k_empty_mv = 2800;
constexpr int k_full_mv = 4100;

// How often the ADC is actually read. The cell moves over minutes, so a
// reading a second is already generous, and the icon has 13 pixels of fill to
// say it with.
constexpr uint32_t k_sample_every_ms = 1000;

// STAT is an open drain output the charger pulls low while the cell takes
// charge, but only while the charger has power: unplug the cable and the pin
// still reads low, clamped by the charger's own ESD diode. So charging is the
// AND of STAT and VBUS, and the bolt means "filling", not "plugged in".
//
// Near a full charge the charger tops the cell off in bursts and releases STAT
// in the gaps, so the two directions are deliberately asymmetric: a confirmed
// low asserts, and only a full k_release_ms with no low at all clears. A
// symmetric debounce cannot represent a bursting STAT and freezes on whatever
// it saw first.
constexpr uint32_t k_release_ms = 3000;

bool g_started = false;
uint32_t g_next_sample_ms = 0;
Battery g_state;
bool g_prev_low = false;
uint32_t g_last_low_ms = 0;

bool stat_low() {
    return gpio_get(PICOSYSTEM_VBUS_DETECT_PIN) &&
           !gpio_get(PICOSYSTEM_CHARGE_STAT_PIN);
}

int read_percent() {
    adc_select_input(k_adc_channel);
    const uint32_t raw = adc_read();
    const int mv = static_cast<int>(raw * k_adc_ref_mv * k_divider /
                                    k_adc_full_scale);
    const int percent = (mv - k_empty_mv) * 100 / (k_full_mv - k_empty_mv);
    if (percent < 0) return 0;
    if (percent > 100) return 100;
    return percent;
}

void start(uint32_t time_ms) {
    adc_init();
    adc_gpio_init(PICOSYSTEM_BAT_SENSE_PIN);

    gpio_init(PICOSYSTEM_CHARGE_STAT_PIN);
    gpio_set_dir(PICOSYSTEM_CHARGE_STAT_PIN, GPIO_IN);
    gpio_pull_up(PICOSYSTEM_CHARGE_STAT_PIN);  // open drain, floats high

    gpio_init(PICOSYSTEM_VBUS_DETECT_PIN);
    gpio_set_dir(PICOSYSTEM_VBUS_DETECT_PIN, GPIO_IN);
    // Pulled down so the safe reading is the quiet one: a floating pin leaves
    // the bolt off rather than latching it on.
    gpio_pull_down(PICOSYSTEM_VBUS_DETECT_PIN);

    // Seeded from the pins, so booting on the charger shows the bolt at once
    // instead of waiting out a confirmation it never needed.
    g_prev_low = g_state.charging = stat_low();
    g_last_low_ms = time_ms;
    g_state.percent = read_percent();
    g_next_sample_ms = time_ms + k_sample_every_ms;
    g_started = true;
}

}  // namespace

Battery battery_sample(uint32_t time_ms) {
    if (!g_started) {
        start(time_ms);
        return g_state;
    }

    // Charge sense every frame, not on the ADC's cadence: the burst gaps this
    // has to measure are far shorter than a second.
    const bool low = stat_low();
    const bool prev = g_prev_low;
    g_prev_low = low;
    if (low) g_last_low_ms = time_ms;

    if (!g_state.charging) {
        // Two lows in a row before believing it: against a pull-up on a
        // floating line, one stray low is likelier to be noise than a charger.
        // At frame rate that is still well under a tenth of a second.
        if (low && prev) g_state.charging = true;
    } else if (!gpio_get(PICOSYSTEM_VBUS_DETECT_PIN) ||
               time_ms - g_last_low_ms >= k_release_ms) {
        g_state.charging = false;
    }

    // Unsigned subtraction, so this stays right when the SDK's millisecond
    // clock wraps.
    if (time_ms - g_next_sample_ms < 0x8000'0000u) {
        g_state.percent = read_percent();
        g_next_sample_ms = time_ms + k_sample_every_ms;
    }
    return g_state;
}

}  // namespace console

#else  // A pico build on some other board: there is no cell to read.

namespace console {

Battery battery_sample(uint32_t) { return Battery{}; }

}  // namespace console

#endif
