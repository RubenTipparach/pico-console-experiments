# Twin Flare, a podracer demake mockup

Design mockup for a PicoSystem podracer: two engines out in front, a cockpit
trailing on cables, and a hover field that will not let either of them touch
the ground. Nothing is built yet. This exists so the flight model can be flown
and argued about before anyone writes C++, which is CLAUDE.md rule 10.

Open `index.html` in a browser. It is a single self contained file, no build
step and no network.

## What it is

A flyable bench, rendered live rather than drawn. The page carries a small
software rasterizer in JavaScript that mirrors `pse::Rasterizer` and
`pse::Renderer3D`:

- 120 x 120 framebuffer with a **one byte** z buffer, the engine's lores render
  size and the engine's depth precision.
- Flat shaded triangles, no textures, backfaces culled in world space.
- Every finished frame squeezed to RGB565, which is what the panel can show.
- HUD text drawn with the real glyphs from `engine/font/console5x7.txt`, so a
  string that would not fit on the device does not fit here either.
- A fixed 100 Hz simulation step, which is Star Dancer's tick and what the
  device would run.

Every picture below the bench is rendered the same way: the six pods, the four
track maps and all six screens are the game with the simulation held still, not
illustrations of it.

## The brief it is answering

Six pods, four tracks (desert, water, barren moon, ice), per engine damage,
repair on a fixed rate, a double tap boost that cooks the engines when the heat
runs away, and ramps, gaps and shortcuts that are all passable. Controls as
asked: B throttle, Y air brake, X repair, A pause.

## The question it is asking

**The brief puts the pause menu and the double tap boost on the same button.**
Those cannot both be true: a double tap ends with a finger resting on the
button, so a pause menu on A opens on the second tap of every boost. Both
readings are built and the page switches between them while flying:

- **Double tap B.** Boost on the throttle, A left alone as pause.
- **Double tap A**, with pause moved to holding A for a third of a second. The
  brief as literally written.

The recommendation is B, and the research is the reason rather than the button
conflict: in Episode I Racer the Thrust Meter is a readout of your current
**speed**, not a bar you charge, and at the top of it you arm the boost by
briefly releasing the accelerator and pressing it again. The resource you spend
is heat. Double tapping the throttle is not a workaround, it is the mechanic,
and the bench gates the boost on speed for the same reason.

## The flight model is one rule, seven times

- **The hover field is a spring that only ever pushes up.** Not a floor and not
  a constraint. Two things fall out of that one line and neither is written
  separately: the pod breathes over a crest and slams into a dip, and above the
  rest height there is no field at all, which is why it can fly.
- **A gap is an absence.** Nodes flagged as a gap carry no surface, so there is
  nothing to push against. No trigger volume, no jump code.
- **Glide is lift and lift costs speed.** Capped below the local gravity, so it
  always extends a jump and never cancels it.
- **The heading swings first, the velocity catches up.** That gap is the drift
  and it is one constant. HOARFROST is that constant halved.
- **Thrust is per engine at its own offset**, scaled by that engine's health, so
  a damaged pod pulls toward its bad side before the engine dies and permanently
  after. Nothing enforces "one engine will inevitably crash": it falls out of
  the torque.
- **The cockpit is a second mass, not a lean.** It lags behind the engines on
  its cables as a real degree of freedom, and its momentum feeds back into the
  yaw, so a pod flung wide keeps going wide after the stick has centred. This is
  the signature of the vehicle and a rigid body with a visual tilt cannot fake
  it.
- **The camera is a character.** Eased rather than welded, carrying a third of
  the pod's bank, pulling back on boost.

The four tracks are a ring of nodes and **four numbers**: gravity, grip,
cooling and drag. That is what makes them four games rather than four palettes.

## What building the bench found

Six of these are the engine's constraints rather than anyone's preference, and
the bench fails in the same places the device would because it implements all
six.

