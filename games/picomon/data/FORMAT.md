# Picomon data format

Everything the game knows that is not code lives here as plain text, and
`tools/picomon_data.py` compiles the whole directory into one pair of generated
C++ files at build time. Nothing generated is committed.

Two rules shape the format.

**It has to diff.** A map is an ASCII grid, an NPC is a handful of lines. When
someone moves a trainer two tiles left, the diff says so. A binary level file
would say "1280 bytes changed", and after that nobody reviews maps.

**A typo has to be an error.** Every name is resolved at build time against the
thing it names: a zone that sends the player to `rout2`, an encounter that
rolls `emberkid`, an NPC that checks a flag nothing ever sets. All of those
fail the build. A silently ignored key is how a level ends up with an event
that never fires and nobody can see why.

## The files

```
data/
  tileset.txt      what each map character is and how it behaves
  species.txt      the creatures
  moves.txt        what they do
  items.txt        the bag
  zones/*.zone     the maps, one file per zone
```

## Syntax

Line oriented. `#` starts a comment, blank lines are ignored, leading
whitespace is not significant. A record is either a single line

```
warp 15 0 route2 15 30 north
```

or a block that opens with a header and closes with `end`

```
npc rival 20 9 north trainer
  sight 4
  party emberkit 12, mossling 11
  say You walked into my line!
end
```

Unknown keys are an error, not a comment.

## tileset.txt

One line per map character. Everything a map needs to know about a tile is
here, so a zone file is only geometry and the things placed on it.

```
# char  name        flags                 r  g  b
.       grass       walk                  55 99 44
G       tallgrass   walk encounter        33 77 33
p       path        walk                  bb aa 77
T       tree        block                 44 88 44
w       water       block water           33 66 bb
v       ledgesouth  walk ledge_south      8a 7a 55
```

| Flag | Meaning |
| --- | --- |
| `walk` | the player may stand here |
| `block` | solid |
| `water` | solid now, surfable later |
| `encounter` | stepping here rolls against the zone's table for this character |
| `ledge_<dir>` | walkable only when entered from the opposite side, then the player hops one extra tile. One way shortcuts, and the reason a route can be a loop |
| `door` | stepping here looks for a warp rather than blocking |

The renderer stands scenery on a tile by its name, not by its flags: `tree`,
`rock`, `house`, `wall` and `counter` each get a mesh. That is why an
interior wall is `W` and not `H`: `H` is an outdoor cottage with a roof on
it, and a room built out of them was a room full of cottages.

Colours are the ground colour the renderer uses for that material. They are
snapped to four bits per channel by the compiler, because that is what the
panel shows, and a colour that is not a multiple of 0x11 shifts on the device
and nowhere else.

## species.txt

```
species emberkit
  name EMBERKIT
  type ember
  stats 39 52 43 65        # hp atk def spd
  catch 45                 # 1 (never) to 255 (always)
  xp 62                    # base experience yield
  mesh emberkit            # models/<name>.obj
  learn 1 scratch
  learn 1 ember
  learn 9 tailwhip
  learn 16 flareup
  evolve 16 flarette
end
```

`evolve` is optional. `learn` may repeat; the game gives a creature the last
four moves it is eligible for.

## moves.txt

```
move ember
  name EMBER
  type ember
  power 40
  pp 25
  effect none              # none, lower_def, lower_spd, raise_atk, heal_half
end
```

A move with `power 0` is a status move and must have an effect.

## items.txt

```
item picoball
  name PICO BALL
  pocket balls             # balls, medicine, key
  effect ball 4            # ball multiplier in quarters: 4 = x1.0, 6 = x1.5
  price 200                # what a shop charges. Absent means not for sale
  desc A BASIC BALL. ODDS X1.
end
```

`effect` is one of `ball <quarters>`, `heal <hp>`, `cure`, `revive`, `key`.

`price` is optional and defaults to zero, which means the item is not for
sale. A shop that stocks a zero priced item fails the build rather than
handing it over for nothing, which is what the key items rely on.

## zones/*.zone

### Header

```
zone route1
name ROUTE 1
size 40 32
indoor              # optional, and only for rooms
```

