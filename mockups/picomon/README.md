# Picomon mockup

Design mockup for the Picomon idea in `ideas.md`: a creature catching adventure
inspired by the DS Pokemon games. Nothing is built yet. This exists so the
design can be argued about before anyone writes C++.

Open `index.html` in a browser. It is a single self contained file, no build
step and no network.

## What it is

Five screens, rendered live rather than drawn: Route 1, Battle, Catch, Bag,
Party. The page carries a small software rasterizer in JavaScript that mirrors
`pse::Rasterizer` and `pse::Renderer3D`:

- 120 x 120 framebuffer with a z buffer, the engine's lores render size.
- Flat shaded triangles, no textures, backfaces culled in world space.
- Every colour snapped to four bits per channel, which is what the PicoSystem
  panel can show. The page chrome uses the same palette.
- Text in a 3 x 5 font on a 4 pixel advance, `minimal_font`'s metric, so what
  fits on the page is what fits on the device.
- Near plane clipping on every triangle, because a ground quad with a corner
  behind the camera has to be cut, not dropped.

The triangle counter under the screen is real. It counts what reached the
rasterizer that frame, against the engine's 640 triangle `FrameQueue`.

## The camera

The first version guessed the angle and it read as top down. It was measured
out of the game instead, three ways, and they agree. The page carries the full
derivation; the short version:

| Parameter    | Value      | Source |
|--------------|-----------:|--------|
| Pitch        | 31 deg     | measured, below horizontal |
| Yaw          | 0 deg      | measured, locked to the tile grid |
| Vertical FOV | 16.18 deg  | Gen IV decomp, confirmed by the horizon |
| Projection   | perspective| the road edges converge |
| Camera height| 21.7 tiles | follows from the lens |
| Camera setback| 36.2 tiles| follows from the lens |

The pitch was never the main problem. The **lens** was. A 60 degree lens at a
shallow pitch still looks near vertical at the bottom of the frame, because the
bottom of that lens is looking 30 degrees further down than the middle. Black
and White uses a very long lens, so every row of the screen sees the ground at
nearly the same angle.

How it was measured:

1. **The horizon.** In a frame of the Nacrene City museum plaza, the paved
   road's two edges run along the world depth axis. Line fits give 0.62 and
   0.72 px RMS, and they intersect at `x = 128.26, y = -314.9` on a 256 x 192
   frame. The x lands on the frame centre (128.0), which proves the yaw is
   locked to the tile grid. The y is the horizon, giving
   `f * tan(pitch) = 410.9 px`.
2. **The lens.** pret's `pokeplatinum` decompilation carries Gen IV's field
   camera table in degrees, and DSPRE reads the same table out of the ROM and
   agrees byte for byte. Stored vertical half angle 8.0914306640625 deg, so
   16.18 deg full. Gen V's camera config is a NARC whose format nobody has
   published, but Gen V inherited the field engine. Combining with step 1:
   pitch = atan(410.9 / 675.3) = **31.32 deg**.
3. **An independent check.** Bulbapedia's top down map render of the Route 3
   Day Care is itself a tilted orthographic view: two repeating decorations in
   it repeat 0.80 as far along depth as across width, putting its tilt at 53.5
   deg. The Day Care building appears in both that map and the game frame, and
   its wall height to width ratio is 1.43x larger in the game frame. That ratio
   is exactly `cos(pitch_game) / cos(pitch_map)`, giving **31.75 deg**.

Worth noting: Platinum's documented camera sits 571.97 units above its target
and 342.98 behind it, which is 59.05 deg. Black and White's measured 31.3 is
the complement to within measurement error. Gen V appears to have kept Gen IV's
projection exactly and swung the camera down to the other side of the diagonal.

Black and White shows 15.8 tiles across a 256 pixel screen. This screen is 120
wide, and holding a character sprite at about one tile puts 12 across instead.
That is the one number chosen rather than measured.

The battle scene deliberately uses a wider 30 degree lens from lower down,
because a battle is a different shot. It is the one place the page leaves the
measured numbers, and it says so.

## Character art

Authored in `art/build_art.py` as character-per-pixel strings, so a change to a
sleeve shows up in a diff rather than as a binary blob nobody can review. Run
it to rebuild everything:

    cd art && python3 build_art.py

For each of the four sheets it writes an editable `.aseprite` (three layers:
fill, shade, outline, plus one animation tag per direction), a `.png` frame
strip, and `sheets.json` / `sheets.js`. The page embeds the strips as data URIs
so it stays a single file.

| Sheet    | Frames | Tags            | PNG   |
|----------|-------:|-----------------|------:|
| hero     | 12     | down, up, side  | 456 B |
| trainer  | 8      | down, up        | 379 B |
| villager | 4      | down            | 306 B |
| healer   | 4      | down            | 330 B |

Aseprite is a paid GUI application and could not be installed here, so
`art/aseprite.py` writes the format directly: a small encoder for the
documented v1.3 spec (RGBA cels, zlib compressed, named layers, tags). Every
file is parsed back and compared against its own PNG before it ships.

Two constraints shaped the art rather than decorating it:

- **Every colour is a multiple of 0x11**, so it survives four bits per channel
  unchanged. A colour that does not is one that shifts on the device and
  nowhere else.
- **Frames are 12 x 20** because the camera puts a tile at 10 pixels, so a
  character is exactly two tiles and draws at 1.0 scale with no resampling.
  Change the tiles across the screen and the art is reauthored, not rescaled.

Walk cycles are four beats (step, pass, step, pass). The profile is drawn
facing east and mirrored for west, so the hero needs three directions of art
rather than four. A sheet only carries the poses its character needs: the
villagers never turn, the trainer has to be seen from behind because it owns a
line of sight.

Characters sit on a darkened ellipse of ground rather than a drawn shadow
sprite. It costs no geometry and no art, and it is the one thing that stops a
billboard looking like a sticker on the screen.

## Measured

| Scene  | Peak triangles | Ground quads | Sprites |
|--------|---------------:|-------------:|--------:|
| Route  | 448            | 118          | 37      |
| Battle | 278            | 3            | 0       |
| Catch  | 275            | 3            | 0       |
| Bag    | 0              | 0            | 0       |
| Party  | 0              | 0            | 0       |

The route's ground would be 465 quads and 930 triangles drawn per tile. Merging
runs of the same material within a row, and deepening rows with distance (1
tile, then 2 past six, then 4 past fourteen), takes it to about 118 quads.
Trees are the other half of the bill: under a long lens a far tree barely
shrinks, so past four tiles it drops to a single four sided cone, 4 triangles
instead of 20.

Route 1 is the scene that will fight for the queue. The two levers are the
depth of the tile window and how early a tree drops to its cone.

## The calls this mockup is making

1. Fixed camera, yaw locked to the grid. Cheap culling, pre sorted props.
2. Creatures are low poly meshes in battle, sprites in the overworld.
3. Six types in a ring, so the chart never has to be shown.
4. Integer damage and an integer catch roll, with the fourth root replaced by a
   64 byte lookup table generated at build time.
5. Menus are 2D only and cost nothing.

Estimated cost, stated per project rule 8: about 33 KB of SRAM (roughly 30 KB
of which is the depth buffer and frame queue every 3D game here already pays)
and about 68 KB of flash. Save state is 640 bytes.

One open device risk: a long lens flattens the depth range, so with the camera
42 tiles from the player the projected depth values sit in a narrow band. That
needs checking against the rasterizer's fixed point depth resolution before the
z buffer can be trusted to sort two creatures standing near each other.

## Open questions

Listed at the bottom of the page. The big one is meshes against sprites for
creatures in battle.