- **There is no clipping anywhere.** A triangle with one corner past the near
  plane is dropped whole, so the road is short strips anchored to the camera.
  Dust Rider's `k_near_ground` comment already says the desert vanished.
- **The near plane is not `z_near`.** `project()` rejects at NDC z 0, which is
  the harmonic mean of the two planes: `set_depth_range(0.25, 400)` really clips
  at 0.50. The bench uses 2 and 170, so the real clip is 3.95.
- **One byte of depth is 0.66 units a step in that bracket.** The road edge
  stripes are coplanar strips cut across the road, not stripes floated on top of
  it: a stripe 0.02 proud is the same depth value as the tarmac, ties go to
  whoever drew first, and it never appears at all.
- **Projection precision decays with absolute world coordinates**, 2 pixels of
  error at 500 units. A lap here is 2,200 units around, so every draw is handed
  camera relative coordinates. Absolute ones would be right at the start line
  and visibly wrong down the back straight.
- **`ScreenTriangle` stores screen x and y as `int16` with no clamp**, so a wide
  ground quad near the near plane can project past 32767 and draw itself on the
  other side of the screen.
- **`run_split` splits at exactly half the height**, and a road game puts nearly
  all its fill in the bottom half, so budget on the split buying much less than
  2x.

And four bugs the bench caught that prose would not have:

- **731 triangles into a queue that holds 640**, with the pack inside forty
  units, which is exactly the moment a race is won. The engine mesh had four
  rings where three give the same silhouette (48 triangles to 36), far road
  segments were drawing the horizon a second time, and rivals needed a **rank**
  cap on top of the distance test so only the nearest is ever drawn in full. It
  is 408 now.
- **The road was invisible.** Road at 198,170,124 against desert at 206,178,128
  is the same colour at four bits a channel. The bench rendered a beautiful dune
  sea with no track in it.
- **The tracks were not circuits.** The turns in each plan summed to about 50
  degrees, so the tail bend that closes the ring dragged the last eighth of the
  road back across half the map. Every minimap came out a long thin hairpin. A
  plan is a circuit when it turns all the way round.
- **On the moon, lift exceeded gravity.** `LIFT_MAX` scaled by the track's thin
  air came to 11.9 against 8.8 of gravity, so nose-up on ASHFALL climbed
  forever and the measured jump reach was 839 units on a lap 2,000 units long.
  The comment above it had always claimed the glide could never cancel a jump.

## Passability, measured rather than asserted

The brief asks that the ramps, gaps and shortcuts all be passable. Each track
card carries the numbers instead of the claim: **worst case jump** flies the
slowest pod on the roster off the end of the road with no nose-up at all, no
glide and no skill. It clears the widest gap on every track by three times or
better. The margin is deliberately generous, because a gap that needs a perfect
entry ends runs rather than rewarding them.

## The keys

The face buttons sit where they physically sit on the PicoSystem, whose diamond
is **X top, Y left, A right, B bottom**, taken from `web/shell.html`, the one
place in the repo that says where those buttons are.

| key | button | does |
| --- | --- | --- |
| S or X | B | throttle, and double tap to boost |
| V or A | Y | air brake |
| W or C | X | repair |
| D or Z | A | pause |
| arrows | dpad | steer, and pitch the nose |

`z x c v` work because those are what the SDL build reads natively and what the
gallery shell dispatches underneath.

## What it does not answer

- Sound. Nothing here makes a noise, and an engine note that rises with the
  heat is probably worth more than any of the geometry above.
- Frame time. Triangle counts are the only performance number anybody in this
  repo can produce without a device, and `config.hpp` says so itself. 408 of
  640 proves the design will not drop geometry. It does not prove 50 frames a
  second on a 133 MHz M0+ with no FPU, and the first build's real job is to
  find that out.
- Rivals that actually race. They pace the centreline rather than flying the
  flight model, on the grounds that a rival which can crash can be lapped by
  its own physics and none of that is visible from behind you.
