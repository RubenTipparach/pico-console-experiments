# LAUNCHER.md

Putting more than one game on the device: a launcher that browses what is
installed, a build that gives every game a name and an icon, and a desktop
tool that manages the library.

`STORAGE.md` covers the flash the hardware gives us. This covers what we build
on top of it. Everything below was checked against the pico SDK and the 32blit
pico port in the tree, and the parts that are built already are marked as
such.

## The constraint everything follows from

The RP2040 executes in place from flash mapped at `0x10000000`. A game is
linked to absolute addresses in that window, so two games cannot both be at
the address they were linked for. Any multi-game device therefore needs either
relocation (which the pico port does not implement) or a fixed place to run
from.

Two SDK facts make the fixed-place design work, both verified in
`/tmp/pico-sdk`:

- `pico_set_linker_script()` sets `PICO_TARGET_LINKER_SCRIPT` per target, so a
  game can be linked at a flash origin other than the base.
- `watchdog_reboot(pc, sp, delay_ms)` exists, so a warm handoff is a supported
  operation rather than a trick.

## Flash map

| Region  | Size  | Address    | Contents                                  |
|---------|-------|------------|-------------------------------------------|
| Launcher| 1 MB  | 0x10000000 | boot2, the launcher, its UI               |
| Run slot| 3 MB  | 0x10100000 | the game currently installed to run       |
| Library | 8 MB  | 0x10400000 | stored game images, one per slot          |
| Storage | 4 MB  | 0x10C00000 | the SDK's FAT partition: saves, files     |

The top 4 MB is not ours to move: `32blit-pico/storage/flash.cpp` hardcodes
the FAT partition at `PICO_FLASH_SIZE_BYTES - PICO_FLASH_SIZE_BYTES / 4`, and
every save already written lives there. The three regions below it are ours.

Games keep their existing base-linked `.uf2` as well, so flashing one game
directly over BOOTSEL keeps working exactly as it does today. The launcher is
an addition, not a replacement.

## Booting a game

1. The launcher owns the reset vector. On power up it draws the menu.
2. Picking a game copies its image from the library region into the run slot,
   4 KB sector at a time, from a routine that is RAM resident. Flash writes
   disable XIP, so the copy loop cannot be executing from flash while it runs.
   A game already in the run slot skips the copy and starts immediately.
3. The launcher reads the game's vector table at `run slot + 0x100` (past its
   boot2), then `watchdog_reboot()` into it.
4. Holding a button at power up skips step 2 and 3 and stays in the menu, so a
   game that hangs can never lock the device out of its own launcher.

Games for the launcher are built a second time with a linker script whose
FLASH origin is the run slot. That variant is what gets stored; it is a
different artifact from the standalone `.uf2`, and both come out of one build.

## The metadata block (built)

A PicoSystem `.uf2` says almost nothing about itself: the pico port's binary
info carries a program name and an author, and no image at all
(`32blit-pico/binary_info.hpp` defines exactly two tags). The 32blit metadata
format does carry an icon and a splash, but only for the STM32 `.blit` path,
which the pico port does not build.

So every game now compiles a fixed 4704 byte block into a `.pse_meta` section:

    magic "PSEGAME1", block size, icon size, format,
    slug, title, version, then a 48x48 RGB565 icon.

- `tools/game_meta.py emit` generates it as a `const` C++ array, so it lives
  in flash and costs no RAM (rule 8).
- `tools/game_meta.py extract --uf2 <file>` reads it back out of a finished
  `.uf2` by flattening the blocks and scanning for the magic, which is how the
  desktop tool learns a game's name and icon without a sidecar file.
- The icon is `games/<slug>/icon.png` if a game ships authored art, otherwise
  the committed `thumbnail.png`, otherwise a generated placeholder keyed off
  the slug so it is stable between builds and obviously synthetic.
- PNG decoding is done in the tool itself rather than with Pillow: this runs
  inside every device build, and a pip install on every CI runner is the cost
  this repo avoids elsewhere.
- `tools/tests/test_game_meta.py` round trips a picture through a real `.uf2`
  and asserts the block survives compilation into an object file.

The block travels inside the binary, so it needs no packaging format of its
own: the library region stores game images, and each image carries its own
name and picture.

## The desktop tool

`tools/flasher/` today is one window that copies a `.uf2` to a board in
BOOTSEL. The library tool is the same program grown a second mode, because a
device in BOOTSEL and a device running the launcher are two different things
on the USB bus:

- **BOOTSEL**: the `RPI-RP2` drive. This is how the launcher itself gets
  installed, and how a single game is flashed directly. Already built.
- **Running the launcher**: the 32blit pico port exposes the FAT partition as
  USB mass storage (`32blit-pico/usb/device.cpp`), so the device appears as an
  ordinary drive. Installing a game is a file copy onto that drive and
  removing one is a delete. No custom protocol, no driver.

That gives the tool its model: read the drive to see what is installed, read
each image's metadata block for its name and icon, and copy or delete to
change it. The local library is a gitignored cache directory of built games,
each with its extracted icon, so the list is browsable with no device
attached, and connecting one just fills in the "installed" column.

## What is built and what is not

Built and tested:

- the metadata block, its generator, its reader, and the CMake wiring that
  puts it in every device build
- the icon pipeline, including the PNG reader and the placeholder

Designed here, not built:

- the launcher firmware: menu, library region, RAM resident copy, handoff
- the run slot linker script and the second build of each game
- the tool's library mode: device drive detection, install, remove, cache

The two unbuilt parts are both user interfaces, and rule 10 says a change that
materially alters a game's screen or the flasher layout gets a mockup and a
confirmation before the implementation. That is the next step, not more code.
