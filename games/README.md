# Adding a game

Create `games/<slug>/` with a `game.yml` and a `CMakeLists.txt`. Commit. CI
discovers it, builds it, publishes it at `/<slug>/`, and captures its first
thumbnail.

You do not edit the top level `CMakeLists.txt`, the workflow, or the gallery
template. If adding a game seems to require that, the discovery mechanism is
broken and that is the bug to fix.

```
games/<slug>/
  game.yml           gallery and build metadata (below)
  metadata.yml       32blit on-device metadata (title, icon, splash)
  CMakeLists.txt     one call to add_picosystem_game()
  src/               game sources
  assets/            images, referenced from assets.yml if you use the pipeline
  models/            .obj plus optional .mtl, converted at build time
  thumbnail.png      the gallery card and the launcher icon, both from here
```

## game.yml

| Field | Required | Meaning |
| --- | --- | --- |
| `slug` | no | URL path and cache key. Defaults to the directory name. |
| `title` | yes | Shown on the gallery card. |
| `blurb` | yes | One line. One. The card has room for one. |
| `target` | no | CMake target name. Defaults to the slug. |
| `sdk` | no | `32blit` (default). See the note below. |
| `web` | no | Build for the browser. Defaults to true for the 32blit SDK. |
| `controls` | no | Short strings shown on the card, like `"A: jump"`. |
| `models` | no | Documentation only. The real list lives in `CMakeLists.txt`. |

Whether a game builds is decided in [`build.yaml`](../build.yaml) at the repo
root, not in `game.yml`: list the slug under `build:` to keep it in the
rotation, under `hold:` to take it out, or leave it out entirely and it builds
by default.

## On the SDK field

Everything here builds against the 32blit SDK, which targets the PicoSystem via
`-DPICO_BOARD=pimoroni_picosystem` and also builds for desktop and for the
browser. That last part is the reason for the choice.

The raw Pimoroni picosystem SDK is device only. It has no SDL target and no
Emscripten target, its one SDL wrapper pull request was closed unmerged in 2021
and the branch deleted. A game written against it can ship a `.uf2` and can
never have a playable page in the gallery. `sdk: picosystem` is therefore
rejected by `tools/build_plan.py` rather than silently producing a game with a
dead URL.

## Thumbnails

A game has one picture, `thumbnail.png`, and it is used twice: the gallery
shows it on the card, and `tools/game_meta.py` resamples it to the 48x48 icon
compiled into the `.uf2` for the on-device launcher. Committing it is what
gives a game a face in the menu, because a device never sees the site.

CI captures one from the web build the first time a game is published and then
never touches it again. Changing a game does not refresh its thumbnail. A
committed PNG wins over a captured one, always.

The better shot comes from the game's own preview harness, which renders real
frames through the real engine at the console's native 120x120:

```
cmake -S . -B build.test -DBUILD_ENGINE_TESTS=ON
cmake --build build.test --parallel --target dustrider_preview
./build.test/games/dustrider/tests/dustrider_preview /tmp/dr
python3 tools/make_thumbnail.py --ppm /tmp/dr/preview_4_bend.ppm \
    --out games/dustrider/thumbnail.png
```

`make_thumbnail.py` doubles it to 240x240 with no filtering, which is what the
PicoSystem does to reach its panel. Pick the frame that says what the game is;
they are named for what they show.

Failing all that, run the `Capture thumbnails` workflow and name the game.
