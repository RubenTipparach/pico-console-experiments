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
- Every colour snapped to four bits per channel on the way in, which is what
  the PicoSystem panel can show. The page chrome uses the same palette.
- Text in a 3 x 5 font on a 4 pixel advance, `minimal_font`'s metric, so what
  fits on the page is what fits on the device.
- Near plane clipping on every triangle, because a ground quad with a corner
  behind the camera has to be cut, not dropped.

The triangle counter under the screen is real. It counts what reached the
rasterizer that frame, against the engine's 640 triangle `FrameQueue`.

## Measured

| Scene  | Peak triangles | Ground quads | Sprites |
|--------|---------------:|-------------:|--------:|
| Route  | 354            | 127          | 59      |
| Battle | 281            | 3            | 0       |
| Catch  | 280            | 3            | 0       |
| Bag    | 0              | 0            | 0       |
| Party  | 0              | 0            | 0       |

The route's ground would be 399 quads and 798 triangles drawn per tile. Merging
runs of the same material within a row, and drawing rows past seven tiles out
at two tiles deep, takes it to 127 quads. That is the single technique the
overworld's budget rests on.

## The calls this mockup is making

1. Fixed three quarter camera, no yaw. Cheap culling, pre sorted props.
2. Creatures are low poly meshes in battle, sprites in the overworld.
3. Six types in a ring, so the chart never has to be shown.
4. Integer damage and an integer catch roll, with the fourth root replaced by a
   64 byte lookup table generated at build time.
5. Menus are 2D only and cost nothing.

Estimated cost, stated per project rule 8: about 33 KB of SRAM (roughly 30 KB
of which is the depth buffer and frame queue every 3D game here already pays)
and about 68 KB of flash. Save state is 640 bytes.

## Open questions

Listed at the bottom of the page. The big one is meshes against sprites for
creatures in battle.
