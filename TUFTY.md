# TUFTY.md

The Pimoroni Tufty 2350 as a second target for this repo: what the hardware
is, what the 32blit SDK already does for it, what had to change here to build
for both boards from one tree, and what is still wrong.

Primary sources: the 32blit SDK's pico port at commit `db99cb7` (its
`32blit-pico/board/pimoroni_tufty2350/` and `32blit-pico/blit_launch.cpp`
were read directly, not inferred), Pimoroni's `pimoroni/tufty2350` repo and
its releases, and builds of this repo's own games for both boards. The
SDK's `docs/pico.md` still lists only the RP2040 boards and is out of date;
do not use it to decide what is supported.

## The hardware

- RP2350, dual Cortex-M33. The board header defaults to 200 MHz
  (`SYS_CLK_HZ`), with a commented 250 MHz block for manual overclocking.
  Both cores have an FPU, which the RP2040 does not.
- **520 KB SRAM**, against the PicoSystem's 264 KB. Plus 8 MB of PSRAM on
  `BW_PSRAM_CS`, which the 32blit SDK does not use on this board.
- 16 MB flash, the same chip size as the PicoSystem, so everything
  `STORAGE.md` says about the 16 MB ceiling still applies unchanged.
- 2.8" 320x240 IPS, an ST7789 driven over an 8 bit parallel (DBI) bus, the
  same driver family the PicoSystem uses.
- **Five front buttons: up, down, A, B, C.** There is no left and no right.
  BOOT and RESET are on the back.
- RM2 module for wifi and Bluetooth, an RTC, four rear white LEDs, a
  1000 mAh battery, and a Qw/ST (Qwiic / STEMMA QT) I2C connector.

## What the SDK already does

Tufty 2350 support landed in 32blit master in March 2026, from Charlie Birks,
who wrote the pico port. It is not in any tagged release: the newest is
v0.3.3 from December 2024, which has no `pimoroni_tufty2350` directory at
all. 32blit's own CI builds a Tufty leg every run.

The whole board config is four lines:

```cmake
set(BLIT_BOARD_NAME "Tufty 2350")
blit_driver(display dbi)
blit_driver(input "gpio;tca9555")
blit_driver(led pwm_mono)
```

Two things in there matter to this repo.

**The display driver centres what it is given.** `dbi.cpp`'s
`display_mode_supported` allows "any size that will fit", and its `update`
does the rest itself:

```c
set_window((DISPLAY_WIDTH - expected_win.w) / 2,
           (DISPLAY_HEIGHT - expected_win.h) / 2, expected_win.w, expected_win.h);
```

So a game that asks for a 240x240 surface gets its PicoSystem picture,
unchanged, in the middle of a 320x240 panel. That is the entire port, and it
is why nothing in this repo had to learn a second screen size.

**`tca9555` is the Qw/ST Pad.** It is Pimoroni's I2C gamepad, and the driver
ORs a full dpad, four face buttons and start/select into the same button word
the five front buttons arrive on. A Tufty with a pad plugged in has better
controls than a PicoSystem.

There is no audio driver in that list, so `BLIT_AUDIO_DRIVER` defaults to
`none`. stardancer, picospace, kingfisher and tomlander are silent on a
Tufty.

## What had to change here

Almost nothing, deliberately, and the shape of the change is the point.

- `engine/include/pse/board.hpp` holds the design size, 240x240: the screen
  every game here was written against.
- `pse::set_screen_mode` (in `engine/src/blit_target.cpp`, which stays the
  only engine file that includes `32blit.hpp`) asks the SDK for a surface of
  exactly that size, rather than for whatever panel is fitted. Games call it
  instead of the SDK's `set_screen_mode`. That is one line per game and the
  only change any game needed.
- `pse::MappedButtons` presents the buttons the games read on a board that
  does not have them.

The consequence worth stating: **`PSE_RENDER_WIDTH` and `PSE_RENDER_HEIGHT`
stay 120x120 on both boards.** The depth buffer, the triangle queue and every
budget in `config.hpp` are untouched, and no game's literal 120 became wrong.
Measured, catcoin on the PicoSystem: bss identical to the byte, text up 104
bytes.

The trade is screen area. A Tufty runs the games in a 240x240 window with a
40 pixel black column either side, and gets no use out of the extra 80
columns. That is the right default: the games measure against a literal 120
in enough places, kingfisher most of all, that a wider surface would not be a
wider view, it would be a misplaced one. A game that wants the width has to
be written to want it.

### The buttons

Five buttons cannot be six, so something gives. The mapping is additive:

