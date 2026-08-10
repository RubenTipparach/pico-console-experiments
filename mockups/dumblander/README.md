# Dumb Lander, a cart demake mockup

Design mockup for moving `dumb_lander.p8` off PICO-8's 128x128 at 30 Hz and
onto the PicoSystem's 240x240 at 100 Hz. Nothing is built yet. This exists so
the flight can be flown and argued about before anyone writes C++, which is
CLAUDE.md rule 10.

Open `index.html` in a browser. It is a single self contained file, no build
step and no network.

Not to be confused with `games/tomlander`, which is this repo's 3D lander with
four independently fired pods. That is a different game. This one is the 2D
cart: one engine, two side jets, and a pad on the far side of the valley.

## What it is

A flyable bench, rendered the way the device will render it rather than the way
a browser would like to:

- 240x240 framebuffer, native, built in a `Uint32Array` and pushed once per
  frame. The canvas is magnified by CSS, never sized responsively.
- A drawing API with nothing in it the RP2040 will not have: `pset`, `hline`,
  `vline`, `rect`, `rectfill`, `line`, `circ`, `circfill`, and text. No
  `ctx.arc`, no `fillText`, no gradients, no alpha.
- Every finished frame squeezed to RGB565, which is what the panel is handed.
- Text in two real fonts: 32blit's `minimal_font` metrics (a 3x5 glyph in a
  4x6 cell) for the HUD, and the engine's 5x7 for headings, which is what
  `pse::draw_text` gives the real build. Everything is placed from
  `measure()`, never by eye, which is rule 9.
- A fixed 100 Hz simulation accumulator, because one tick is 10 ms and that is
  what the SDK's `update()` runs at on this hardware.

The six screens below the bench are the same simulation with its state driven
there by an autopilot, not illustrations of it.

## The brief it is answering

Convert the cart and run it at 240x240. The flight model is the cart's, put
through one conversion table rather than retuned by feel:

    position or velocity per tick  = pico per frame  * 1.875 * (30/100)
    acceleration per tick squared  = pico per frame2 * 1.875 * (30/100)^2

1.875 is 240/128. 30 Hz is the cart's, because it defines `_update` and not
`_update60`. So gravity 0.1 becomes 0.016875, thrust -0.2 becomes -0.03375,
the sideways jet 0.05 becomes 0.0084375, and the 1.0 crash threshold becomes
0.5625 px per tick, which is 56 px per second.

## The questions it is asking

**1. Should a run be more than one landing?** The cart ended at the first
touchdown and offered a key to reset. The mockup makes a clean landing a leg:
the world regenerates, the gold pad moves further out and narrows by 2 px, the
tank tops up by 55, and the leg count goes up. A wreck ends the run. This is
the one change that turns the cart into something a gallery card can promise,
and it is the one most worth vetoing.

**2. The tank had to grow, and the page says by how much.** The cart's 100
units of fuel was 3.3 seconds of burn on a 128 px screen, which was almost
exactly one crossing. The crossing is 1.875x longer now. Flying 24 generated
legs under the page's own autopilot:

| tank | legs landed | fuel left on a clean leg |
| --- | --- | --- |
| the cart's rate, 0.3 per tick | 3 of 6 | 0 percent, every time |
| 0.3 / 1.875 = 0.16 per tick | 24 of 24 | 49 to 63 percent |

The number on the bar is still 100. What changed is how fast it drains, which
is the smallest way to say it. A leg takes 4.9 to 6.6 seconds.

**3. Is the pad edge a cliff, and is that alright?** Flattening a pad into the
terrain leaves a vertical step where the deck meets natural ground, in the
cart and here. It is visible on screen and you can fly around it, so this
mockup keeps it. Say the word and the pad can be blended over a few columns
instead.

## How it maps to the engine

Everything in this game is 2D, so nothing here touches `pse::Renderer3D`, the
depth buffer or the rasterizer, and rule 7's interface segregation means it
does not link them either. What it wants from the engine is the 2D primitive
set the mockup is written against, drawn into a `pse::RenderTarget`:

- `pse::draw_text`, `pse::fill_rect` and `pse::plot_pixel` exist already in
  `engine/include/pse/text.hpp`.
- `line`, `circ`, `circfill`, `hline` and `vline` do not. They belong in the
  engine next to the others rather than in the game, because they are not
  game rules: a second 2D game wants exactly the same five.

Drawing the HUD through a `RenderTarget` rather than through `screen.text`
also means the preview harness can see it, so the thumbnail and any layout
test show the real screen. Today's games draw their HUD with SDK calls that do
not exist in a host build, which is why CLAUDE.md warns that a HUD change is
unverified until the game has been run.

## What it costs the device

| | |
| --- | --- |
| framebuffer, 240x240 at 16 bits | 115 KB |
| terrain, 240 columns of Q8.8 | 480 B |
| rocks, 34 of 6 bytes | 204 B |
| stars, 60 of 3 bytes | 180 B |
| flame particles, 24 of 12 bytes | 288 B |
| wreck particles, 32 of 12 bytes | 384 B |
| the lander itself, 6 scalars in Q16.16 | 24 B |

About 1.6 KB on top of the framebuffer, against 264 KB of SRAM. There is no
pressure here. Flash is code and no baked assets: the terrain is generated, so
rule 11 does not apply, and the only art is the thumbnail.

## Running it

Open the file. Click the screen so the canvas has focus, then Z for the main
engine and the arrow keys for the side jets. Any key starts a run and any key
starts another after a wreck, which is rule 9: nothing on the game screen names
a button. R reseeds the world, and is a bench affordance rather than a game
control.
