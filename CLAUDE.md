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

Publishing is limited to the default branch. A manual run on any other branch
builds and uploads artifacts but leaves the live site alone unless it is
dispatched with `publish` ticked on purpose. Never widen that: a branch build
quietly replacing the gallery is very hard to notice.

### 3. Pages is deployed by GitHub Actions, never from a branch

The Pages source for this repo is **GitHub Actions**. `actions/upload-pages-artifact`
plus `actions/deploy-pages` is how the site reaches the web, and that is not
negotiable. Do not switch the repo to "Deploy from a branch" and do not write a
pipeline that assumes a branch is being served: pushing to `gh-pages` publishes
nothing here.

The `gh-pages` branch still exists, but it is **state, not the served site**. It
carries the previous build forward so a game that did not change keeps the
binaries it already had. Every run assembles the complete site from that branch
plus whatever was just rebuilt, then uploads the whole tree as the Pages
artifact. That full upload is required, because `deploy-pages` replaces the
entire site on every deployment.

So both halves are load bearing:

- Drop the state branch and every game that was not rebuilt disappears from the
  site.
- Drop the artifact upload and nothing reaches the web at all.

Any job that changes what the site should look like has to end in an upload and
a deploy. `thumbnails.yml` does this too, otherwise a recaptured screenshot
would sit in the state branch and never be seen.

Jobs that deploy need `pages: write` and `id-token: write`, plus
`environment: github-pages`.

### 4. Branches deploy to previews, never to the root

Every push deploys. The default branch owns the site root; every other branch
publishes under `preview/<branch>/`. `tools/site_prefix.py` is the single place
that decides which, and both the detect job and the publish job must ask it,
because if they disagree a branch reads one set of fingerprints and writes
another and the build plan quietly stops meaning anything.

A branch reaching the site root takes an explicit `publish` dispatch. Do not
loosen that. Pull requests must never deploy at all, because a PR from a fork
would then have write access to the site.

Each preview carries its own `builds.json`, so a branch tracks its own state.
That is why the first push to a new branch rebuilds everything, and it is
correct rather than a bug in the skip logic.

### 5. Never rebuild or republish an unchanged game

CI minutes are the scarce resource here. A game is rebuilt only when its own
content hash changes, or when something it depends on (the shared engine, the
build tooling, the workflow itself) changes.

- Change detection lives in `.github/workflows/build.yml`, job `detect`.
- Each game's fingerprint is a hash of its directory plus its declared
  dependencies. See `tools/fingerprint.py`.
- Games that did not change keep their previously published web build and their
  previously published `.uf2`. They are copied forward, not rebuilt.
- When you add a new shared dependency, add it to the game's `depends_on` list
  in `game.yml`, otherwise the game will silently go stale.
- Never "just rebuild everything to be safe". If you think a rebuild is needed,
  fix the fingerprint inputs so the tool agrees with you.

### 6. One SDK: 32blit

Every game builds against the 32blit SDK, which targets the PicoSystem through
`-DPICO_BOARD=pimoroni_picosystem` and also builds for desktop and for the
browser.

Do not reach for the raw Pimoroni picosystem SDK. It is device only: no SDL
target, no Emscripten target, and its single SDL wrapper pull request was closed
unmerged in 2021 with the branch deleted. A game written against it can ship a
`.uf2` and can never have a playable page in the gallery, which breaks rule 12.
`tools/fingerprint.py` rejects `sdk: picosystem` rather than letting a game
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
- Core 1 is not yours to take. The 32blit pico backend claims it for display
  and audio when built with `ENABLE_CORE1`, which is why the 3D renderer
  rasterizes immediately on core 0 instead of queueing triangles for a second
  core. That choice also gave back the 84 KB the old triangle lists cost.
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
  installed. The shell passes `--size 240,240` so the browser surface matches
  the device exactly: the SDL backend otherwise defaults to 320x240, and a game
  drawing to its own fixed bounds ends up with black margins on the web that it
  does not have on hardware. Do not fall back to the SDK's stock shell: it is keyboard only,
  so a phone can load the game and then not play it.
- The shell synthesises keyboard events rather than calling into the engine.
  Keep it that way: it means the page needs no per game knowledge, and a change
  to the C++ button mapping does not break it. `web` is in every game's
  `depends_on` so editing the shell actually rebuilds the games.
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
engine/            shared library, SDK facing code, no game rules
games/<slug>/      one game: game.yml, CMakeLists.txt, src/, assets/, models/
cmake/             reusable CMake helpers (game registration, obj packaging)
tools/             build tooling, gallery generator, flasher utility
web/               gallery templates and the emscripten page shell
.github/workflows/ the build and publish pipeline
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

## Before you open a PR

- The workflow builds clean from a cold cache.
- No game rebuilt that did not need to rebuild. Check the `detect` job summary.
- RAM and flash deltas stated for anything touching the device build.
- No em dashes.
