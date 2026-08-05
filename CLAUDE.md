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

Copying a rebuilt game over the state it carried forward must compare
**contents**, never size and timestamps. `rsync -a` alone calls a file
unchanged when its size and mtime match, and Emscripten's `index.js` is
built to defeat exactly that: its `ASM_CONSTS` keys are addresses into the
wasm's data, so they move whenever the game's data moves, but they keep the
same number of digits, so the file's size never changes. The site checkout
and the artifact download land in the same second, so the mtimes match too.
Every rebuild kept the first `index.js` a game ever published while
replacing its wasm, and a game rode that until its EM_ASM addresses shifted
and it died on boot. `--checksum` is not an optimisation to tune away.

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
- Compiling proves nothing about booting, and checking the build artifact
  proves nothing about what ships. The publish job boot checks the
  **assembled site**, after the overlay and the stamping, in a real browser
  (`tools/boot_check.py`), and fails the deploy on any page error. It used
  to check the incoming artifact instead, and a publish that dropped a
  game's `index.js` on the way to the site went out green: the artifact
  booted perfectly and something else was deployed. Check the tree that is
  uploaded, never an earlier copy of it. The shell also shows any runtime
  crash on the page with a copy button, so a phone can report exactly what
  broke and from which build.
- A red run must not publish, and must not claim it did. The publish job runs
  under `!cancelled()` so it can still deploy when the build was legitimately
  skipped, and that let a run through where the engine tests had **failed**
  and the build never ran: it deployed the previous binaries and stamped the
  manifest with the new commit, so the site reported a version it was not
  serving. Every job the publish depends on now has to have passed, and a
  skipped build is only acceptable when the detect job found no work. The
  manifest's `built` flag is read from the files actually present in the
  incoming artifact, never from what an earlier job said it intended to
  build. A green badge on a red run is worse than a red one, because it
  removes the reason to look.
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
  the console, or any game big enough to care about asset budgets.
  `CONSOLE.md` is the design built on top of it: every game linked into one
  binary next to a menu that calls one of them, which is how both projects
  that have shipped a multi-game PicoSystem do it. There is no relocation,
  no slot linking and no bundle composer, and there should not be one again:
  that was tried, specified in detail, and never booted.