| physical | reads as |
|---|---|
| up, down | `DPAD_UP`, `DPAD_DOWN` |
| A | `A` |
| B | `B` **and** `DPAD_LEFT` |
| C | `X` **and** `DPAD_RIGHT` |

Additive rather than a permutation for two reasons. A game that reads B still
gets B, so nothing breaks outright; and a Qw/ST Pad's real left and right come
through the same word untouched, so the accessory composes instead of
fighting the fallback.

The cost is real and is not hidden: a game whose B does something other than
move left now does both at once. catcoin is the clearest case, where B uses
the selected item and left walks the row. The fix for that is per game control
schemes, which is a design decision nobody has made yet, not a bug in the
mapping.

## Building it

```
build_uf2s_tufty.bat              every game
build_uf2s_tufty.bat kingfisher   one game
```

Two things about that build are worth knowing before they cost an evening.

**It needs its own SDKs.** The Tufty board config is newer than the tag CI
pins, and pico-sdk 2.0.0 is too old as well (the board header wants
`PICO_RP2350_A2_SUPPORTED`, and the config pulls in `hardware_powman`). The
script fetches a pinned 32blit master commit and pico-sdk 2.3.0 into sibling
directories of their own and leaves the PicoSystem's SDKs alone. Those SDKs
do build the PicoSystem too, verified across all twelve games, so the
separation is caution rather than necessity.

**It needs `pico2.toolchain`, not `pico.toolchain`.** This is the one that
looks like an SDK bug. `pico.toolchain` hardcodes `-mcpu=cortex-m0plus`; it
configures without complaint and then fails deep in the compile:

```
spin_lock.h:196: #error no SW_SPIN_LOCK_LOCK available for PICO_USE_SW_SPIN_LOCK on this platform
```

`engine/src/parallel_pico.cpp` includes `pico/multicore.h`, RP2350 turns
`PICO_USE_SW_SPIN_LOCKS` on by default for errata E2, and that implementation
is gated on `__ARM_ARCH_8M_MAIN__`, which an ARMv6-M build does not define.
It is a wrong `-mcpu`, nothing more.

## Badge OS, and getting it back

The Tufty ships running Pimoroni's MicroPython firmware with apps in a flash
filesystem. There is no seam for a native ARM binary to live inside it: a
32blit game **is** the firmware. The two cannot coexist.

Nothing is lost by replacing it. Hold BOOT (far left on the back), tap RESET,
and a drive named `RP2350` appears; drag a `.uf2` on. To put the badge back
exactly as it shipped, flash
`tufty-vX.X.X-micropython-with-filesystem.uf2` from
https://github.com/pimoroni/tufty2350/releases/latest the same way. The other
release asset, `tufty-vX.X.X-micropython.uf2`, replaces only the firmware and
leaves the apps alone.

## The RP2350 changes what CONSOLE.md says is impossible

`CONSOLE.md` records that relocation was tried, specified in detail, and never
booted, so the console links every game into one binary and the menu picks
which `update()` runs. That is true on the RP2040 and it should stay true
there.

It is not true on the RP2350. From `32blit-pico/blit_launch.cpp`:

```c
#ifdef PICO_RP2350
  if(flash_offset == 4 * 1024 * 1024) {
    // we can use address translation for this, so flash in any free space
    flash_offset = find_flash_offset(file_len);
  }
```

and at launch it programs the QMI address translation unit, `qmi_hw->atrans[1]`,
to remap a game's fixed 4 MB link address onto wherever it actually landed.
The hardware does what the RP2040 could not do at all. That is why 32blit's
Tufty CI leg builds `.blit` files rather than a `.uf2`: `32blit-pico/loader/`
is a `blit-loader` firmware you flash once, and games are then pushed over USB
and picked from a menu.

So on a Tufty a console could be separately flashed games rather than one fat
binary, with no bundle composer and no slot linking, neither of which is being
proposed again. Nothing in this repo uses that path yet. It is written down
here so the next person does not read `CONSOLE.md` and conclude the question
is closed on both boards.

## What is not done

- **CI does not build for the Tufty.** Rule 2 says a build that only works on
  one machine is not done, and by that measure this is not. Publishing a
  second `.uf2` per game is a gallery change as much as a workflow one (a card
  would carry two downloads, and the manifest would need to say which is
  which), so it wants deciding rather than assuming.
- **Nothing has run on hardware.** Every claim above about what compiles and
  what it costs was measured. Every claim about what it looks like was read
  out of the driver. Per the note in `CLAUDE.md` on desktop testing, a change
  to anything drawn with `screen.text` is unverified until a device has
  actually run it, and that goes double for a board nobody here has held.
- **Audio, wifi, the RTC, the LEDs and the battery** are all untouched.
