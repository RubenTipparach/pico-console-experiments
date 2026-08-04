# pico-console-experiments

PicoSystem games, built and published automatically.

Push a change, and CI rebuilds only the games that actually changed, publishes
each one to its own URL, and leaves everything else alone.

**Gallery:** https://rubentipparach.github.io/pico-console-experiments/

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
4. **`publish`** overlays the rebuilt games onto the site state held on the
   `gh-pages` branch, regenerates the gallery, then uploads the complete tree
   and deploys it to Pages. Games that were not rebuilt keep the binaries they
   already had.

Change detection is content based rather than git-diff based, so it stays
correct across force pushes, re-runs, and reverts. Reverting a change restores
the old hash, so nothing rebuilds, which is the right answer.

To force a rebuild, run the workflow manually and pass slugs (or `all`) to the
`force` input. To force everything permanently, bump `.build-epoch`.

## Playing on a phone

Every web build ships with an on-screen gamepad and a fullscreen button, so a
game is playable on a touch device with nothing installed. Open the game's URL
and the pad appears on its own.

- The D-pad and the A/B/X/Y buttons match the console's own layout. Sliding a
  thumb between D-pad arrows works, so diagonals and rolls behave like a real
  pad.
- **Fullscreen** takes the whole page, not just the canvas, so the pad stays on
  screen. It also asks for a landscape lock where the browser allows one.
- **Controls** hides or shows the pad, and the choice is remembered. The pad is
  on by default on touch devices and off on desktop.
- In landscape the pad moves to flank the screen instead of sitting under it.

On desktop the keyboard still works: arrows or WASD to move, `Z X C V` for
A/B/X/Y.

The page is `web/shell.html`, used for every game. It synthesises keyboard
events rather than calling into the engine, so it needs no per game knowledge
and keeps working if the C++ button mapping changes.

iPhone Safari has no Fullscreen API, so the button does nothing there. The
layout is sized with `dvh` and the safe-area insets so it fits anyway.

## Branch previews

Every push, on every branch, builds and deploys. The default branch owns the
site root; every other branch gets its own preview URL:

```
main            -> /
my-branch       -> /preview/my-branch/
feature/pad     -> /preview/feature-pad/
```

So you can push and reload on your phone without opening a PR, and a branch can
never overwrite the live gallery.

Each preview keeps its own `builds.json`, so a branch tracks its own build
state. The first push to a new branch rebuilds everything, because that preview
has nothing published yet; pushes after that only rebuild what changed.

To deploy a non default branch to the **root**, run the workflow manually with
`publish` ticked. That is the only way a branch reaches the live gallery, and it
is off by default because a branch silently replacing the site is hard to spot.

Pull requests build but never deploy: a PR from a fork would otherwise get write
access to the site.

## Building a branch without publishing

`workflow_dispatch` runs the pipeline on any branch you pick in the Actions UI.
By default it builds and uploads artifacts but **does not touch the live site**:
publishing only happens automatically on the default branch.

- **Run workflow** on any branch: builds, and attaches the `.uf2` and web build
  as run artifacts. Download them from the run page.
- `force`: slugs to rebuild even when unchanged, comma separated, or `all`.
  Useful on a fresh branch where the fingerprints still match the live site.
- `publish`: tick this to overwrite the live site from that branch. Off by
  default, because a branch build silently replacing the gallery is a nasty
  surprise.

Pull requests build too, and never publish.

Pushes to a non default branch do not build on their own. Open a PR or dispatch
manually. That is deliberate: building every push on every branch is exactly the
CI spend this pipeline exists to avoid.

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

Add `-DPICO_ONLY_GAME=<slug>` to any of these to build a single game.

## Repository settings this depends on

The Pages source is **GitHub Actions**, not "Deploy from a branch". The workflow
uploads the site with `actions/upload-pages-artifact` and deploys it with
`actions/deploy-pages`.

Nothing else needs configuring. If the site ever 404s, check that the Pages
source is still set to GitHub Actions: with it set to "Deploy from a branch"
instead, `deploy-pages` is unavailable and the deployment step fails.

### Why there is still a gh-pages branch

`deploy-pages` replaces the entire site on every deployment, so a run that
rebuilt one game would wipe every other game if it uploaded only what it built.

The `gh-pages` branch is the durable state that prevents that. It holds the last
published site, so each run can assemble the complete tree from it plus whatever
was just rebuilt, and upload the whole thing.

It is state, not the served site. Pushing to it publishes nothing on its own.

## Conventions

Project rules live in [`CLAUDE.md`](CLAUDE.md): no em dashes, SOLID, one SDK,
never rebuild an unchanged game, respect how small this device is, keep the
on-screen UI sparse, models come from `.obj` files.
