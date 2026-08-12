# Cat Coin Pusher, a cart demake mockup

Design mockup for moving `cat_coin_pusher.p8` off PICO-8's 128x128 at 60 Hz and
onto the PicoSystem's 240x240 at 100 Hz. Nothing is built yet. This exists so
the layout and the feel can be argued about before anyone writes C++, which is
CLAUDE.md rule 10.

Open `index.html` in a browser. It is a single self contained file, no build
step and no network.

## What it is

A playable bench, rendered the way the device will render it rather than the
way a browser would like to:

- 240x240 framebuffer, native, built in a `Uint32Array` and pushed once per
  frame.
- A drawing API with nothing in it the RP2040 will not have: `pset`, `hline`,
  `vline`, `rect`, `rectfill`, `line`, `circ`, `circfill`, sprites and text. No
  `ctx.arc`, no `fillText`, no gradients, no alpha.
- Every finished frame squeezed to RGB565, which is what the panel is handed.
- Text in two real fonts: 32blit's `minimal_font` metrics (a 3x5 glyph in a
  4x6 cell) for the panel, and the engine's 5x7 for headings, which is what
  `pse::draw_text` gives the real build. Everything is placed from `measure()`,
  never by eye, which is rule 9.
- A fixed 100 Hz simulation accumulator, because one tick is 10 ms and that is
  what the SDK's `update()` runs at on this hardware.

All of the cart comes across: eleven specials with their radii, impulses and
two second fuses, the combo spinner, the shop with its refresh, ten rounds, and
a target of `150 * 1.5^(round-1)`.

## The brief it is answering

Convert the cart and run it at 240x240. Every tuning constant goes through one
table rather than being retuned by feel:

    velocity per tick    = pico per frame  * 1.875 * (60/100)   = x 1.125
    acceleration         = pico per frame2 * 1.875 * (60/100)^2 = x 0.675
    a count of frames    = frames * 100/60                      = x 1.667
    a damping factor d   = d ^ (60/100)

That last one is the one worth stating out loud: damping compounds, so the
cart's 0.92 per frame at 60 Hz is 0.9509 per tick at 100 Hz, not 0.92.

## The questions it is asking

**1. The row that replaced the button prompts.** The cart's bottom panel was
three button prompts: `"⬇:buy 5 coins g5"`, `"end round❎"`, and
`"🅰❎ to start"` on the title. Rule 9 forbids naming a button on screen, so
those cannot come across as written. Selection carries the meaning instead: the
five bag slots, a BUY chip and an END ROUND chip are all slots on one row, left
and right move along it, one button uses what is selected, and a line
underneath says what that would do in words about the game rather than words
about the controller. Dropping a coin is the verb of the game and keeps a
button of its own. This is the biggest single departure from the cart's screen
and the thing most worth ruling on.

**2. The field is measured in coins, not in pixels, and this page had to learn
that twice.** A coin pusher's geometry is not really in pixels: what decides
whether a shove reaches the lip is how many coins deep the shelf is. The cart
in its own coins (5 px across) was 21.6 x 8.8 coins of field, a 2 coin plate,
3.2 coins of travel, and a 4.4 coin free shelf.

The first version of this page scaled the field by pixels instead and gave it
208 x 112, which is 20.8 x 11.2 coins: two and a half coins deeper than the
cart. It looked better and it was unplayable. Sixty seconds of simulation at
round 1, with every coin the round hands out, scored **zero**, because the pile
never got deep enough to reach the lip. Scaling in coins instead gives a 208 x
88 field, and the 24 px that frees goes to the payout tray and the panel, which
wanted it anyway. Measured after the change, with a bot that drops a coin every
quarter second:

| round | coins on the shelf | seconds to target |
| --- | --- | --- |
| 1 | 50 | 8.7 |
| 4 | 59 | 8.3 |
| 8 | 70 | 12.1 |
| 10 | 76 | 23.8 |

**3. The cart's seed count was a ceiling it never reached.** It asked for
`100 + 10 * round` coins and placed them by rejection sampling into a shelf of
1836 px2, which is over 100 percent coverage before the round number is even
applied. So the sampler always saturated, the round scaling barely moved the
result, and the real seed count was "as many as random packing fits". Copying
the arithmetic literally put 65 coins on the table when the comment said 135.
The sites are a jittered hexagonal lattice taken in random order now: the same
loose carpet, but the number asked for is the number placed, and the page can
report a real figure. The fill goes from two thirds of the shelf at round 1 to
a full shelf at round 10.

**4. Is a coin readable enough now?** At r 2.5 several of the cart's eleven
specials were the same three pixels in a different colour. At r 5 each one
carries a 5x5 face. The coin is one baked 11x11 sprite rather than a
`circfill` per coin, both because 240 rasterised circles read as a flowerbed
and because a const sprite blitted 240 times is what the device would actually
do.

## How it maps to the engine

Everything here is 2D, so nothing touches `pse::Renderer3D`, the rasterizer or
the depth buffer, and rule 7's interface segregation means it does not link
them either. What it wants from the engine is the 2D primitive set this page is
written against, drawn into a `pse::RenderTarget`:

- `pse::draw_text`, `pse::fill_rect` and `pse::plot_pixel` exist already in
  `engine/include/pse/text.hpp`.
- `line`, `circ`, `circfill`, `hline`, `vline` and a sprite blit do not. They
  belong in the engine next to the others rather than in the game: they are not
  game rules, and the lander demake wants the same ones.

Drawing the panel through a `RenderTarget` rather than through `screen.text`
also means the preview harness can see it, so the thumbnail and any layout test
show the real screen. Today's games draw their HUD with SDK calls that do not
exist in a host build, which is why CLAUDE.md warns that a HUD change is
unverified until the game has been run.

## What it costs the device

| | |
| --- | --- |
| framebuffer, 240x240 at 16 bits | 115 KB |
| coins, 340 slots of 20 bytes | 6.8 KB |
| collision grid, head per cell plus next per coin, `int16_t` | 1.4 KB |
| particles, popups and payout coins, fixed arrays | 2.2 KB |
| pusher, dispenser, bag, shop, spinner | under 200 B |

About 10.6 KB on top of the framebuffer, so roughly 126 KB of 264 KB. The 340
coin slots are sized for the clone special, which adds five at a time, rather
than for the 76 a round seeds.

For frame time, the page measures rather than guesses: a full round 10 table
runs **240 to 330 pair tests per tick**, and the great majority are rejected on
the squared distance before any square root, which is what the cart already
did. The grid is one cell per coin diameter with same cell plus four forward
neighbours, so no pair is tested twice and none is missed. That is a
comfortable number for a 250 MHz Cortex-M0+ at 100 ticks a second, which is why
the simulation stays at 100 Hz rather than dropping to every other tick.

## Running it

Open the file. Click the screen so the canvas has focus, then Z to drop a coin,
the arrow keys to move along the row, and X to use what the row has selected.
Any key starts the game and continues on the title, prize and end screens,
which is rule 9: nothing on a game screen names a button. R reseeds, and is a
bench affordance rather than a game control.
