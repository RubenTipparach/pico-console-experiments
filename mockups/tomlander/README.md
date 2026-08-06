# Tom Lander demake mockup

Design mockup for a PicoSystem demake of Tom Lander, the LOVE2D game in the
sibling `tom-lander` repo. Nothing is built yet. This exists so the control
scheme can be flown and argued about before anyone writes C++, which is
CLAUDE.md rule 10.

Open `index.html` in a browser. It is a single self contained file, no build
step and no network.

## What it is

A flyable bench, rendered live rather than drawn. The page carries a small
software rasterizer in JavaScript that mirrors `pse::Rasterizer` and
`pse::Renderer3D`:

- 120 x 120 framebuffer with a **one byte** z buffer, the engine's lores render
  size and the engine's depth precision. The depth range is bracketed to the
  scene for the reason `Renderer3D::set_depth_range` gives.
- Flat shaded triangles, no textures, backfaces culled in world space.
- Every finished frame squeezed to RGB565, which is what the panel can show.
- HUD text drawn with the real glyphs from `engine/font/console5x7.txt`, so a
  string that would not fit on the device does not fit here either.
- A fixed 60 Hz simulation step, which is what the device runs.

The ship is `models/tom.obj`, a picoCAD model of the `cross_lander` supplied by
the user. It renders here exactly as it would on hardware.

## The question it is asking

"ABXY rotates each thruster" reads more than one way, so the bench implements
three answers and lets you switch between them while flying:

- **Direct.** Hold a button, that pod burns. Four rotors, four buttons.
- **Gimbal.** Every pod idles at hover. Holding a button rotates that nozzle
  out and opens it up, vectoring the thrust.
- **Paired.** Buttons work opposite pairs, so Y and A roll and X and B pitch.

On top of that, a toggle for whether a button follows the hull or the screen,
which matters because left and right orbit the camera.

Left and right rotate the camera, up opens the pause menu, and down levels the
hull and fires all four. The camera sits 45 degrees above the horizontal.

## The keys

WASD sits on the face buttons in the positions they physically occupy on the
PicoSystem, whose diamond is **X top, Y left, A right, B bottom**. That is not
the Xbox arrangement, which is its mirror, and the layout here is taken from
`web/shell.html`, the one place in the repo that says where those buttons sit.

| key | button | cross_lander pod |
| --- | --- | --- |
| W | X, top | Front, lifts the nose |
| A | Y, left | Right, lifts the right side so the ship goes left |
| S | B, bottom | Back, lifts the tail |
| D | A, right | Left, lifts the left side so the ship goes right |

The side pods read backwards on purpose, and tom-lander does the same: the
button on the left of the diamond takes you left, which is what a player means
by it, even though the pod that fires is the right hand one.

`z x c v` still work, because those are what the SDL build reads natively and
what the gallery shell dispatches underneath.

This is the same mapping the gallery shell now applies to every game, so these
are the real keys rather than a convenience for this page.

## The flight model is not invented

Gravity, thrust, torque, damping, fuel and the hard landing threshold are
tom-lander's own constants, taken out of their per tick form and scaled by 3.2
because this ship is that much bigger in world units. The ratios are untouched,
so one pod gives 0.70 thrust to weight and all four give 2.81: a single pod
cannot hold the ship up, which is true in the original too.

| tom-lander | value | here |
| --- | --- | --- |
| `VTOL_GRAVITY` | -0.005 per tick | 57 u/s^2 |
| `VTOL_THRUST` | 0.0035 per tick | 40 u/s^2 per pod |
| `VTOL_TORQUE_PITCH` / `_ROLL` | 0.002 at arm 0.9 | 3.2 rad/s^2, see below |
| `VTOL_DAMPING` / `_ANGULAR_` | 0.95 per tick | unchanged |
| `MOON_FUEL_DRAIN` | 13 per second | all four pods at full |
| `SHIP_HARD_LANDING_THRESHOLD` | 0.05 per tick | 9.5 u/s |

One constant did not survive. The original's torque settles at 118 degrees of
roll per second off a single pod, which is fine with WASD, an analog pad and a
free camera, and a coin flip on four digital buttons at 120 pixels. The bench
runs at half. That is a decision for the user, not a silent tuning change, so
it is called out on the page and in `TUNE` in the source.

Two other places the bench deliberately differs from the original, both flagged
on the page: firing the front pod lifts the front, so the ship drifts backward,
and a landing steeper than 20 degrees is a crash. tom-lander has no tilt gate
at all until the hull passes 90 degrees.

## What it found

- The model is five distinct solids instanced seventeen times, perfectly
  symmetric. 144 vertices, 110 faces, 204 triangles, 3,324 bytes of flash
  through `obj2cpp.py`.
- The frame queue holds 640 triangles. The ship spends 204 of them, so the
  terrain has to be culled to a radius rather than drawn whole.
- **picoCAD winds its faces the opposite way to every model in this repo.** All
  24 existing models have negative signed volume, which is what
  `obj2cpp.face_normal` compensates for by taking `v x u`. All 17 solids in
  tom.obj are positive. Imported unchanged, 0 of 204 normals point outward and
  the ship renders as a flat silhouette: it compiles, it runs, and it looks
  like a modelling mistake. `tools/pack_picocad_texture.py` measures this and
  flips it.
- The model reads 17 rectangles of its 128 x 128 sheet and nothing else, 1,728
  texels or 10.5 percent. Those repack to 48 x 40 at 90 percent efficiency.
  Thirteen are exactly 8 x 8, one picoCAD colour cell, and every rectangle
  belongs to exactly one part, so baking them to flat face colours loses
  nothing and costs no flash at all.

## Building the game would need

`Renderer3D::draw_mesh` takes yaw and pitch. A lander that tilts on two axes at
once needs roll as well. That is a generic addition to a generic mesh drawer
rather than a game specific special case, so it sits inside rule 7, and it
costs one more rotation in the per frame matrix build.

## Regenerating

The page is assembled from a template plus the repo's own data, so nothing in
it is a second copy of something:

- the mesh comes from `models/tom.obj`
- the HUD font comes from `engine/font/console5x7.txt`
- the palette is the Picotron 32 from `tom-lander/src/game/palette.lua`
- the two typefaces are tom-lander's own, from `assets/fonts/`
