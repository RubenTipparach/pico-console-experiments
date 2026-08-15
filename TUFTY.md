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
- Nothing at all for input. The buttons report as themselves on both boards,
  and the reason that is worth a bullet is that it briefly did not: see the
  buttons section below.

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

Nothing is remapped. Every button reports as itself:

| physical | reads as |
|---|---|
| up, down | `DPAD_UP`, `DPAD_DOWN` |
| A | `A` |
| B | `B` |
| C | `X`, which is the pin the board config wires it to |

**There is no left and no right on the badge itself.** A game that steers
needs a Qw/ST Pad on the I2C connector, which the board config already
supports through the `tca9555` driver: it ORs a full dpad, four face buttons
and start/select into the same word, so it works with no code changes.

This was briefly done the other way, with B also reporting as left and C also
as right, on the reasoning that five buttons cannot be six and all twelve
games read `DPAD_LEFT` and `DPAD_RIGHT`. It was wrong on hardware, and
obviously so once seen: with a pad attached the badge has real left and right
already, so B and C fired their own action and moved the player at the same
time. A button that does two things is worse than a button that does one and
a direction you have to plug a pad in for. If a game needs to be playable on
the bare badge, the answer is a control scheme for that game, not a mapping
that makes every game's B ambiguous.

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

## Flashing it

**There are two drives, and only one of them takes a `.uf2`.** This is the
step that actually costs people time, because the wrong one appears first and
looks perfectly reasonable: you drop the file on, nothing complains, and
nothing happens.

| button | drive | what it is |
|---|---|---|
| double-tap RESET | `TUFTY` | MicroPython's filesystem, where badge OS keeps its apps. A `.uf2` copied here is just a file. |
| **hold BOOT, tap RESET** | `RP2350` | The RP2350 bootrom. This is the one. |

BOOT is at the far left on the back and RESET is just to its right. Hold BOOT
down for the whole gesture, including while tapping RESET, and let go after.
If `TUFTY` appears instead of `RP2350`, BOOT was not held.

Before the first flash, **copy the whole `TUFTY` drive somewhere**. It holds
`secrets.py` with the wifi credentials, and anything under `apps/`. The
restore image below puts Pimoroni's defaults back, not your copy, and once a
32blit game saves anything its storage driver claims the top 4 MB of flash,
which is where that filesystem lives.

Then:

1. Build: `build_uf2s_tufty.bat catcoin`
2. Eject the `TUFTY` drive so Windows is not holding it.
3. Hold BOOT, tap RESET, release BOOT. The drive becomes `RP2350`.
4. Drag `build.tufty\games\catcoin\catcoin.uf2` onto it. It reboots into the
   game by itself.

`tools/flasher` does the same job and knows about both boards: its
`BootselWatcher` matches the `RPI-RP2` and `RP2350` volume labels, and
`Uf2Locator` scans `build.pico` and `build.tufty`.

**This cannot brick the badge.** BOOT plus RESET is in the RP2350's boot ROM,
not in anything being flashed, so it is reachable whatever state the chip is
in. If a build does not boot, hold BOOT and tap RESET and the drive comes
back.

One `.uf2` is one game, so trying another means reflashing. The loader route
under "the RP2350 changes what CONSOLE.md says is impossible" is the fix for
that, and nothing here uses it yet.

CI publishes a Tufty build for every game on every run, so the gallery is the
other way to get one: each card offers `Pico` and `Tufty` side by side, and
the site is always what main last produced.

## Badge OS, and getting it back

The Tufty ships running Pimoroni's MicroPython firmware. A badge OS app is a
directory in `apps/` holding an `__init__.py`, a 24x24 launcher icon, and a
loop that calls `badge.update()`: MicroPython source, with no slot in the
format for a compiled binary. A 32blit game **is** the firmware rather than
something that runs on top of one, so the two cannot coexist and there is
nothing to drop into `apps/`. Living there would mean rewriting each game in
MicroPython against Pimoroni's `badge` API, which is a rewrite rather than a
port, and a fixed point software rasterizer split across two cores would not
survive the trip.

Nothing is lost by replacing it, and the swap takes about thirty seconds each
way. To put the badge back as it shipped, flash
`tufty-vX.X.X-micropython-with-filesystem.uf2` from
https://github.com/pimoroni/tufty2350/releases/latest exactly as above, then
copy your backed up `secrets.py` and `apps/` over the defaults. The other
release asset, `tufty-vX.X.X-micropython.uf2`, replaces only the firmware and
leaves the filesystem alone.

Worth knowing: badge OS has no over the air updating either. Apps are
installed by dragging a directory onto the `TUFTY` drive and firmware by
dragging a `.uf2` onto `RP2350`, so keeping it buys no update mechanism that
replacing it takes away.

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

## A corrupt bottom band, and what it actually was

Worth keeping because the first diagnosis was wrong and the evidence that
corrected it was one sentence from the person holding the device.

Every 3D game came back with the bottom of the screen corrupt. That band is
exactly the rows core 1 renders in `pse::run_split`, 2D games were untouched,
and the fault looked board specific, so core 1 on the RP2350 was the obvious
suspect and the first fix took it out of the split there.

Then: **the same glitch was on the RP2040.** Which rules core 1 out entirely,
because that path has shipped and been played on the PicoSystem for as long as
this repo has existed. What actually distinguishes the affected games is not
the split at all, it is that only 3D games link `shared_render.cpp`.

The cause was an optimisation from this same branch. The shared depth buffer
had been turned into a bare arena so a game drawing a different shape could
lay its own window over the same bytes, saving 14,444 bytes. The binding
happened in each game's static initialiser: jokerreels bound its 240x112
window unconditionally, everyone else bound 120x120 only if nothing had bound
yet, and in a console holding both the winner was link order. Whenever
jokerreels won, every other 3D game rasterized believing its depth buffer was
240 wide on a 120 wide screen. The index `y * 240 + x` passes the end of a
26,880 byte buffer at about `y = 112`, which is the bottom band, on any chip.

Reverted. The shared rasterizer is an `OwnedRasterizer<120, 120>` again and
jokerreels carries its own, which costs back the 14,444 bytes. The lesson is
not "bind more carefully": a shared object whose shape is decided by whichever
game initialised last has no single correct state, and a type that fixes the
shape cannot be got wrong that way.

Core 1 renders the bottom band on both boards again, as it always did. One
thing was worth keeping from the wrong diagnosis: the handoff's comment
claimed compiler barriers where the code uses `__sync_synchronize`, a real
`DMB` on Cortex-M33, so the acquire/release pair is correct on both chips and
the comment now says so.

## What is not done

- **The console is PicoSystem only in CI.** Every game is built and published
  for both boards, but `build_console.bat` is still a local job on either
  board and nothing publishes a console binary.
- **Not every game has been played on the badge.** A build has run on real
  hardware and works, which is what settled that the centred surface and the
  button mapping are right in principle. That is one game booting, not twelve
  games proven: per the note in `CLAUDE.md` on desktop testing, anything drawn
  with `screen.text` is unverified until a device has actually run it, and
  that is still true per game.
- **The button conflict is unmeasured in practice.** B doing double duty is
  known to be a compromise; which games it actually spoils is a question for
  playing them, not for reading them.
- **Audio, wifi, the RTC, the LEDs and the battery** are all untouched.
