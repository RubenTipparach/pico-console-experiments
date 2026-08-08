# Star Dancer's ships

Five hulls, all authored the same way, all facing **+Z** with **+Y** up and the
origin at the middle of the hull. Each is exactly **1.0 long in Z**, from
z = -0.5 to z = +0.5, and the renderer draws it at a scale taken from
`hull_length()` in `src/sim.cpp`. That is the only place a ship's real size is
written down: change 26 to 30 there and the frigate is bigger everywhere, in
the picture, in the collision and on the HUD, with nothing else to keep in
step.

## Winding

**List a face's corners clockwise as seen from outside the model.**

This is the opposite of the usual right hand rule convention, and it is not
optional: `tools/obj2cpp.py` takes the face normal as `(c - a) x (b - a)`
rather than `(b - a) x (c - a)`, because that is the winding the engine's
rasterizer counts as front facing. Get it backwards on a face and that face is
culled from the outside and drawn from the inside, so the ship renders with
holes in it that move as you fly around it.

Nothing catches that by eye at 120 pixels, so `tests/test_models.cpp` catches
it instead: it sums the signed volume of every mesh, which comes out positive
for a closed solid wound outward and negative for one wound inward.

## Blocks

Every hull is a union of hexahedral blocks (six sided solids, not necessarily
axis aligned), one `usemtl` each. A block is eight vertices in a fixed corner
order followed by the same six faces every time:

```
v  x0 y0 z0      # 1  low  x, low  y, low  z
v  x1 y0 z0      # 2  high x, low  y, low  z
v  x1 y1 z0      # 3  high x, high y, low  z
v  x0 y1 z0      # 4  low  x, high y, low  z
v  x0 y0 z1      # 5  low  x, low  y, high z
v  x1 y0 z1      # 6  high x, low  y, high z
v  x1 y1 z1      # 7  high x, high y, high z
v  x0 y1 z1      # 8  low  x, high y, high z
f -8 -7 -6 -5    # -z
f -4 -1 -2 -3    # +z
f -7 -3 -2 -6    # +x
f -8 -5 -1 -4    # -x
f -5 -6 -2 -1    # +y
f -8 -4 -3 -7    # -y
```

The corners can be moved anywhere, which is what makes a block a tapered nose
or a swept wing rather than only a box. What must not change is which corner is
which: the eight are in the order above, and the faces are written with
negative (relative) indices so a block can be pasted in without renumbering
anything.

**A mirrored block inverts its winding.** A left wing is not a right wing with
the sign of x flipped: flipping one axis turns a solid inside out. Author the
left wing with its TIP as the low-x corner and its ROOT as the high-x corner,
so the block is still ordered the standard way round. The volume test is what
notices when it is not.

## Hardpoints

The seats in `k_gunship_subs` and `k_frigate_subs` (`src/sim.cpp`) are in
hundredths of a world unit, so a model coordinate is
`seat / 100 / hull_length`. They are meant to sit on the blocks that look like
what they are: the turret seats on the sponsons, the engine seat in the engine
block, the bridge on the bridge. `tests/test_models.cpp` checks each seat is
actually inside the mesh's bounding box, which is the cheap half of that.