- **The whole device maxes out at 16 MB total** (confirmed against Pimoroni's
  own spec for the PicoSystem's QSPI flash chip, not assumed). That is the
  entire chip, the 12 MB program region and the 4 MB storage partition
  together, not a per-operation limit. It matters beyond the firmware itself:
  BOOTSEL exposes the device as a fake FAT16 drive with that same 16 MB
  ceiling, and the UF2 format costs 512 bytes on disk per 256 bytes of real
  flash data, exactly doubling a payload's size, so a single flash operation
  cannot represent more than about 8 MB of actual flash content before the
  `.uf2` file itself exceeds what the drive can hold. The bundle composer
  that used to live in `tools/flasher` learned this the hard way: an early
  "clear every game slot" action wrote the full 512 KB of all 23 slots in one
  UF2, producing a file bigger than the drive it had to fit through. Both the
  composer and the slots are gone, but the ceiling is not, and anything that
  ever writes many blocks at once has to budget against it. For scale, the
  measured real flash content (uf2 file size / 2) of everything this project
  builds or has imported:
  chicken.uf2 110,080 B, pico-santa.uf2 118,528 B,
  dustrider.uf2 132,096 B, kingfisher.uf2 141,056 B (the largest single game
  this project builds), raycaster.uf2 146,432 B, Daft-Freak.uf2 153,344 B,
  celeste.uf2 300,288 B, pico3d.uf2 323,584 B. The console, which holds four
  of those games and a menu, is one file of about the same order, because
  what four games cost is four games and not four slots.
  Anything that touches many slots at once has to budget its `.uf2` file
  size against the 16 MB drive ceiling, using a real measured figure for how
  much of a slot actually needs reaching, not a guessed one.
- If you add a feature, state its RAM and flash cost in the PR body.

### 9. Keep on-device UI sparse

The screen is 240x240, or 120x120 when pixel doubled. Text is expensive to read
and expensive to draw.

- Show the minimum: score, state, and the one thing the player needs right now.
- No tutorial paragraphs, no explanatory subtitles, no decorative status lines.
- **No button prompts.** Do not spend a line telling the player which button
  starts, retries, or continues. Accept any button instead: with nothing on
  screen naming one, no press can be the wrong guess, and the line disappears.
  The gallery card already lists the controls for anyone who wants them.
- Debug overlays (frame time, triangle count, CPU split) are opt in behind a
  build flag, not on by default in a published build.
- Same rule for the web gallery: short labels, no marketing copy.
- **Measure text, never place it by eye.** `screen.measure_text()` gives the
  width; centre and size panels from it. A hand tuned x is only correct for
  the exact string it was tuned against, so the first wording change prints
  through the edge of its own panel, and nothing catches it because it still
  compiles and still runs.
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
- A game's models are listed once, in `games/<slug>/models.cmake`, and both the
  game and the host preview harness include that file. They compile the same
  `src/render.cpp`, so a model named for one and not the other is a missing
  header, not a missing picture: adding `rock.obj` to the game while the
  preview kept its own list turned main red. Add a model in one place only.

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
- **Every web game ships a mini tutorial on its own page.** It is the first
  thing a player meets, and a game nobody can work out is a game nobody plays.
  Panel one is the objective, what the game is asking of them. The rest are the
  controls, each key beside what it actually does, split across panels with
  arrows when there are more than four. The page used to say
  `Arrows or WASD - Z X C V`: the keys the SDL build reads, with nothing about
  what any of them does in the game in front of you. Never ship that again.
  - Write it in `game.yml` as `objective` and `controls`, nowhere else.
    `tools/gen_shell.py` builds the panels into that game's page at configure
    time, so the shell in `web/` stays game agnostic and there is no second
    copy of the text to keep in sync.
  - Controls are `"key: what it does"`. A key on its own is noise; the half
    after the colon is the whole point. A line with no colon is kept whole,
    for what a game needs to say that is not one button.
  - Name the console buttons (`A`, `B`, `X`, `Y`, `up`, `left`...). The
    keyboard key is added beside each one automatically, read out of the
    on-screen gamepad's own markup, which is what those buttons dispatch.
    Never write the keyboard key into `game.yml`: `A` is `Z` in exactly one
    place, and a second copy is a mapping waiting to go stale.
  - `tools/tests/test_gen_shell.py` walks every game in the repo and fails the
    build when one that ships to the web has no objective or no controls. That
    check is the rule; do not weaken it to get a game out.
- **Thumbnails are manual.** CI captures a screenshot only the first time a game
  is published, when `games/<slug>/thumbnail.png` does not exist yet. After that
  the committed thumbnail is left alone forever, even when the game changes.
  Refreshing a thumbnail is an explicit user request, done by running the
  `Capture thumbnails` workflow with the game named, or by committing a new PNG.
  Never refresh thumbnails on your own initiative.
- **One picture, three places.** `games/<slug>/thumbnail.png` is the gallery
  card, the 48x48 icon `game_meta.py` puts inside the standalone `.uf2`, and
  the 24x24 row icon `gen_library.py` puts in the console menu. A device never reaches the site, so a screenshot that exists only on
  `gh-pages` leaves the menu showing a coloured rectangle. When a thumbnail is
  asked for, commit it: the publish step copies a committed PNG over whatever
  was captured, so one file keeps both honest.
- **Screenshots come from the preview harness, not from a running device.**
  `<game>_preview` renders real frames through the real engine at the native
  120x120, and `tools/make_thumbnail.py` doubles them to 240x240 with no
  filtering, which is what the hardware does to reach its panel. Do not
  hand-draw a thumbnail and do not scale one with a filter: a soft screenshot
  reads as a soft game.
- **`games/<slug>/thumbnail.png` is 120x120.** The native render size, not the
  240x240 the gallery card doubles it to for display: `game_meta.py`'s icon
  pipeline center-crops whatever it is handed to a square and box-averages it
  down to the 48x48 the `.uf2` actually carries, so an off-size thumbnail
  still produces a correctly sized icon, but a non-120x120 source (a browser
  screenshot from the older `gh-pages` capture path, at whatever size that
  captured, is the way this has actually gone wrong) is a sign the file did
  not come from the preview harness and should be regenerated or resized to
  match, not left as the odd one out.

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
console/           the multi game console: menu, dispatch, its own tests
console.yaml       which games are on the console, and in what order
STORAGE.md         the 16 MB flash: persistence, game library, larger games
CONSOLE.md         the console: one binary, every game in it, a menu
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
