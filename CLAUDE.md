# CLAUDE.md

Guidance for Claude Code when working in this repository.

This repo is a multi-game monorepo for the Pimoroni PicoSystem. It holds a shared
engine, one directory per game, an automated build and publish pipeline, and a
desktop flashing utility.

## Non-negotiable rules

These are project rules. Follow them without being reminded.

### 1. No em dashes

Never use an em dash (`—`) in any file in this repo: code, comments, commit
messages, PR bodies, docs, gallery copy, UI strings. Use a comma, a colon,
parentheses, or two sentences instead. En dashes in numeric ranges are fine.

### 2. Every build is automated

All builds run through GitHub Actions. If a build step only works on one
person's machine, it is not done. Local scripts exist for convenience, but the
workflow in `.github/workflows/` is the source of truth. When you change how a
game builds, change the workflow, not just the local script.

A push to main builds and publishes; any other branch builds only when it is
dispatched by hand, and then it deploys under `preview/<branch>/` unless the
run is dispatched with `publish` ticked on purpose. Never widen that: a branch
build quietly replacing the gallery is very hard to notice.

### 3. Pages is deployed by GitHub Actions, never from a branch

The Pages source for this repo is **GitHub Actions**. `actions/upload-pages-artifact`
plus `actions/deploy-pages` is how the site reaches the web, and that is not
negotiable. Do not switch the repo to "Deploy from a branch" and do not write a
pipeline that assumes a branch is being served: pushing to `gh-pages` publishes
nothing here.

The `gh-pages` branch still exists, but it is **state, not the served site**. It
carries the previous build forward so a held game keeps the binaries it
already had. Every run assembles the complete site from that branch plus
whatever it just built, then uploads the whole tree as the Pages artifact. That full upload is required, because `deploy-pages` replaces the
entire site on every deployment.

So both halves are load bearing:

- Drop the state branch and every held game disappears from the site.
- Drop the artifact upload and nothing reaches the web at all.

Any job that changes what the site should look like has to end in an upload and
a deploy. `thumbnails.yml` does this too, otherwise a recaptured screenshot
would sit in the state branch and never be seen.

Jobs that deploy need `pages: write` and `id-token: write`, plus
`environment: github-pages`.

### 4. main builds on every push; every other branch is manual

A push to main builds and publishes, always. No other branch builds by
itself: pushing to a branch does nothing, and a branch build is a deliberate
`workflow_dispatch`. Pull requests do not build either, so a PR carries no CI
signal of its own and the merge to main is what proves the tree.

Building is publishing. Any run that builds also deploys what it built, so
there is no such thing as a green build that never reached a site. The
default branch owns the site root; a dispatched branch publishes under
`preview/<branch>/`. `tools/site_prefix.py` is the single place that decides
which, and both the detect job and the publish job must ask it.

A branch reaching the site root takes an explicit `publish` dispatch. Do not
loosen that: a branch quietly replacing the gallery is very hard to notice.

Pages caches every file for ten minutes under unchanging names, which used to
make a fresh deploy look stale on a device that had just loaded the previous
build. The publish step stamps each rebuilt game's page and asset URLs with
the build version, gallery links carry that version and re-check a freshly
fetched manifest, and the shell offers a one tap reload when a newer build
lands. The version reaches the page as a window.PSE_BUILD global injected
after the body tag at publish time, and the shell reads it with a bracketed
window lookup on purpose: Emscripten minifies the shell, and any compile
time constant in there gets folded and the whole mechanism dead code
eliminated. Publish time data must never travel through the minifier.

### 5. What builds is a decision in `build.yaml`, not an inference

Every enabled game is built on every run, whether or not anything under it
changed. There is no change detection: the build list is written down, so a
build is reproducible from the config alone and no game is ever skipped for a
reason nobody can see.

- `build.yaml` at the repo root is the switchboard. `build:` is the rotation,
  `hold:` takes a game out of it, and a game in neither list is built, so
  adding a game is still just adding a directory.
