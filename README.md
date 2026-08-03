# pico-stanta

PicoSystem games, built and published automatically.

Push a change, and CI rebuilds only the games that actually changed, publishes
each one to its own URL, and leaves everything else alone.

**Gallery:** https://rubentipparach.github.io/pico-stanta/

## What is here

```
engine/            shared renderer, no SDK dependency except one adapter file
games/<slug>/      one game: game.yml, CMakeLists.txt, src/, assets/, models/
cmake/             reusable CMake helpers (game registration, obj packaging)
tools/             build tooling, gallery generator, flasher utility
web/               gallery stylesheet and the Emscripten page shell
.github/workflows/ the build and publish pipeline
```

Two games ship today:

| Game | URL | What it is |
| --- | --- | --- |
| Chicken | `/chicken/` | Endless side scroller, chunked infinite level |
| Pico Santa | `/pico-santa/` | Chunked 3D city, software rasterizer, .obj models |

## How the pipeline works

1. **`engine-tests`** builds and runs the host test suite. No SDK, no cross
   compile, fails in seconds if the renderer is broken.
2. **`detect`** hashes each game directory plus everything it declares a
   dependency on, and compares that against the hashes recorded on the live
   site. Only games whose hash moved get built.
3. **`build`** runs once per stale game: a `.uf2` for the PicoSystem and an
   Emscripten build for the browser.
4. **`publish`** overlays the rebuilt games onto the `gh-pages` branch,
   regenerates the gallery, and pushes. Games that were not rebuilt keep the
   binaries they already had.

Change detection is content based rather than git-diff based, so it stays
correct across force pushes, re-runs, and reverts. Reverting a change restores
the old hash, so nothing rebuilds, which is the right answer.

To force a rebuild, run the workflow manually and pass slugs (or `all`) to the
`force` input. To force everything permanently, bump `.build-epoch`.

## Adding a game

Create `games/<slug>/` with a `game.yml` and a `CMakeLists.txt`, and commit. CI
discovers it, builds it, publishes it at `/<slug>/`, and captures its first
thumbnail. You never edit the top level build, the workflow, or the gallery.

See [`games/README.md`](games/README.md) for the field reference.

## Thumbnails

CI screenshots a game once, the first time it is published, and then never
touches it again. Rebuilding a game does not refresh its thumbnail, so the
gallery does not churn.

To refresh one deliberately, run the **Capture thumbnails** workflow and name
the game, or commit a PNG at `games/<slug>/thumbnail.png`.

## Models

Models are `.obj` files under `games/<slug>/models/`, editable in Blender or
anything else. `tools/obj2cpp.py` converts them to a `const` C++ table at build
time, so they live in flash and cost no RAM. Nothing generated is committed.

```
models/gem.obj      6 vertices, 8 triangles, 144 bytes of flash
models/sleigh.obj  16 vertices, 16 triangles, 300 bytes of flash
```

Materials come from the sidecar `.mtl`. A `usemtl` with no matching entry gets a
colour derived from its name, so a multi part model never silently flattens to
one grey.

## Getting a game onto the device

Download [`PicoFlasher.exe`](tools/flasher/README.md) from the latest release,
put the board in BOOTSEL (hold **X**, press power), pick a `.uf2`, press Flash.

By hand: drop the `.uf2` onto the `RPI-RP2` drive. Windows may report an error
at the end of the copy. That is expected and means it worked, see the flasher
README for why.

## Building locally

You need the 32blit SDK and the Pico SDK checked out next to this repo.

```
git clone https://github.com/32blit/32blit-sdk
git clone https://github.com/raspberrypi/pico-sdk
git -C pico-sdk submodule update --init lib/tinyusb
```

Host tests, which need nothing else:

```
cmake -S . -B build.test -DBUILD_ENGINE_TESTS=ON
cmake --build build.test
ctest --test-dir build.test --output-on-failure
```

A `.uf2` for the device:

```
cmake -S . -B build.pico -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=../32blit-sdk/pico.toolchain \
  -DPICO_BOARD=pimoroni_picosystem \
  -DCMAKE_BUILD_TYPE=Release
cmake --build build.pico
```

The browser build, with the Emscripten SDK active. `32BLIT_DIR` must be an
absolute path here:

```
emcmake cmake -S . -B build.web -D32BLIT_DIR="$PWD/../32blit-sdk"
cmake --build build.web
```

Add `-DPICO_STANTA_ONLY_GAME=<slug>` to any of these to build a single game.

## Repository settings this depends on

The site is published from a branch, not from the Pages action. In
**Settings > Pages**, set the source to **Deploy from a branch**, branch
`gh-pages`, folder `/`.

This is deliberate. `actions/deploy-pages` replaces the entire site on every
deploy, which would delete every game that was not rebuilt that run. Keeping the
built site on a branch makes git itself the durable state, so an incremental
overlay is possible and a bad publish can be reverted.

## Conventions

Project rules live in [`CLAUDE.md`](CLAUDE.md): no em dashes, SOLID, one SDK,
never rebuild an unchanged game, respect how small this device is, keep the
on-screen UI sparse, models come from `.obj` files.
