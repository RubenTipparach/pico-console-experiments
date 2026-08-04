# STORAGE.md

How the PicoSystem's 16 MB flash is used in this repo: what the hardware and
the SDK actually provide, how persistence works today, how a multi-game
library could work, and how far the budget stretches for larger games.

Primary sources: the RP2040 flash persistence thread at
https://forums.raspberrypi.com/viewtopic.php?t=310821, the 32blit SDK's pico
port (`32blit-pico/` in the SDK tree), and measurements of our own published
builds. Everything below was verified against the SDK source, not just the
thread.

## The hardware

- The PicoSystem carries a 16 MB QSPI flash chip
  (`PICO_FLASH_SIZE_BYTES = 16 * 1024 * 1024` in the SDK's
  `pimoroni_picosystem.h` board header).
- The RP2040 has no internal program flash. Code executes in place (XIP)
  straight from that chip, memory mapped at `0x10000000`. Reads are free:
  any byte of flash is a pointer dereference. That is why rule 8 says large
  const data belongs in flash; a `static const` table costs zero RAM.
- Writes are not free. Flash erases in 4096 byte sectors
  (`FLASH_SECTOR_SIZE`) and programs in 256 byte pages (`FLASH_PAGE_SIZE`).
  While a write is in progress XIP is unavailable, so nothing may execute
  from flash on either core until it finishes.

## The raw technique (what the forum thread covers)

The thread describes bare-metal persistence on a Pico, and it matches what
the SDK does under the hood:

1. Pick an offset past the end of the program (the thread reserves 512 KB
   for the program on a 2 MB Pico; the 32blit port reserves 12 MB for the
   program on the PicoSystem's 16 MB chip).
2. To write: `save_and_disable_interrupts()`, erase whole sectors with
   `flash_range_erase()`, program in page multiples with
   `flash_range_program()`, restore interrupts. Link `hardware_flash`.
3. To read: just dereference `XIP_BASE + offset`. No API needed.
4. On dual-core code, core 1 must be parked somewhere that is not flash
   before core 0 writes: the stock answer is
   `multicore_lockout_victim_init()` on core 1 plus
   `multicore_lockout_start_blocking()` around the write on core 0.

Do not hand-roll this in game code. The 32blit SDK already wraps all of it,
and games in this repo talk to flash only through `blit::` APIs (rules 6
and 7). The raw mechanics matter because they explain the safety contract
and the granularity, not because we call them directly.

## What the 32blit pico port provides

The SDK splits the 16 MB chip into two regions:

| Region  | Size  | Flash offset      | XIP address  | Contents                    |
|---------|-------|-------------------|--------------|-----------------------------|
| Program | 12 MB | 0x000000          | 0x10000000   | boot2, code, baked assets   |
| Storage | 4 MB  | 0xC00000 (12 MB)  | 0x10C00000   | FAT filesystem              |

The storage region is the top quarter of the chip
(`storage_size = PICO_FLASH_SIZE_BYTES / 4` in `32blit-pico/storage/flash.cpp`)
and carries a FatFs filesystem that the port auto-formats on first boot.
Everything below rides on it:

- **Saves.** `blit::write_save(data, slot)` writes the file
  `.blit/<game title>/save<slot>` on that partition;
  `blit::read_save` reads it back. The title comes from `game.yml` via the
  `blit_metadata` build step, so every game gets its own directory and no
  game can clobber another's saves.
- **Files.** The full `blit::File` API (open, read, write, list, remove)
  works against the same partition. A game can stream data it did not bake
  into its binary.
- **USB drive.** The port exposes the partition as USB mass storage
  (TinyUSB MSC in `32blit-pico/usb/device.cpp`). Plug the PicoSystem into a
  PC and the 4 MB partition mounts as a writable drive, so content can be
  dropped on without reflashing. The drive refuses writes while the game
  has files open.
- **Reflash safe.** A `.uf2` only programs the blocks it contains, which
  are all in the program region. Flashing a new game, or a new build of the
  same game, leaves the storage partition and every save on it intact.

### The dual-core safety contract (this repo is special)

The port's `storage_write` takes the multicore lockout only when the
backend itself started core 1, and on the PicoSystem it never does: the dbi
display and alarm-driven audio live on core 0, so the backend leaves core 1
idle. Our engine claims core 1 instead (`pse::run_split`), which means the
SDK's lockout does not protect our worker. The contract that does, from
rule 8:

- Core 1's idle loop is RAM resident (`__not_in_flash_func` in
  `parallel_pico.cpp`). While parked there it survives XIP going away.
- Therefore saves happen only outside `run_split`, never during a render.
  `update()` is outside by construction; Kingfisher additionally defers
  with a `save_pending` flag and skips saving mid-fight.

Break either half and core 1 hard-faults the first time a save lands during
a frame. Never move the worker loop into flash, never save from render.