- A held game is not built and not touched. Its last published build stays on
  the site and in the gallery, carried forward from the gh-pages state
  branch, with the commit it was published from. Holding a game hides nothing
  from players.
- A slug in either list that no longer exists is a hard error. A typo under
  `hold` would otherwise read as "unlisted", which means the opposite.
- `tools/build_plan.py` turns that file into the matrix, and the run summary
  names what was built, what was held, and anything the config never
  mentioned.
- A manual run can name games explicitly, which overrides both lists. That is
  how to rebuild one held game without editing the config.
- The pico builds reuse objects through ccache, cached per game.
  The Emscripten builds use NO compile cache, deliberately: a warm ccache
  once shipped a wasm whose EM_ASM addresses no longer matched its JS, so
  the game compiled green and died on its first frame. Web correctness
  beats web build speed; do not re-add caching there.
- Compiling proves nothing about booting. The publish job boot checks every
  rebuilt web game in a real browser (`tools/boot_check.py`) and fails the
  deploy on any page error, so a dead game cannot replace a live one. The
  shell also shows any runtime crash on the page with a copy button, so a
  phone can report exactly what broke and from which build.
- That browser comes from `tools/setup_browser.sh`, which never touches apt.
  `playwright install --with-deps chromium` pulls Chromium's whole system
  dependency set, 21 MB of it CJK fonts that a canvas game has no use for,
  and that apt run hung a publish job on the archive mirror. Prefer the
  browser the runner already ships, fall back to Playwright's download, and
  do not add `--with-deps` back.

### 6. One SDK: 32blit

Every game builds against the 32blit SDK, which targets the PicoSystem through
`-DPICO_BOARD=pimoroni_picosystem` and also builds for desktop and for the
browser.

Do not reach for the raw Pimoroni picosystem SDK. It is device only: no SDL
target, no Emscripten target, and its single SDL wrapper pull request was closed
unmerged in 2021 with the branch deleted. A game written against it can ship a
`.uf2` and can never have a playable page in the gallery, which breaks rule 12.
`tools/build_plan.py` rejects `sdk: picosystem` rather than letting a game
publish a dead URL.

Game code should not call the SDK where the engine already abstracts it.
`engine/src/blit_target.cpp` is the only file in the engine that includes
`32blit.hpp`, and it should stay that way: everything else is plain C++ against
a `pse::RenderTarget`, which is why the renderer compiles unchanged for device,
desktop, web, and the host test binary.

### 7. SOLID

The engine is a library, the games are consumers. Keep it that way.

- **Single responsibility.** `engine/` modules do one thing: rasterize, project,
  load a mesh, read input. Game rules live in `games/<slug>/src/`. Do not add
  a game specific special case to the engine.
- **Open/closed.** Adding a game must not require editing engine source, the
  gallery template, or the workflow. Adding a game means adding a directory with
  a `game.yml`. The build discovers it.
- **Liskov.** Anything typed as a `pse::Renderer` or a `pse::Mesh` must be usable
  without the caller knowing the concrete type.
- **Interface segregation.** A 2D game must not be forced to link the 3D
  rasterizer or the depth buffer. Engine targets are split so a game links only
  what it uses.
- **Dependency inversion.** Game code depends on engine headers, never on the
  SDK directly where an engine abstraction exists. The SDK backend is selected
  at the CMake level.

Practical consequence: no globals shared between engine and game. The city's
buildings and gems used to be file scope arrays that the game reached into
directly; they are owned by `santa::City` now. Keep it that way.

### 8. The PicoSystem is tiny. Budget everything.

Hardware: RP2040 dual core Cortex-M0+ at 133 MHz (no FPU), 264 KB SRAM, 16 MB
flash, 240x240 LCD, 4 bit per channel colour.

Before adding anything, ask what it costs:

