# PicoFlasher

A one window Windows utility with two jobs: copy a single `.uf2` onto a
PicoSystem, or build a library of games and put the whole lot on at once.

Quick and dirty on purpose: single form, no installer, no settings file. It is a
tool, not a product.

## Getting it

Download `PicoFlasher.exe` from the latest release, or grab the artifact from
the `Build flasher` workflow run. It is self contained, so nothing else needs
installing.

## Using it

1. Put the board in BOOTSEL: from a power off state, hold **X** and press
   **power**. It mounts as `RPI-RP2`.
2. Open `PicoFlasher.exe`. The board appears in the device list on its own.
3. Pick a `.uf2`. The list is filled from your build output automatically when
   the app is run from inside the repo, and `Folder...` points it anywhere else.
4. Press **Flash**.

The board reboots into the game the moment the copy finishes.

## The Library tab

The console holds one launcher and up to 23 games, all in a single `.uf2`. See
[LAUNCHER.md](../../LAUNCHER.md) for why it is one file rather than an install
step per game. This tab is where that file gets made.

1. Fill the library. It defaults to `library/` at the repo root, which is
   gitignored. Put built `.uf2` files there, or press **Add file...**. The
   `launcher-bundle` artifact from CI and any local build tree are picked up
   too, so a game you just built shows up without being copied anywhere.
2. Pick games on the left and press **Add** to put them in the bundle. Each
   game's name and icon come out of the `.uf2` itself, so the list is readable
   without a manifest.
3. Press **Flash to console** with a board in BOOTSEL, or **Save bundle...** to
   write the file and flash it later.

Two things it will refuse, both because they produce a console that boots into
the wrong thing with nothing to read but a black screen:

- A game linked for the wrong slot. Bundled games are built with
  `-DPICO_SLOT=n`; the standalone `.uf2` you flash on its own is linked at the
  base of flash and cannot go in a bundle. The tool says which is which.
- A game with no metadata block, which the launcher would leave out of its menu
  while it silently occupied a slot.

### What it cannot do

Tell you what is already on the console. `RPI-RP2` is a fake FAT volume with no
storage behind it: sectors are parsed for UF2 magic as they arrive and then
discarded, so there is nothing to read back. The right hand list is the bundle
being built, not a report from the device.

## Building it locally

```
cd tools/flasher
dotnet publish PicoFlasher.csproj -c Release -r win-x64 --self-contained true -o publish
```

## Why the copy "failing" means it worked

The RP2040 bootrom arms its reboot as soon as the final UF2 block is written,
with about a second of watchdog delay. Everything the host does after that last
data sector, flushing, updating the directory entry, closing the handle, is
talking to a device that has already left the USB bus. Windows surfaces that as
an `IOException`.

So the utility treats an I/O error after the last block as success, and only
reports failure if the write did not get that far. Every UF2 bootloader behaves
this way, which is why dragging a `.uf2` onto the drive in Explorer often shows
an error too.

For the same reason the tool never reads the file back to check it. `RPI-RP2` is
a fake FAT16 volume with no storage behind it: sectors are parsed for UF2 magic
as they arrive and then discarded, so there is nothing on the other side to
verify against. What it does check is the UF2 magic in the file **before**
writing, so picking the wrong file is caught before the board is touched.
