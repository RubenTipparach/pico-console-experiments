#pragma once

// TEMPORARY: bisecting the launcher handoff hang by lighting the RGB LED at
// checkpoints, cumulatively (never turned off), so whichever colours are lit
// when the screen freezes says how far execution actually got. Raw GPIO,
// bypassing 32blit's own LED subsystem, so this works even before 32blit's
// engine has started anything. Remove this whole file and its call sites
// once the handoff bug is found.
//
// red   = dustrider's own init() was reached (confirmed: it lights)
// green = dustrider's own render() was reached, about to call render_scene
// blue  = about to call pse::run_split, i.e. about to launch core 1 for the
//         first time. If red+green+blue all light and the screen is still
//         frozen, the hang is inside run_split's core 1 launch specifically.

#ifdef PICO_ON_DEVICE
#include "hardware/gpio.h"
#include "boards/pimoroni_picosystem.h"

namespace diag {

inline void led_mark(bool r, bool g, bool b) {
    static bool initialised = false;
    if (!initialised) {
        gpio_init(PICOSYSTEM_LED_R_PIN);
        gpio_init(PICOSYSTEM_LED_G_PIN);
        gpio_init(PICOSYSTEM_LED_B_PIN);
        gpio_set_dir(PICOSYSTEM_LED_R_PIN, GPIO_OUT);
        gpio_set_dir(PICOSYSTEM_LED_G_PIN, GPIO_OUT);
        gpio_set_dir(PICOSYSTEM_LED_B_PIN, GPIO_OUT);
        initialised = true;
    }
    if (r) gpio_put(PICOSYSTEM_LED_R_PIN, 1);
    if (g) gpio_put(PICOSYSTEM_LED_G_PIN, 1);
    if (b) gpio_put(PICOSYSTEM_LED_B_PIN, 1);
}

}  // namespace diag

#define DIAG_LED_RED() ::diag::led_mark(true, false, false)
#define DIAG_LED_GREEN() ::diag::led_mark(false, true, false)
#define DIAG_LED_BLUE() ::diag::led_mark(false, false, true)
#else
#define DIAG_LED_RED()
#define DIAG_LED_GREEN()
#define DIAG_LED_BLUE()
#endif