`zone` must match the filename. `name` is what the banner shows and is capped
at 16 characters, because that is what fits.

`indoor` says this zone is a room. The renderer drops the sky for it, stops
the ground at the walls rather than running the border tile out into the
void, and draws the room as a cutaway with its near wall left off. An indoor
zone must be sealed: every edge tile has to be solid or a door, and it may
not carry an encounter table, both checked at build time.

Row 0 is the north edge. Pressing up walks the player toward it and it is
drawn at the top of the screen, and both of those are tested, because for a
while only the first was true.

### tiles

Exactly `h` lines of exactly `w` characters, each one defined in the tileset.

```
tiles
TTTTTTTTTTTTTTTT
T..............T
T..GGGG....pppT
...
end
```

### encounter

One block per encountering tile character. `rate` is how often a step on that
tile rolls, as a percentage.

```
encounter G
  rate 11
  emberkit 3 5 40
  mossling 3 5 40
  nibble   2 4 20
end
```

The three numbers are minimum level, maximum level, and weight. Weights are
relative and need not sum to anything; the compiler turns them into a
cumulative 0-255 table so the device rolls one byte and compares.

### npc

```
npc bruno 12 18 south villager
  say The grass to the west moves on its own.
  say Something lives in it.
end
```

Header is `npc <id> <x> <y> <facing> <sheet>`. Facing is `north`, `south`,
`east`, `west`. Sheet names an art sheet in `art/`.

| Key | Meaning |
| --- | --- |
| `say <text>` | one page of dialogue. Repeat for more. |
| `kind <k>` | `villager` (default), `trainer`, `healer`, `shop` |
| `sight <n>` | line of sight in tiles. Implies `kind trainer`. |
| `party <species> <lv>, ...` | up to six. Required for a trainer. |
| `win <text>` | said after the player wins |
| `lose <text>` | said after the player loses |
| `reward <n>` | money on defeat |
| `flag <name>` | set when the NPC is beaten, or first talked to |
| `onlyif <name>` | present only when the flag is set |
| `hideif <name>` | absent once the flag is set |
| `stock <item>, ...` | what this NPC sells. Implies `kind shop` |

A trainer's sight line is drawn on the ground while it is unbeaten, so the
trap is visible and avoidable rather than a gotcha. A trainer with no `sight`
is challenged by walking up and talking to it, which is what a gym leader is.

A `shop` says its `say` lines first and opens its counter when they finish.
Every item it stocks needs a `price`, it may stock at most eight (what the
list shows without scrolling), and it may not stock the same item twice.

An NPC blocks the tile it stands on, which is the whole mechanism behind a
gate: an NPC with `hideif <badge>` standing on the only tile a warp can be
reached from is a locked door that needs no lock. The compiler checks that an
NPC stands on a walkable tile, is not on a warp, is not on top of another
NPC, and that a trainer with a sight line is not looking into a wall.

### warp

```
warp <x> <y> <dest zone> <dest x> <dest y> [facing]
```

Single line. Stepping on the tile moves the player. The destination zone must
exist and the destination tile must be walkable, both checked at build time.

### event

```
event sign 11 16
  say ROUTE 1. TALL GRASS BOTH SIDES.
end

event item 22 8 potion 1
  flag got_potion_r1
end

event trigger 15 20
  flag saw_the_view
  say Something moves in the trees.
end
```

| Kind | Fires on | Extra header args |
| --- | --- | --- |
| `sign` | A pressed while facing it | none |
| `item` | A pressed while standing on it | `<item> <count>` |
| `trigger` | stepping on the tile | none |

An `item` or `trigger` with a `flag` fires once ever: the flag is both the
record that it happened and the reason it does not happen again.

## Flags

Flags are declared by use. Any `flag` key defines one, and `onlyif` / `hideif`
may only name a flag that something sets. The compiler assigns bit indices,
emits them as an enum, and fails on a flag that is read and never written,
which is the typo that would otherwise silently disable an event forever.

The save block carries 64 flag bits. Running out is a build error, not a
corruption.

## What comes out

`picomon_data.hpp` and `picomon_data.cpp` in the build tree: const tables,
readable straight from XIP flash, no dynamic allocation and no parsing on the
device. The device never sees this text.
