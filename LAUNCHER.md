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
  game can be linked at a flash origin other than the base. That is what makes
  a slot possible at all.
- The handoff at the end of `exit_from_boot2.S` is four instructions long and
  can be done from C, so entering a game is a supported sequence rather than a
  trick.

## Flash map

Every game lives at an address of its own and runs from there. Nothing is
copied at runtime, so there are no flash writes outside the SDK's own save
path, no wear, and switching games is instant.

| Slot    | Size   | Address    | Contents                                |
|---------|--------|------------|------------------------------------------|
| 0       | 512 KB | 0x10000000 | boot2, the launcher, its UI              |
| 1       | 512 KB | 0x10080000 | a game                                   |
| 2       | 512 KB | 0x10100000 | a game                                   |
| ...     | 512 KB | ...        | 23 game slots in all                     |
| storage | 4 MB   | 0x10C00000 | the SDK's FAT partition: saves, files    |

The top 4 MB is not ours to move: `32blit-pico/storage/flash.cpp` hardcodes
the FAT partition at `PICO_FLASH_SIZE_BYTES - PICO_FLASH_SIZE_BYTES / 4`, and
every save already written lives there. Everything below it is slots.

A game is about 135 KB today, so a 512 KB slot is roughly four times the
headroom, and 23 of them fit. Three places hold that map and they must agree:
`cmake/slot.cmake` links to it, `launcher/src/library.hpp` scans by it, and
`tools/make_bundle.py` writes by it. A test asserts all three match, because a
silent disagreement puts a game at an address the launcher never looks at.

Games keep their existing base-linked `.uf2` as well, so flashing one game
directly over BOOTSEL keeps working exactly as it does today. The launcher is
an addition, not a replacement.

## Booting a game

1. The launcher owns the reset vector. On power up it scans each slot for a
   metadata block and draws the menu from what it finds. The games describe
   themselves, so nothing has to be written down about what is installed.
2. Picking a game reads the two words at `slot + 0x100`: its stack pointer and
   its reset handler. Both are range checked first, because a jump into
   rubbish is the one failure with no way out but a reflash.
3. The launcher disables interrupts, puts the peripherals back in reset,
   points VTOR at the slot's vector table, installs the stack pointer, and
   branches. That is what boot2 does at the end of a normal boot.

Two details here are not free choices, and both were checked against the SDK
source rather than assumed:

- **The game's own boot2 must be skipped.** `exit_from_boot2.S` ends with
  `ldr r0, =(XIP_BASE + 0x100)`, a hardcoded jump into the vector table at the
  base of flash. A slot's boot2 would therefore jump back into the launcher,
  forever. Entering at the reset handler is the only way in.
- **A watchdog reboot is the wrong tool.** `watchdog_reboot(pc, sp, ms)` exists
  and works, but the bootrom's watchdog vector path skips flash boot, so the
  QSPI setup the launcher's own boot2 performed would be lost and the game
  would run with flash in its slow default mode. Jumping in place keeps that
  setup, which is why the launcher resets the peripherals by hand instead.

Games are built a second time for the bundle, with a linker script whose FLASH
origin is their slot (`-DPICO_SLOT=n`). That variant is what gets bundled; the
standalone `.uf2` is unchanged.

## The bundle

`tools/make_bundle.py` composes the launcher and every slot build into one
`.uf2`. A UF2 block carries its own destination address, so a bundle is the
launcher's blocks plus each game's blocks with the numbering redone: no
relinking and no patching, because the slot builds already point where they
belong.

What the tool adds is refusal. It rejects a game linked for one slot and
bundled into another (naming the `-DPICO_SLOT=n` that would fix it), two games
claiming one slot, a game too big for its slot, a launcher that would run into
slot 1, and a game with no metadata block, which the launcher would leave out
of the menu while it silently occupied a slot. Every one of those produces a
console that boots into the wrong thing, and none can be diagnosed by looking
at the device.

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
  the slug so it is stable between builds and obviously synthetic. The
  placeholder is a fallback, not a plan: a menu of coloured rectangles tells a
  player nothing, so a game in the rotation should carry a real screenshot,
  made with `tools/make_thumbnail.py` from a frame its preview harness
  rendered.
- PNG decoding is done in the tool itself rather than with Pillow: this runs
  inside every device build, and a pip install on every CI runner is the cost
  this repo avoids elsewhere.
- `tools/tests/test_game_meta.py` round trips a picture through a real `.uf2`
  and asserts the block survives compilation into an object file.

The block travels inside the binary, so there is no packaging format to
invent: a slot holds a game image, and the image carries its own name and
picture.

## The desktop tool

`tools/flasher/` today is one window that copies a `.uf2` to a board in
BOOTSEL. The library tool is the same program grown a library view.

Because the games are bundled, changing what is installed means composing a
new bundle and writing it over BOOTSEL: the tool picks games from the local
library, assigns them slots, runs the same composition `make_bundle.py` does,
and flashes the result. There is no protocol and no driver, and the device
does not need to be running anything in particular to be managed.

The local library is a gitignored `library/` cache of built `.uf2` files, each
with the name and icon read out of its own metadata block. It is browsable
with nothing plugged in, which is what the empty state in the mockups is for;
connecting a device in BOOTSEL is what turns the Install button on.

The storage partition the port exposes as a USB drive (`32blit-pico/usb/
device.cpp`) stays what it is today: saves and files. It is the natural home
for a future "extra games without BOOTSEL" mode, which would need the copy
into a run slot that the bundle design deliberately does without.

## What is built and what is not

Built and tested:

- the metadata block, its generator, its reader, and the CMake wiring that
  puts it in every device build
- the icon pipeline, including the PNG reader and the placeholder

- the launcher itself: slot scanning, the menu, and the boot vector checks,
  host tested against synthetic flash images (`launcher/tests`)
- the slot linker script and `-DPICO_SLOT=n`, proven with a real ARM link: the
  image lands at its slot with its vector table at slot + 0x100
- the bundle composer and every refusal it makes
  (`tools/tests/test_make_bundle.py`)

Not built yet:

- the tool's library mode: device detection, the cache directory, install and
  remove. The mockups are confirmed, so that is implementation, not design.
- CI does not assemble a bundle yet, so there is no downloadable one.

## Related projects

[PicoCrystal-GBC](https://github.com/Wyatt-Grant/PicoCrystal-GBC) is a Game
Boy Color emulator for the PicoSystem with a boot menu for up to 14 ROMs,
per-game save slots, custom names and icons, and a "boot last game" option.
Worth a look for menu and save-slot UX ideas, but it solves a different, easier
problem than this repo does: it is one compiled emulator with ROM files
embedded into its own firmware image, not independently built native
programs. An emulator interpreting ROM data never runs into the addressing
constraint this file opens with, because only the emulator itself ever
executes; the "games" are data, not code linked to their own absolute
addresses. That is also why it cannot be the model for bundling arbitrary
PicoSystem `.uf2` games, ours or anyone else's: those are native code, and
native code is exactly what the constraint applies to.

None of this has run on hardware. The handoff is the part to watch: if it is
wrong the console needs a BOOTSEL reflash, which is recoverable, but it is not
something a player should ever meet.
