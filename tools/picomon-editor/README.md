# Picomon editor

A one window Windows utility for the Picomon maps: paint the tile grid, place
the NPCs, warps and events on it, fill in the encounter tables, and save a
`.zone` file the build will accept.

Quick and dirty on purpose, the same way the flasher is: single form, no
installer, no undo. It is a tool, not a product.

## Building it locally

```
cd tools/picomon-editor
dotnet publish PicomonEditor.csproj -c Release -r win-x64 --self-contained true -o publish
```

The result is a single self contained `PicomonEditor.exe`, so it can be handed
to someone who has no .NET installed.

## Using it

Run it from anywhere inside a checkout and it finds `games/picomon/data` by
walking up from the executable. Otherwise press **Folder...** and point it at
that directory. It remembers the last one, and which zone was open.

It opens the whole directory, not one file: the tileset, the species list, the
item list, `start.txt`, and every zone. That is what lets it tell you a warp
lands in a tree two zones away, or that the tile you just painted is the one the
player starts on.

- **The palette** down the left is `tileset.txt`, with each character's colour,
  name and flags. It is read from the file, so a tile added to the tileset
  appears here on the next **Reload** with no change to this app.
- **Left click** paints, **right click** picks up the tile under the cursor.
  **Fill** floods the character you clicked, **Rect** fills a dragged box.
- **Markers** are the placed things: blue for NPCs, amber for events, green for
  warps. Click one to select it, drag it to move it. A trainer's line of sight
  is drawn on the ground, the same way the game draws it, so you can see what a
  `sight 4` actually covers.
- **The panel on the right** has the fields for whatever is selected, and they
  are the fields for its kind: an NPC has a party and a sheet, a warp has a
  destination and an **Open the other side** button, an event has only the
  arguments its own kind takes. The **Encounters** tab is one table per
  encountering tile, and shows what each weight works out as as a percentage,
  which is the only way to see that a weight of 40 next to a weight of 400 may
  as well not be there.
- **The list along the bottom** is everything wrong with the data. Click a row
  to jump to it: it opens the zone, selects the thing, and scrolls to the tile.

`Ctrl+S` saves. `Delete` removes the selected marker. `Escape` deselects.

## Why it refuses to save

The checks in the list are the checks `tools/picomon_data.py` makes at build
time, and they run on every edit. **Save refuses while the open zone has an
error in it**, because a `.zone` that fails the build is the one thing this app
exists to prevent: catching it here is a five second fix, and catching it in CI
is a red run and a round trip through a runner.

Errors in *other* zones are listed but do not block the save. A broken warp in
another file is real, and refusing to save the map in front of you because of
it would make the tool useless in exactly the situation it is for.

Warnings never block anything. They are the things the compiler allows and
nobody meant: an NPC standing in a wall, a `#` in a line of dialogue (the
compiler reads it as a comment and cuts the page off there), a second encounter
table for a character the game already rolls the first table for.

## Why the diffs stay small

A `.zone` file is text so that it can be reviewed as a diff. An editor that
rewrites the file every time it saves takes that away, so saving a file this
app just opened produces the same bytes, down to the line endings.

Records are written in the order the existing zones already use, and everything
the format allows but the fields do not cover is carried through rather than
dropped: blank lines, comments on their own line, and comments hung off the end
of a line all come back where they were. The one thing that is corrected on
save is a `size` line that disagrees with its own tiles block, which is a file
the build rejects anyway, and the problem list says so before you save it.

## What it deliberately does not do

- **No undo.** A wrong fill is fixed by filling it back, or by **Reload**,
  which throws away everything since the last save.
- **No resizing a zone, and no making one.** Cropping a map is destructive and
  there is no undo to catch it, so the grid is whatever the file says. A new
  zone starts as a copy of an existing file.
- **Only zones.** A `.zone` file is the only thing it writes. `tileset.txt`,
  `species.txt`, `items.txt` and `start.txt` are read so it can check against
  them; `moves.txt` it never opens, because nothing in a zone names a move.
  They are short, hand written, and a form over them would be slower than the
  text.
- **No preview.** It draws tiles as flat colours from the tileset, not the
  game's own art, and it is not a simulator: there is no way to walk around in
  it, open a menu, or see a battle.
- **Nothing about the device.** Building and flashing are somebody else's job
  (`tools/flasher`).