- A full 240x240 16 bit framebuffer is 115 KB. Two of them will not fit
  alongside a game. `set_screen_mode(ScreenMode::lores)` halves it to 120x120
  (28.8 KB per buffer) and that is why the 3D game uses lores.
- There is no hardware FPU. `float` maths is software emulated. Use fixed point
  in anything that runs per pixel or per vertex. The rasterizer uses a 1024
  scale fixed point format (`pse::k_fixed_one`), keep to it. Model coordinates
  are separate: `MeshData::scale` says how many units make a world unit.
- Large constant data belongs in flash, not RAM. Mark it `const` (and prefer
  `static const` at file scope) so it stays in XIP flash. A non const global
  array is RAM you have permanently spent.
- Prefer fixed size arrays with an `active` flag over dynamic allocation. Do not
  introduce `new`, `malloc`, `std::vector`, or `std::string` into game or engine
  hot paths.
- Core 1 is available on the PicoSystem, and the engine uses it. The 32blit
  pico backend only claims core 1 under `ENABLE_CORE1`, which its CMake sets
  for scanvideo display boards alone; the PicoSystem uses the dbi driver with
  audio on a core 0 hardware alarm, so core 1 idles. `pse::run_split()` gives
  it the bottom half of the screen: collect triangles with
  `begin_frame_collect`, run_split rasterizes both halves at once, each core
  owning disjoint rows so there are no locks. A queue with `mark_split()`
  renders as two independent scenes, one per band with its own gradient,
  which is how a split screen game gives each core a whole scene. The host
  tests prove both forms byte identical to single core rendering.
- The flash safety contract that comes with that: `write_save` disables XIP
  while it programs flash, and core 1 survives only because its idle loop is
  RAM resident (`__not_in_flash_func` in `parallel_pico.cpp`). Save outside
  the render call, never during it, and never move that worker loop into
  flash. The clock is 250 MHz on both cores (`OVERCLOCK_250` defaults on).
- Flash is the roomy resource: 12 MB for the program and baked assets, plus
  a 4 MB FAT partition the SDK mounts for saves and loose files. How to use
  both, and how a multi-game library would work, is documented in
  `STORAGE.md`. Read it before touching persistence, the storage partition,
  a launcher, or any game big enough to care about asset budgets.
- If you add a feature, state its RAM and flash cost in the PR body.

### 9. Keep on-device UI sparse

The screen is 240x240, or 120x120 when pixel doubled. Text is expensive to read
and expensive to draw.

- Show the minimum: score, state, and the one thing the player needs right now.
- No tutorial paragraphs, no explanatory subtitles, no decorative status lines.
- Debug overlays (frame time, triangle count, CPU split) are opt in behind a
  build flag, not on by default in a published build.
- Same rule for the web gallery: short labels, no marketing copy.
- If the user explicitly asks for more text, give them more text. This rule is a
  default, not a veto.

### 10. Mock up large UI changes before building them

If a change materially alters the gallery, a game's title screen, or the flasher
utility layout, produce a mockup first and get the user to confirm it. A mockup
is a rendered HTML page, an image, or an ASCII layout, whatever communicates
fastest. Do not spend a long implementation on an unconfirmed design.

Small changes (a label, a colour, one control) do not need a mockup.

### 11. Models come from .obj files, not from code

Do not hand write vertex tables in C++. Do not procedurally emit geometry in
source when a real model would do.

- Author models as `.obj` in `games/<slug>/models/` so they can be edited in
  Blender or any modeller.
- `tools/obj2cpp.py` converts them to a compact `const` C++ table at build time
  via `cmake/obj_model.cmake`. Generated files land in the build directory and
  are never committed.
- Materials come from the sidecar `.mtl` if present, otherwise per face colours
  come from the model's `usemtl` names.
- The exception is trivial primitives (a cube, a quad) that the engine already
  generates parametrically. Those stay in code.

### 12. Every game gets its own URL, a gallery entry, and touch controls

