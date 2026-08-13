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
| `objective` | yes for web | What the player is trying to do, a sentence or two. Panel one of the tutorial. |
| `controls` | yes for web | `"key: what it does"`, one a line. Shown on the card and paged into the tutorial. |
| `rules` | no | `"Heading: what it means"`, one a line, one tutorial panel each. For a game with a scoring system to explain. |
| `target` | no | CMake target name. Defaults to the slug. |
| `sdk` | no | `32blit` (default). See the note below. |
| `web` | no | Build for the browser. Defaults to true for the 32blit SDK. |
| `models` | no | Documentation only. The real list lives in `CMakeLists.txt`. |

Whether a game builds is decided in [`build.yaml`](../build.yaml) at the repo
root, not in `game.yml`: list the slug under `build:` to keep it in the
rotation, under `hold:` to take it out, or leave it out entirely and it builds
by default.

## What a game exports

One symbol, and it is not `main`. A game defines its three functions with
names of its own and exports them with the `PSE_GAME` macro:

```cpp
namespace {
void game_init() { ... }
void game_update(uint32_t time) { ... }
void game_render(uint32_t time) { ... }
}  // namespace

PSE_GAME(myslug, game_init, game_update, game_render);
```

The slug in the macro must match the directory's slug with any dashes turned
into underscores, because that is how the generated code spells the symbol.

Do **not** define the SDK's own `init`, `update` and `render`.
`add_picosystem_game()` generates a four line `standalone_main.cpp` per game
that writes those and forwards them to the exported symbol. Defining them
yourself is a duplicate symbol at the device link and nothing at all before
it, since the host tests never compile a game's SDK facing file.

That indirection is what makes the console possible: several games are linked
into one binary, so only one of them can own a global called `init`, and
everything else a game owns belongs in an anonymous namespace for the same
reason. `tools/tests/test_game_entry.py` walks every game and checks both
halves of this, because the last game to get it wrong took main red.

## The mini tutorial

Every game that ships to the web ships a tutorial on its own page: what the
game wants from the player, then its controls, as panels with arrows to page
through. It is built from `objective` and `controls` by
[`tools/gen_shell.py`](../tools/gen_shell.py) at configure time, so there is
nothing to write beyond those two fields and nothing to keep in sync.

Write `controls` as `"key: what it does"`. The key half is set in its own
column and the rest is the explanation, so `"A: jump"` reads as a control and
`"A"` alone reads as noise. A line with no colon is kept whole, for the things
a game needs to say that are not one button.

Name the console buttons: `A`, `B`, `X`, `Y`, `up`, `down`, `left`, `right`.
The keyboard key beside each one is added for you, read out of the on-screen
gamepad in `web/shell.html`, which is what those buttons actually dispatch. Do
not write `Z` in a control string: the console has no Z, and the mapping would
then have two homes to drift between.

A rule can carry a **picture**, at `games/<slug>/tutorial/<heading>.svg`, and
"Chips and mult" looks for `chips-and-mult.svg`. Adding one is adding a file:
nothing is listed anywhere, and the SVG is inlined into the panel rather than
linked, so it is already there when the page opens and it inherits the panel's
colour through `currentColor`. Some rules are shapes and sums, and a paragraph
is the worst way to hand somebody either. `test_gen_shell.py` fails on an SVG
that no rule heading claims, so renaming a heading breaks the build instead of
quietly dropping its diagram.

`rules` is optional and most games should not have it. Knowing what the buttons
do is enough to play a lander, and a panel per rule on a game that has none is
padding. It exists for a game with a SCORING SYSTEM: Joker Reels asks a player
to pick a payline, read a hand and weigh a multiplier against how hard a symbol
is to read, and none of that is discoverable from a control list. The
alternative was one enormous `objective`, which is a wall of text in the panel
a player meets first and is therefore a wall of text nobody reads. Each entry
is `"Heading: what it means"`, the same shape a control takes, and the panels
land between the objective and the controls.

`tools/tests/test_gen_shell.py` walks every game in the repo and fails if one
that ships to the web has no objective or no controls. A page a player cannot
understand is a broken page, and it should not get as far as being published.

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