## Persistence: how to do it in a game

- Define a small versioned save struct with a magic number, like
  Kingfisher's `SaveData` (`magic = 'KFR2'`), and bump the magic when the
  layout changes so stale saves are rejected instead of misread.
- Keep it small and save it rarely. Kingfisher asserts
  `sizeof(SaveData) <= 64` at compile time and saves on state changes, not
  per frame. Every write costs at least one 4 KB sector erase, and flash
  sectors wear out on the order of 100k erases. FatFs does not wear-level;
  save frequency is the only protection.
- Use `blit::read_save` / `blit::write_save`, honor the dual-core contract
  above, and nothing else is required: no offsets, no sector maths, no
  interrupt handling.
- Slots are arbitrary integers. Multiple profiles or a records book beyond
  the main save are just extra slots or extra files under the same
  directory.

## Game library: many games on one device

Today the device holds exactly one game: the pico port runs a single fixed
XIP image and stubs out the whole launcher API (`launch`, `erase_game`,
`flash_to_tmp`, `list_installed_games`, `can_launch` are all null in
`32blit-pico/main.cpp`). The 32blit console proper ships a launcher and a
relocatable `.blit` format; none of that is ported to the RP2040. So a
multi-game PicoSystem is ours to build, and there are three credible
designs:

1. **Copy-down launcher (recommended).** A small launcher owns
   `0x10000000`. Game images are stored high, either in a raw reserved
   region or as ordinary files on the 4 MB FAT partition (put there over
   USB, or shipped in the launcher image). To boot a game the launcher runs
   a RAM-resident routine that erases and reprograms the program region
   with the selected image, then resets via the watchdog. Games come back
   to the launcher by a held-button-at-boot convention or by writing a flag
   file and resetting.
   - Cost: a few seconds of flash programming per switch, wear on the low
     sectors, and the copy routine must live entirely in RAM.
   - Payoff: stock, unmodified game images work; every game still believes
     it owns `0x10000000`; our existing `.uf2` outputs are already the
     right artifact. At current build sizes (about 135 KB per game, see
     below) the FAT partition alone could hold about two dozen games and
     their saves; a raw high-flash region could hold far more.
2. **Fixed-slot multiboot.** Link N games at N distinct XIP offsets with
   custom linker scripts; a tiny launcher sets the vector table and stack
   pointer and jumps. Switching is instant and wear free, but every game
   must be rebuilt per slot, boot2 and vector table assumptions get fragile
   across SDK updates, and a stock image no longer works. Not worth it
   while builds are this small.
3. **Port the `.blit` relocation format.** The full-fat 32blit firmware
   relocates position-annotated binaries at install time. Porting that
   machinery to the pico backend is the cleanest end state and by far the
   most work. Revisit if option 1 outgrows itself.

Whichever design lands, saves already cooperate: every game keeps its own
`.blit/<title>/` directory on the shared partition, so a library switch
never touches another game's records.

## Larger games: the actual budget

Current published `.uf2` sizes against the 12 MB program region (a `.uf2`
stores 256 data bytes per 512 byte block, so real flash use is half the
file size):

| Game       | .uf2 size | Flash used | Of 12 MB |
|------------|-----------|------------|----------|
| Kingfisher | 277 KB    | ~135 KB    | ~1.1%    |
| Pico Santa | 227 KB    | ~111 KB    | ~0.9%    |
| Chicken    | 210 KB    | ~103 KB    | ~0.8%    |

The takeaway: we are using about one percent of the program budget. There
is room for roughly 80x more content per game before the region is a
constraint, and reads from XIP cost no RAM, so the way to a bigger game is:

- **Bake assets into the image.** Models via `obj2cpp`, sprite sheets and
  audio via the asset pipeline, all as `static const` tables. They live in
  flash, are read in place, and only what a frame touches transits RAM.
  This is the default and it scales to megabytes before anything hurts.
- **Stream from the FAT partition** when content should be replaceable
  without reflashing (user content, level packs, anything a player might
  drop on over USB). `blit::File` reads cost a FatFs traversal, so stage
  into RAM at load points, not per frame.
- The 264 KB of SRAM is still the real limit (rule 8). Flash holds the
  content; RAM budgets what can be alive at once.

## Rules of the road

- Game and engine code never includes `hardware/flash.h` or calls the pico
  SDK flash functions. `blit::` APIs only.
- Saves are small, versioned, magic-checked, and written outside
  `run_split`.
- Anything touching flash layout, save formats, or the storage partition
  states its cost and its wear behavior in the PR body, same as RAM and
  flash deltas under rule 8.
- A future launcher lives in its own directory as its own build target and
  must not leak into the engine or the games (rule 7): a game must not know
  whether it was booted by a launcher or flashed directly.