- The gallery lives at the Pages root and lists every game.
- Each game is published at `/<slug>/`, where `slug` comes from `game.yml`.
- Games buildable for web get a "Play" link. Games that are device only get a
  "Download .uf2" link and say so on the card.
- Every web build uses `web/shell.html`, which carries the on-screen gamepad
  and the fullscreen button. Games must stay playable on a phone with nothing
  installed. Two SDK behaviours govern how the screen is sized, and both have
  bitten already, so do not "simplify" this:
  - The shell passes `--size 240,240`, because the SDL backend defaults to
    320x240 while the PicoSystem board config is 240x240. Without it a game
    drawing to its own bounds gets black margins on the web it does not have on
    hardware.
  - The canvas is pinned to 240x240 in CSS and magnified with a `transform`,
    never sized responsively. The SDL window is `SDL_WINDOW_RESIZABLE`, so
    Emscripten keeps the render buffer in step with the element's CSS box, and
    `Renderer.cpp` hardcodes `KeepPixels`, which turns on integer scaling with
    no switch to disable it. A responsive CSS size gives an arbitrary buffer
    like 358, `floor(358 / 240)` is 1, and the game sits small in a black
    frame. Pinning the buffer keeps the integer scale at exactly 1x (hires) or
    2x (lores). Media queries must resize `#frame`, never the canvas. Do not fall back to the SDK's stock shell: it is keyboard only,
  so a phone can load the game and then not play it.
- The shell synthesises keyboard events rather than calling into the engine.
  Keep it that way: it means the page needs no per game knowledge, and a change
  to the C++ button mapping does not break it. Editing the shell reaches every
  game on the next build, since every enabled game is rebuilt every run.
- **Thumbnails are manual.** CI captures a screenshot only the first time a game
  is published, when `games/<slug>/thumbnail.png` does not exist yet. After that
  the committed thumbnail is left alone forever, even when the game changes.
  Refreshing a thumbnail is an explicit user request, done by running the
  `Capture thumbnails` workflow with the game named, or by committing a new PNG.
  Never refresh thumbnails on your own initiative.

### 13. Flashing to the device is a one click job

`tools/flasher/` is a small Windows Forms utility. It watches for a PicoSystem in
BOOTSEL mode and copies a selected `.uf2` across. Keep it quick and dirty on
purpose: single form, no installer, no framework churn. It is a tool, not a
product.

## Repository layout

```
build.yaml         which games CI builds, and which are held
engine/            shared library, SDK facing code, no game rules
games/<slug>/      one game: game.yml, CMakeLists.txt, src/, assets/, models/
cmake/             reusable CMake helpers (game registration, obj packaging)
tools/             build tooling, gallery generator, flasher utility
web/               gallery templates and the emscripten page shell
.github/workflows/ the build and publish pipeline
STORAGE.md         the 16 MB flash: persistence, game library, larger games
```

## Adding a game

1. `mkdir games/<slug>` and write `game.yml` (see `games/README.md` for fields).
2. Add `CMakeLists.txt` calling `add_picosystem_game()`.
3. Put sources in `src/`, art in `assets/`, models in `models/`.
4. Commit. CI discovers it, builds it, publishes it at `/<slug>/`, and captures
   its first thumbnail.

Do not edit the workflow, the gallery template, or the top level CMakeLists to
add a game. If you find yourself needing to, the discovery mechanism is broken
and that is the bug to fix.

### 14. Sync with main before every push

Before each push to any branch, the branch must be synced up with main so it
carries the latest version: fetch main and rebase the branch onto it (or
restart the branch from main when its previous PR already merged). A push
from a stale base ships code that was never built against what main has
become, and the preview it deploys tests yesterday's repo with today's
change. Never force push over commits that are not already merged into main.

## Before you open a PR

- The branch is freshly synced with main (rule 14).
- The workflow builds clean from a cold cache.
- No game rebuilt that did not need to rebuild. Check the `detect` job summary.
- RAM and flash deltas stated for anything touching the device build.
- No em dashes.
