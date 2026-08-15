# CONSOLE.md

Putting more than one game on the device: one binary that holds them all, and
a menu that picks one.

`STORAGE.md` covers the flash the hardware gives us. This covers what is built
on top of it. Everything here has been run.

## What this replaces, and why

The previous plan gave every game a 512 KB slot in flash, linked each game a
second time at its slot's address, composed the launcher and every slot build
into one `.uf2`, and had the launcher hand the machine over by pointing VTOR
at a slot and branching. It was carefully specified, it had tests, and no part
of it ever booted a console. It also cost a second device build per game, a
bundle composer with five separate refusals, a desktop tool that assembled
bundles, and a flash map that three files had to agree about.

The two projects that actually do this on this hardware do not do any of it:

- **[crisp-game-lib-portable-32blit](https://github.com/joyrider3774/crisp-game-lib-portable-32blit)**
  builds around fifty games into one binary. Each game is a single file where
  everything is `static` except one `addGameX()` that registers a title and an
  update function; `lib/menu.c` keeps a `Game games[]` table, and picking one
  swaps which update runs. The menu is itself an entry in that table.
- **[PicoCrystal-GBC](https://github.com/Wyatt-Grant/PicoCrystal-GBC)** embeds
  every ROM in `assets/` into its own firmware image. `tools/gen_rom_data.py`
  validates each one and emits a `rom_catalog[]` of names, sizes, save slots
  and icons; the boot menu draws that catalog. It fails the build with a
  readable message rather than shipping something that dies at boot.

Neither relocates native code, because neither has to. That is the whole
insight: **the games do not need to move, only the call does.**

### What PicoCrystal-GBC costs in RAM, and why we cannot copy it

Worth reading carefully, because at a glance it looks like it solves a problem
this repo has and it does not. Everything below was read out of the source at
`Wyatt-Grant/PicoCrystal-GBC`, not inferred from the README.

**Its games are data.** `tools/gen_rom_data.py` emits each ROM as
`extern "C" const uint8_t rom_N_data[]`, so every ROM is const and lives in
flash. `rom_catalog[]` is a table of pointers into it. Picking a game is one
assignment, `g_rom_data = rom.data`, and reading a byte is
`g_rom_data[addr]`: a dereference straight into XIP flash, with no copy into
RAM at any point.

**Its emulator is one instance.** `main.cpp` has exactly one
`static struct gb_s gb;`, and that structure holds the whole machine:
`wram[WRAM_SIZE]`, `vram[VRAM_SIZE]`, `oam[OAM_SIZE]`, which on the colour
build is 32 KB + 16 KB + 160 bytes. That figure does not move whether the
firmware carries one ROM or a hundred.

So the cost of N games there is N ROMs of flash and **zero** extra RAM. That
is not a subtle optimisation, it is what "the games are input to one program"
means. Confirmed in practice as well as in the source: 13 to 14 MB of ROMs on
a 16 MB device, running fine.

**Ours are code.** Every game here is native C++ with its own file scope
state, and the linker allocates all of it, permanently, at build time. Nothing
is loaded when the menu picks a game because there is nothing to load: a
game's `g_world` exists from boot whether or not it is ever chosen. Ten games
is ten `g_world`s resident at once, about 18 KB of the console's 213 KB.

The property PicoCrystal gets for free is therefore not available by tidying
our code. It needs one of:

- **An overlay.** Only one game runs at a time, so their state could share one
  arena sized to the largest rather than the sum. `pse::Game::init()` already
  must fully re-seed on every entry, which is the contract that would make it
  safe. Worth roughly 21 KB including the rasterizers, against a framebuffer
  that is 54% of the budget and cannot be shared away.
- **Separately linked games**, which is the real analogue and is only possible
  on RP2350. See `TUFTY.md`: `32blit-pico/loader/` plus the QMI address
  translation unit lets a `.blit` be flashed anywhere free and remapped at
  launch, so only the running game exists at all. On an RP2040 this is the
  thing that never booted.

Two other pieces of it are worth stealing regardless, and neither is about
game count:

- **Sliced flash erase.** `flash_sliced.cpp` uses the W25Q Erase Suspend
  (0x75) and Resume (0x7A) commands to erase a sector in ~1.5 ms slices
  instead of holding XIP down for the whole ~45 ms, so the game keeps running
  through a save. Our `write_save` contract (rule 8, `STORAGE.md`) is the
  blunt version of the same problem.
- **Ping-pong save slots.** Saves alternate between two flash slots so a
  power cut during a write cannot destroy the previous save.

## The design

One `.uf2`. It contains the menu and every game `console.yaml` lists, all
linked at the address they were built for, which is the address they run at.
Switching games is an indirect call.

```
console.yaml   -> tools/gen_library.py -> console_library.cpp   (the menu table)
                                       -> console_games.cmake   (what to compile)
                                       -> console_game_stubs.cpp (for host tests)
```

A game exports exactly one symbol:

```cpp
PSE_GAME(kingfisher, game_init, game_update, game_render);
```

which is an `extern "C" const pse::Game` of three function pointers
(`engine/include/pse/game.hpp`). Everything else in a game is internal
linkage, because four games in one binary is four `g_world`s, and two of those
at file scope is a duplicate symbol rather than a warning.

The same game still builds on its own. `add_picosystem_game()` generates a
four line `standalone_main.cpp` that forwards the SDK's global
`init`/`update`/`render` to that game's `pse::Game`, so the per game `.uf2`
and the web build are exactly what they were. A game's `CMakeLists.txt` is
identical either way; `-DPSE_CONSOLE=ON` is the only difference.

## The menu

`console/src/menu.cpp` draws into a `pse::RenderTarget` and touches no SDK.
That is deliberate and it is the reason this rewrite could be checked: the
same code that runs on the PicoSystem renders into a buffer on a host, so
`console/tests/console_menu_preview` writes the real menu out as pictures and
`console_menu_tests` asserts its behaviour. The launcher this replaces could
only be looked at by flashing it.

Text comes from `engine/font/console5x7.txt`, which is ASCII art, one picture
per character, packed into a 464 byte const table by `tools/gen_font.py`. Rule
11's reasoning applied to a font: art belongs in a file that can be looked at.
The charset is the whole charset, and `gen_library.py` refuses a name with a
character outside it rather than drawing holes.

Controls, in full:

- **up/down** move, with key repeat when held.
- **any button** starts the game under the cursor. Nothing on screen names a
  button, so no press can be the wrong guess (rule 9).
- **the power switch** is the way back. A game runs until the console is
  restarted, and it restarts into this menu with the dot on the game you were
  in. There used to be a gesture, up and down held together, and the one line
  of instruction the console printed was there to explain it; the switch is a
  control the device already has and nothing has to say out loud.

The header carries the console's title and, at its right end, the battery: a
20x13 icon whose interior fills proportionally in a red to amber to mint ramp,
with a charging bolt knocked out of the fill while the cable is in. Both the
icon and the charge sense are PicoCrystal-GBC's, including the part that is
not obvious (charging is STAT **and** VBUS, because an unplugged charger's
STAT line still reads low). `console/src/battery_pico.cpp` reads the cell:
ADC0 through the board's 3:1 divider, empty at 2.8V and full at 4.1V, sampled
once a second. That file is the one place the console goes around the 32blit
SDK, which has no battery API on any board, and it goes to the pico-sdk the
device build already links rather than to a second board SDK (rule 6). The
menu itself never reads a pin: `Menu::set_battery` is handed a level, so the
preview harness can draw any of them and the desktop build honestly reports
that it has no cell (`percent` of -1 draws no icon at all).

The last game played is remembered in the console's own save (`blit::write_save`,
keyed by slug), so the menu reopens where you left it with a dot on the row.
Writing happens on entering a game, from `update()`, never from inside
`run_split`: that is the flash safety contract in STORAGE.md.

## What it costs

Measured on the desktop build, which has the same static allocations as the
device:

| | bytes |
|---|---|
| shared `pse::Rasterizer` (depth buffer) | 14,440 |
| shared `pse::FrameQueue` (640 triangles) | 17,926 |
| `kf::World` + `dr::World` + `santa::City` + Chicken's level | ~5,000 |
| menu icons, 24x24 RGB565, four of them | 4,608, in flash |

The rasterizer and the queue are **shared**, from
`pse::shared_rasterizer()` / `pse::shared_queue()`. Three games with a copy
each would be 89 KB. The SDK statically allocates a 115,200 byte framebuffer
on the PicoSystem (`screen_fb` in `32blit-pico/display.cpp`, hires and single
buffered on RP2040), which leaves about 149 KB of the 264 KB for everything
else, so 89 KB of scratch space for scenes nothing is rendering was not
affordable and was not necessary: one game runs at a time and no state
survives leaving it.

Nothing in CI checks that total any more: the console is built locally, by the
person about to flash it, so the number to watch is the one `arm-none-eabi-size`
prints at the end of that build. `data + bss` is the static footprint, the
framebuffer is already inside it, and the RP2040 has 270,336 bytes. Past about
230,000 there is too little left for the stack, and the linker's own
`region RAM overflowed` is the next warning you get — which names no game.

A 2D game that never asks for the shared rasterizer never links it:
`shared_render.cpp` is a translation unit of its own, and an archive member
nothing references is not pulled in (rule 7, interface segregation).

## Adding a game to the console

Add it to `console.yaml`. That is the whole procedure:

```yaml
menu:
  - heading: ARCADE
  - game: chicken
    name: CHICKEN     # optional, defaults to the title in its game.yml
```

`tools/gen_library.py` refuses, with a message naming the entry and the fix:

- a slug with no `games/<slug>/game.yml`, listing what does exist
- the same game on two rows, which would share one save between them
- a name with a character the font has no picture for
- a title too wide for the header, which has the battery icon at its right end
- a game built against another SDK, which cannot be linked in at all (rule 6)
- a menu with no games in it

A name too wide for its row is not on that list. The menu slides one back and
forth while its row is selected, holding still at each end, so a long name is
something to read rather than something to rename; an unselected row shows the
start of its name and stops at the row's edge. Only the selected row moves,
because seven names sliding at once is a menu nobody can read. The build log
still names every entry that will do it, so it is never a surprise on the
device. The console's own title is the exception and is still refused: the
header does not scroll, and a title that ran on would print through the
battery.

This is a different question from `build.yaml`, which says what CI builds for
the web gallery. A game held out of the web rotation can still be on the
console, and usually should be.

## The metadata block

Every device build still compiles a `.pse_meta` block carrying its slug,
title, version and a 48x48 icon (`tools/game_meta.py`). It is what the
standalone `.uf2` says about itself. The console does not read it: its menu
comes from `console_library.cpp`, generated from `console.yaml` at build time,
which is both simpler and checkable before the binary exists.

## What is not built

- The console is device and desktop only. It has no web page: the gallery
  publishes games individually, which is what rule 12 asks for, and a console
  in a browser tab would be a second way to play the same games.
- Nothing installs games onto a device that is already running. Changing what
  is on the console means building a console and flashing it. Both references
  work this way, and it is why neither of them needs a protocol.
