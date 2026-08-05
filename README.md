# pico-console-experiments

PicoSystem games, built and published automatically.

Push a change, and CI rebuilds only the games that actually changed, publishes
each one to its own URL, and leaves everything else alone.

**Gallery:** https://rubentipparach.github.io/pico-console-experiments/

## What is here

```
engine/            shared renderer, no SDK dependency except one adapter file
games/<slug>/      one game: game.yml, CMakeLists.txt, src/, assets/, models/
console/           the multi game console: menu, dispatch, its own tests
console.yaml       which games are on the console, and in what order
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
| Kingfisher | `/kingfisher/` | 3D fishing across a day cycle, both RP2040 cores rendering |

## How the pipeline works

1. **`engine-tests`** builds and runs the host test suite. No SDK, no cross
   compile, fails in seconds if the renderer is broken.
2. **`detect`** hashes each game directory plus everything it declares a
   dependency on, and compares that against the hashes recorded on the live
   site. Only games whose hash moved get built.
3. **`build`** runs once per enabled game: a `.uf2` for the PicoSystem and an
   Emscripten build for the browser.
4. **`publish`** overlays the games this run built onto the site state held on
   the `gh-pages` branch, regenerates the gallery, then uploads the complete
   tree and deploys it to Pages. Held games keep the binaries they already had.

What gets built is written down in [`build.yaml`](build.yaml), not inferred.
Every game under `build:` is built on every run; `hold:` takes a game out of
the rotation while leaving its published build on the site; a game in neither
list is built by default. Adding a game is still just adding a directory.

To build one held game without editing the config, run the workflow manually
and name it in the `games` input.

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

Only `main` builds on a push. Every other branch is manual: `workflow_dispatch`
runs the pipeline on any branch you pick in the Actions UI, and it deploys to
`preview/<branch>/` rather than to the live gallery.

- **Run workflow** on any branch: builds, deploys the preview, and attaches the
  `.uf2` and web build as run artifacts. Download them from the run page.
- `games`: slugs to build, comma separated. Empty builds everything `build.yaml`
  enables; naming a game overrides the config, including a held one.
- `publish`: tick this to overwrite the live site from that branch. Off by
  default, because a branch build silently replacing the gallery is a nasty
  surprise.

Pull requests do not build at all, so the merge to `main` is what proves the
tree.

Pushes to a non default branch do not build on their own. Open a PR or dispatch
manually. That is deliberate: building every push on every branch is exactly the
CI spend this pipeline exists to avoid.

## Adding a game

Create `games/<slug>/` with a `game.yml` and a `CMakeLists.txt`, and commit. CI
discovers it, builds it, publishes it at `/<slug>/`, and captures its first
thumbnail. You never edit the top level build, the workflow, or the gallery.

See [`games/README.md`](games/README.md) for the field reference.

## Thumbnails

One picture per game, `games/<slug>/thumbnail.png`, doing three jobs: the
gallery card shows it, the 48x48 icon inside the `.uf2` is resampled from it,
and so is the 24x24 icon on the console's menu row. A device never reaches the
site, so a game with no committed thumbnail has a generated placeholder in the
menu.

CI screenshots a game once, the first time it is published, and then never
touches it again. Rebuilding a game does not refresh its thumbnail, so the
gallery does not churn. A committed PNG always wins over a captured one.

To make one deliberately, run the game's preview harness and convert the frame
you want:

```
./build.test/games/dustrider/tests/dustrider_preview /tmp/dr
python3 tools/make_thumbnail.py --ppm /tmp/dr/preview_4_bend.ppm \
    --out games/dustrider/thumbnail.png
```

Or run the **Capture thumbnails** workflow and name the game.

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

## Every game at once

`console.uf2` is one file holding the menu and every game `console.yaml`
lists. Flash it the same way. Up and down move, any button starts a game, and
holding up and down together for a moment comes back to the menu.

There are no slots and nothing is relocated: the games are linked together and
the menu calls one of them, which is how the two projects that have shipped a
multi-game PicoSystem do it. [CONSOLE.md](CONSOLE.md) has the details and the
RAM numbers. `build_console.bat` builds it locally; CI attaches it to every
run as the `console` artifact.

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

The console, every game plus the menu in one `.uf2`:

```
cmake -S . -B build.console -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=../32blit-sdk/pico.toolchain \
  -DPICO_BOARD=pimoroni_picosystem \
  -DCMAKE_BUILD_TYPE=Release \
  -DPSE_CONSOLE=ON
cmake --build build.console
```

Drop the toolchain lines for a desktop build you can actually sit and play,
which is how the menu and the game switching were checked.

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
what builds is a decision in `build.yaml`, respect how small this device is,
keep the on-screen UI sparse, models come from `.obj` files.
