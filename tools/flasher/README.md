# PicoFlasher

A one window Windows utility that copies a `.uf2` onto a PicoSystem.

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
