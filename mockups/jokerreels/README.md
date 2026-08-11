# Joker Reels, a 3D slot machine mockup

Design mockup for a 3D slot machine with a Balatro shaped run around it.
Nothing is built yet. This exists so the design can be played and argued about
before anyone writes C++, which is CLAUDE.md rule 10.

Open `index.html` in a browser. One self contained file, no build step and no
network.

## What it is

Three drums you can see turning, in 3D. A speed dial that trades reading the
symbols against a multiplier. Jokers that fire while the score counts. Hands
you level up. And the ability to open a drum and change what it is able to land
on, which is the deckbuilding.

The page carries a software rasterizer that mirrors `pse::Rasterizer` and
`pse::Renderer3D`: flat shaded triangles, backfaces culled, a one byte depth
buffer, and the same three step lambert the engine shades a face with. The 2D
panel is drawn with the same restricted primitive set the other mockups use,
text is measured with the engine's own 5x7 metrics, and every frame is squeezed
to RGB565.

## The brief it answers

A slot machine, in 3D, with combos, upgrades, symbol swapping, player control
over spin speed, and jokers with special powers. Balatro inspired.

Four of those are one mechanic each. The fifth, spin speed, is the one that
needed a reason to exist, and it became the game's whole risk:

| speed | the symbol on the front face | worth |
| --- | --- | --- |
| SLOW | drawn clearly, so you can watch one come round and stop it | nothing |
| FAIR | greyed, so you can time a stop but not read it comfortably | +2 mult a reel |
| WILD | not drawn at all | +5 mult a reel |

Stopping is optional: leave every drum to stop itself and you take no risk and
get no bonus. Measured on the page, that floor scores 140 on a given spin where
stopping all three at WILD scores 1540. One joker (BLUR) pays x2 for touching
nothing, which is the kind of inversion Balatro builds its best runs from.

## The thing this mockup is really for

**A 3D game in this repo runs at lores 120x120, and a Balatro UI does not fit
in 120x120.** `engine/include/pse/config.hpp` is blunt about why: the depth
buffer is `width * height` bytes, 120x120 costs 14,400, and 240x240 would be
57.6 KB on top of a 115 KB hires framebuffer. That would normally end the idea.

It does not, because of something the engine already does.
`pse::Rasterizer` indexes its depth buffer as `y * target.width + x` on
whatever `RenderTarget` it is handed, and `RenderTarget` carries `row_stride`
separately from `width` precisely so its rows need not be packed. So a target
pointing at row 0 of a 240x240 screen, with height 112 and the screen's own
stride, gives a **3D window in the top band** and a depth buffer that only
covers that band. `PSE_RENDER_WIDTH` and `PSE_RENDER_HEIGHT` are already per
game overridable through `DEFINES`.

| | lores 120x120 | full hires | this |
| --- | --- | --- | --- |
| framebuffer | 28.8 KB | 115.2 KB | 115.2 KB |
| depth buffer | 14.4 KB | 57.6 KB | 26.9 KB |
| triangle queue | 21.8 KB | 21.8 KB | 21.8 KB |
| total scratch | 65.0 KB | 194.6 KB | 163.9 KB |
| left of 264 KB | 199 KB | 69 KB | 100 KB |
| pixels a full frame can shade | 14,400 | 57,600 | 26,880 |

So a readable UI costs 1.9x the lores fill rate rather than 4x, and leaves
100 KB. Whether that is affordable in *time* on a 250 MHz M0+ is not something
this page can answer and it does not pretend to: config.hpp says plainly that
nobody has measured the time budget on hardware. What it can say is measured
and printed on the page: the worst frame of a spin is **160 triangles and
20,814 pixels shaded**, against a window of 26,880 and a queue that holds 640.
The triangle count is nothing. The fill is 77 percent of the window every
frame, most of it textured, and that is the number that would decide this on
hardware. It is the first thing to measure with a cycle counter.

## Twelve facets, and a reel strip that can be longer

A drum is a twelve sided prism, so twelve symbols to start. That is also the
shape that makes a reel look like a reel: a facet is `2 * pi * R / 12` tall, so
a facet only comes out roughly square if the drum is about four times taller
than it is wide, which is exactly what a slot reel is. Sizing it any other way
squashes a square symbol into a colour band, which is what the first attempt at
this did and what the screenshots showed.

**The strip is not limited to twelve.** What the drum shows is a window of
twelve consecutive strip entries centred on whichever entry is at the front.
Turn the drum one facet and the window advances one entry. So the facets being
repainted are always the ones pointing away from the camera, which are backface
culled and therefore never seen changing, and a twelve sided prism can present
a strip of any length. That is how a real machine's virtual reel works, and it
is the trick that lets a swap add a symbol the drum has no facet for: measured
on the page, a strip grown from 12 to 16 still reaches all eight symbols as it
turns, with no seam and no extra geometry.

## Two things running it found

**A spin never ended.** Reels only stopped when the player stopped them, so a
hands off spin turned for ever and the BLUR joker, which pays x2 for stopping
nothing, was unreachable. Every drum now stops itself in sequence if you leave
it, and a hands off spin ends in about 4.6 seconds.

**Two joker texts and the swap hint printed through the right edge.** The page
now measures every string the game can draw against the box it is drawn in, and
says so under the screens. That check is built from the joker, hand and symbol
tables rather than a list somebody typed, because the catcoin build shipped
exactly this bug by listing two of three strings by hand.

## How it maps to the engine

- The drums are 12 facets each plus two end fans, and the cabinet is a back
  panel, a floor and two dividers. All of it is `pse::Renderer3D::draw_mesh`
  work, and rule 11 says the drum and the cabinet should be `.obj` files rather
  than emitted in source.
- **The symbols are textures on the drum faces.** 16x16, power of two, which is
  what `pse::Texture` wants: sampling is a shift and a mask, so wrapping is
  free and there is no divide near the inner loop. A transparent texel leaves
  the drum face showing through, so a symbol reads as printed on the drum, and
  the texel multiplies the face's lambert so it is lit by the same light the
  drum is. Eight of them is 6 KB of flash, and they want to be real PNGs
  through `add_texture` rather than the tables this page carries to stay one
  file.
- The panel is `pse::draw2d` and `pse::draw_text`, the same 2D set the two cart
  demakes use.
- Game state is under 200 bytes: three rings of eight, five joker indices, four
  hand levels, and a tally of at most a dozen entries.

## Open questions

1. **Is 112 rows the right split?** It is the number that lets the panel hold a
   score box, a speed dial, a tally line and five joker slots without crowding.
   More rows to the machine crowds the panel; fewer and the drums stop reading
   as objects.
2. **Three drums or five?** Three keeps a hand readable and the drums big. Five
   gives room for real poker hands and makes each swap cheaper. This is built
   on three.
3. **Should not stopping cost you?** Right now it is the safe floor and one
   joker pays for it. The alternative forces engagement and removes the floor.
4. **Eight jokers is a demo, not a game.** Balatro ships 150. Whether this is
   worth building comes down to whether twenty or so can be written that
   interact, and that is design work to do before the C++, not after.

## Running it

Open the file, click the screen, then Z to pull and Z again to stop each drum
still turning. Up and down move the speed dial, and you can move it mid spin.
Any key starts and continues. R reseeds, and is a bench affordance rather than
a game control.
