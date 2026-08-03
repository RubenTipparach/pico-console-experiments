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
  thumbnail.png      written once by CI, then left alone forever
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
| `depends_on` | no | Paths that force a rebuild when they change. Defaults to `engine`, `cmake`, `tools/obj2cpp.py`. |

`depends_on` is the field that bites. A game is rebuilt only when its own
directory or one of these paths changes. If your game starts using a new shared
directory and you do not list it here, the game will quietly go stale: CI will
keep republishing the old binary because nothing it watches changed.

## On the SDK field

Everything here builds against the 32blit SDK, which targets the PicoSystem via
`-DPICO_BOARD=pimoroni_picosystem` and also builds for desktop and for the
browser. That last part is the reason for the choice.

The raw Pimoroni picosystem SDK is device only. It has no SDL target and no
Emscripten target, its one SDL wrapper pull request was closed unmerged in 2021
and the branch deleted. A game written against it can ship a `.uf2` and can
never have a playable page in the gallery. `sdk: picosystem` is therefore
rejected by `tools/fingerprint.py` rather than silently producing a game with a
dead URL.

## Thumbnails

CI captures `thumbnail.png` once, the first time a game is published, and then
never touches it again. Changing a game does not refresh its thumbnail.

To refresh one deliberately, run the `Capture thumbnails` workflow and name the
game, or just commit a new PNG.
