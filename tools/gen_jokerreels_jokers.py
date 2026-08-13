#!/usr/bin/env python3
"""Draw Joker Reels' icons: the jokers, the consumables, the hands, the extras.

20x20 a cell, one sheet per family, written out as RGBA PNGs and committed. The
build turns each into a pse::Sprite through add_sprite, so nothing here runs in
CI. Re-run it when an icon changes.

Four sheets rather than one, because a cell index has to mean something: on
`jokers.png` the cell IS the Joker enum value, on `items.png` it is the Item
enum value, and on `hands.png` it is the Hand enum value. One sheet with
everything on it would need an offset table in render.cpp, which is the table
this whole arrangement exists to not have.

**The hand icons are generated from the hand's own shape**, not drawn. A hand
is a pattern across five reels, so its icon is five pips with the matching ones
lit: three of a kind is three of one colour and two greys, two pair is two and
two and a grey. Drawing eight of those by hand would be eight chances to draw
the wrong pattern for the name beside it.

Three things about this are not decoration:

**One sheet, not eight files.** pse::blit_sprite takes a region, and the engine
says in as many words that a sheet is how a sprite is used: one picture carries
every frame and the caller names the cell. That matters more here than
tidiness. Eight separate sprites would need a table in render.cpp listing them
in the Joker enum's order, and a table like that is a fourth place the order is
written down and a fourth place it can go wrong. With a sheet the cell IS the
enum value, `sx = joker * CELL`, and there is no table to misorder.

**The background is transparent, and the outline is dilated.** These are drawn
over a dark panel, a lit shop card and a black end screen, so an icon that
relied on one background would vanish on another. Dilating the ring rather than
drawing it means it cannot have a gap and cannot go stale when the art changes.
Each icon is authored inside an 18x18 box so the ring has a pixel of margin to
grow into, and art that reaches the edge of its cell is a hard error rather
than an icon that quietly loses its outline down one side.

**The order is the Joker enum's.** tools/tests/test_jokerreels_art.py is what
keeps the two in step, the same way it does for the symbols.

Usage:
    gen_jokerreels_jokers.py [--out games/jokerreels/assets]
"""

import argparse
import os
import struct
import sys
import zlib

CELL = 20

# The order sim.hpp's Joker enum is in. A name here is that enum entry with the
# k stripped and lowercased, which is exactly what the test compares.
ORDER = ["greaser", "twin", "ratchet", "blur", "collector", "metronome",
         "sunkcost", "understudy"]

# The same, for sim.hpp's Item enum.
ITEM_ORDER = ["hotstreak", "doubledown", "sparespin", "luckycoin", "polish",
              "blueprint"]

# And the extras, which are the two shop rows that are not a thing you own: a
# drum to open and a shelf to reroll. Their order is render.cpp's, and it is
# named here so there is one list rather than two.
EXTRA_ORDER = ["swap", "reroll"]

# The console's own palette, so an icon is a colour the panel can actually
# show rather than one that gets quantised on the way to it.
COLOURS = {
    'r': (0xFF, 0x00, 0x4D), 'g': (0x00, 0xE4, 0x36), 'w': (0xFF, 0xF1, 0xE8),
    'p': (0x7E, 0x25, 0x53), 'b': (0x1D, 0x2B, 0x53), 'o': (0xFF, 0xA3, 0x00),
    'c': (0x29, 0xAD, 0xFF), 'y': (0xFF, 0xEC, 0x27), 'k': (0x00, 0x87, 0x51),
    's': (0xC2, 0xC3, 0xC7), 'e': (0x5F, 0x57, 0x4F), 'n': (0xAB, 0x52, 0x36),
    'i': (0xFF, 0x77, 0xA8), 'l': (0x83, 0x76, 0x9C),
}
OUTLINE = (0x00, 0x00, 0x00)

# Every icon is a picture of what the joker DOES, not a portrait of a joker.
# Five of these sit side by side in a 240 px panel with no room for a name
# under them, so the only thing telling a player which one just fired is the
# silhouette. That is why they are eight different shapes rather than eight
# recolours of a face.
ART = {
    # An oil can with a drop coming off it: the reels are greased, so they
    # stop fast.
    "greaser": [
        '....................',
        '....................',
        '...............yy...',
        '..............yyyy..',
        '..............yyyy..',
        '...............yy...',
        '...........sss......',
        '..........sss.......',
        '.........sss........',
        '........ssss........',
        '..ssssssssss........',
        '..ssssssssssss......',
        '..swwsssssssss......',
        '..swssssssssss......',
        '..ssssssssssss......',
        '..ssssssssssss......',
        '..ssssssssssss......',
        '...ssssssssss.......',
        '....................',
        '....................',
    ],
    # Two of the same coin. A pair, which is what it is paid for.
    "twin": [
        '....................',
        '....................',
        '....................',
        '....................',
        '....................',
        '....................',
        '...yyyy......yyyy...',
        '..yyyyyy....yyyyyy..',
        '.yyyyyyyy..yyyyyyyy.',
        '.yywyyyyy..yywyyyyy.',
        '.yywyyyyy..yywyyyyy.',
        '.yyyyyyyy..yyyyyyyy.',
        '..yyyyyy....yyyyyy..',
        '...yyyy......yyyy...',
        '....................',
        '....................',
        '....................',
        '....................',
        '....................',
        '....................',
    ],
    # A cog, which only turns one way. One more mult for every spin the round
    # has already taken.
    "ratchet": [
        '....................',
        '....................',
        '.......ssssss.......',
        '.......ssssss.......',
        '......ssssssss......',
        '.....ssssssssss.....',
        '....ssssssssssss....',
        '....ssssssssssss....',
        '.sssssss....sssssss.',
        '.sssssss....sssssss.',
        '.sssssss....sssssss.',
        '.sssssss....sssssss.',
        '....ssssssssssss....',
        '....ssssssssssss....',
        '.....ssssssssss.....',
        '......ssssssss......',
        '.......ssssss.......',
        '.......ssssss.......',
        '....................',
        '....................',
    ],
    # A drum still turning, with the speed lines that mean you never stopped
    # it. That is the whole condition.
    "blur": [
        '....................',
        '....................',
        '....................',
        '............sssss...',
        '...........sssssss..',
        '...........sssssss..',
        '..cccccc...sssssss..',
        '...........sssssss..',
        '...........sssssss..',
        '....cccc...sssssss..',
        '...........sssssss..',
        '...........sssssss..',
        '..cccccc...sssssss..',
        '...........sssssss..',
        '...........sssssss..',
        '...........sssssss..',
        '............sssss...',
        '....................',
        '....................',
        '....................',
    ],
    # A jar with three different things in it. Paid per DIFFERENT symbol, so
    # the picture has to be of variety rather than of quantity.
    "collector": [
        '....................',
        '....................',
        '.....nnnnnnnnnn.....',
        '.....nnnnnnnnnn.....',
        '......ssssssss......',
        '....cccccccccccc....',
        '...cccccccccccccc...',
        '...cccccccccccccc...',
        '...ccrrccccyyyccc...',
        '...ccrrccccyyyccc...',
        '...cccccccccccccc...',
        '...cccgggcccccccc...',
        '...cccgggcccccccc...',
        '...cccccccccccccc...',
        '...cccccccccccccc...',
        '...cccccccccccccc...',
        '....cccccccccccc....',
        '.....cccccccccc.....',
        '....................',
        '....................',
    ],
    # A metronome: every beat the same as the last, which is the condition,
    # every reel stopped at the same speed.
    "metronome": [
        '....................',
        '....................',
        '.........pp.........',
        '........pppp........',
        '........ppsp........',
        '.......pppspp.......',
        '.......ppppsp.......',
        '......pppppspp......',
        '......ppppppsp......',
        '.....ppppppyyyp.....',
        '.....ppppppyyyp.....',
        '....pppppppyyypp....',
        '....pppppppppspp....',
        '...pppppppppppspp...',
        '...pppppppppppspp...',
        '..pppppppppppppppp..',
        '..pppppppppppppppp..',
        '..pppppppppppppppp..',
        '....................',
        '....................',
    ],
    # An anchor: the spins already spent, and what they are now worth.
    "sunkcost": [
        '....................',
        '........ssss........',
        '........s..s........',
        '........s..s........',
        '.........ss.........',
        '....ssssssssssss....',
        '.........ss.........',
        '.........ss.........',
        '.........ss.........',
        '.........ss.........',
        '.........ss.........',
        '.........ss.........',
        '..s......ss......s..',
        '..s......ss......s..',
        '..s......ss......s..',
        '..ss.....ss.....ss..',
        '...ss..ssssss..ss...',
        '....ssssssssssss....',
        '......ssssssss......',
        '....................',
    ],
    # A mask, and an arrow pointing at whatever is to its left. It has no act
    # of its own.
    "understudy": [
        '....................',
        '....................',
        '....................',
        '........wwwwwww.....',
        '.......wwwwwwwww....',
        '.......wwwwwwwww....',
        '.......wwwwwwwww....',
        '.......wwbwwwbww....',
        '.......wwbwwwbww....',
        '...yyy.wwwwwwwww....',
        '.yyyyy.wwwwwwwww....',
        '...yyy.wwwwwwwww....',
        '.......wwwwwwwww....',
        '.......wbbbbbbbw....',
        '.......wwwwwwwww....',
        '........wwwwwww.....',
        '.........wwwww......',
        '....................',
        '....................',
        '....................',
    ],

    # ---- the consumables ----
    # A flame: the next spin runs hot.
    "hotstreak": [
        '....................',
        '....................',
        '...........oo.......',
        '..........oooo......',
        '.........ooooo......',
        '........oooooo......',
        '.......oooyyoo......',
        '......oooyyyyoo.....',
        '.....oooyyyyyoo.....',
        '....oooyyyyyyooo....',
        '....oooyyyyyyooo....',
        '....oooyyyyyyooo....',
        '....oooyyyyyyooo....',
        '.....oooyyyyooo.....',
        '.....oooyyyyooo.....',
        '......oooyyooo......',
        '.......oooooo.......',
        '........oooo........',
        '....................',
        '....................',
    ],
    # Two stacks of chips, one taller. The chips half of the sum, doubled.
    "doubledown": [
        '....................',
        '....................',
        '....................',
        '....................',
        '....................',
        '....................',
        '...........ccccccc..',
        '...........bbbbbbb..',
        '...........ccccccc..',
        '...........bbbbbbb..',
        '...........ccccccc..',
        '...........bbbbbbb..',
        '..ccccccc..ccccccc..',
        '..bbbbbbb..bbbbbbb..',
        '..ccccccc..ccccccc..',
        '..bbbbbbb..bbbbbbb..',
        '..ccccccc..ccccccc..',
        '..bbbbbbb..bbbbbbb..',
        '....................',
        '....................',
    ],
    # A plus in a disc: one more of the thing you are running out of.
    "sparespin": [
        '....................',
        '....................',
        '.......gggggg.......',
        '.....gggggggggg.....',
        '....gggggggggggg....',
        '...gggggg..gggggg...',
        '...gggggg..gggggg...',
        '..ggggggg..ggggggg..',
        '..ggggggg..ggggggg..',
        '..ggg..........ggg..',
        '..ggg..........ggg..',
        '..ggggggg..ggggggg..',
        '..ggggggg..ggggggg..',
        '...gggggg..gggggg...',
        '...gggggg..gggggg...',
        '....gggggggggggg....',
        '.....gggggggggg.....',
        '.......gggggg.......',
        '....................',
        '....................',
    ],
    # One coin, rimmed, with a glint. Gold, right now.
    "luckycoin": [
        '....................',
        '....................',
        '.......oooooo.......',
        '.....oyyyyyyyyo.....',
        '....oyyyyyyyyyyo....',
        '...oywwyyyyyyyyyo...',
        '...oywyyyyyyyyyyo...',
        '..oyyyyyyyyyyyyyyo..',
        '..oyyyyyyyyyyyyyyo..',
        '..oyyyyyyyyyyyyyyo..',
        '..oyyyyyyyyyyyyyyo..',
        '..oyyyyyyyyyyyyyyo..',
        '..oyyyyyyyyyyyyyyo..',
        '...oyyyyyyyyyyyyo...',
        '...oyyyyyyyyyyyyo...',
        '....oyyyyyyyyyyo....',
        '.....oyyyyyyyyo.....',
        '.......oooooo.......',
        '....................',
        '....................',
    ],
    # An arrow up, with sparkles: a symbol on a drum becomes a better one.
    "polish": [
        '....................',
        '....................',
        '...y................',
        '..yyy....gg.........',
        '...y....gggg........',
        '.......gggggg.......',
        '......gggggggg......',
        '.....gggggggggg.....',
        '....gggggggggggg....',
        '.......gggggg.......',
        '.......gggggg.......',
        '.......gggggg...y...',
        '.......gggggg..yyy..',
        '.......gggggg...y...',
        '.......gggggg.......',
        '.......gggggg.......',
        '....................',
        '....................',
        '....................',
        '....................',
    ],
    # A ruled plan: the shape you already made, drawn up again a level better.
    "blueprint": [
        '....................',
        '....................',
        '....................',
        '...ccccwccccwcccc...',
        '...ccccwccccwcccc...',
        '...ccccwccccwcccc...',
        '...ccccwccccwcccc...',
        '...wwwwwwwwwwwwww...',
        '...ccccwccccwcccc...',
        '...ccccwccccwcccc...',
        '...ccccwccccwcccc...',
        '...ccccwccccwcccc...',
        '...wwwwwwwwwwwwww...',
        '...ccccwccccwcccc...',
        '...ccccwccccwcccc...',
        '...ccccwccccwcccc...',
        '...ccccwccccwcccc...',
        '....................',
        '....................',
        '....................',
    ],

    # ---- the two shop rows that are not a thing you own ----
    # A drum with two faces showing: the one screen where you change what a
    # reel is able to land on.
    "swap": [
        '....................',
        '....................',
        '....................',
        '.......ssssss.......',
        '.....ssssssssss.....',
        '.....ssssssssss.....',
        '.....ssyyyyyyss.....',
        '.....ssyyyyyyss.....',
        '.....ssyyyyyyss.....',
        '.....ssssssssss.....',
        '.....ssssssssss.....',
        '.....ssooooooss.....',
        '.....ssooooooss.....',
        '.....ssooooooss.....',
        '.....ssssssssss.....',
        '.....ssssssssss.....',
        '.......ssssss.......',
        '....................',
        '....................',
        '....................',
    ],
    # Two dice: the shelf, thrown again.
    "reroll": [
        '....................',
        '....................',
        '..wwwwwwww..........',
        '..wwwwwwww..........',
        '..wbwwwwbw..........',
        '..wwwwwwww..........',
        '..wwwbbwww..........',
        '..wwwbbwww..........',
        '..wwwwwwww..........',
        '..wbwwwwbw..........',
        '..wwwwwwww..........',
        '..........wwwwwwww..',
        '..........wbwwwwww..',
        '..........wwwwwwww..',
        '..........wwwbwwww..',
        '..........wwwwbwww..',
        '..........wwwwwwww..',
        '..........wwwwwwbw..',
        '..........wwwwwwww..',
        '....................',
    ],
}


def validate():
    problems = []
    missing = [n for n in HAND_ORDER if n not in HANDS]
    if missing:
        problems.append("no pattern for hand: %s" % ", ".join(missing))
    for name in HAND_ORDER:
        if name not in HANDS:
            continue
        if len(HANDS[name]) != 5:
            problems.append("%s covers %d reels, not five"
                            % (name, len(HANDS[name])))
    art_of = dict(ART)
    for name in HAND_ORDER:
        if name in HANDS and len(HANDS[name]) == 5:
            art_of["hand:" + name] = hand_art(HANDS[name])
    for name, art in art_of.items():
        if len(art) != CELL:
            problems.append("%s: %d rows" % (name, len(art)))
            continue
        for y, row in enumerate(art):
            if len(row) != CELL:
                problems.append("%s row %d: %d chars" % (name, y, len(row)))
                continue
            for x, ch in enumerate(row):
                if ch == '.':
                    continue
                if ch not in COLOURS:
                    problems.append('%s row %d: unknown "%s"' % (name, y, ch))
                if y in (0, CELL - 1) or x in (0, CELL - 1):
                    problems.append(
                        "%s: paints the edge of its cell at %d,%d, so the "
                        "outline has nowhere to go" % (name, x, y))
    drawn = ORDER + ITEM_ORDER + EXTRA_ORDER
    missing = [n for n in drawn if n not in ART]
    if missing:
        problems.append("no art for: %s" % ", ".join(missing))
    extra = [n for n in ART if n not in drawn]
    if extra:
        problems.append("art with no place in any order: %s" % ", ".join(extra))
    return problems


def render(art):
    """The icon, then the dilated ring, then nothing at all.

    RGBA, and alpha is a mask rather than a blend: pse::blit_sprite draws a
    pixel or skips it, so there is no half transparent edge to get wrong.
    """
    painted = [[art[y][x] != '.' for x in range(CELL)] for y in range(CELL)]
    out = []
    for y in range(CELL):
        row = []
        for x in range(CELL):
            ch = art[y][x]
            if ch != '.':
                row.append(COLOURS[ch] + (255,))
                continue
            touches = any(
                painted[y + dy][x + dx]
                for dy in (-1, 0, 1) for dx in (-1, 0, 1)
                if 0 <= y + dy < CELL and 0 <= x + dx < CELL)
            row.append(OUTLINE + (255,) if touches else (0, 0, 0, 0))
        out.append(row)
    return out


def sheet(order=None):
    """Every icon of one family side by side. Cell n is enum value n."""
    cells = [render(ART[name]) for name in (order or ORDER)]
    return join(cells)


def join(cells):
    rows = []
    for y in range(CELL):
        row = []
        for cell in cells:
            row.extend(cell[y])
        rows.append(row)
    return rows


# ---------------------------------------------------------------------------
# The hands, drawn from the hand rather than by hand
# ---------------------------------------------------------------------------
#
# A hand is a pattern across five reels, so its icon is five bars: the reels
# that matched stand tall in the group's colour, the ones that took no part sit
# low in grey, and a run climbs. That makes the picture the pattern, which is
# the only thing an icon this small can usefully be, and it means the eight of
# them cannot disagree with the eight names beside them.
#
# The order is sim.hpp's Hand enum, best to worst, and
# tools/tests/test_jokerreels_art.py is what keeps the two in step.
HAND_ORDER = ["five", "four", "fullhouse", "run", "three", "twopair", "pair",
              "nothing"]

# Per hand: one entry a reel, (group, height). Group 0 and 1 are the two match
# groups, 2 is a run, and -1 is a reel that took no part.
TALL = 11
SHORT = 4
HANDS = {
    "five":      [(0, TALL)] * 5,
    "four":      [(0, TALL)] * 4 + [(-1, SHORT)],
    "fullhouse": [(0, TALL)] * 3 + [(1, TALL)] * 2,
    "run":       [(2, 3), (2, 5), (2, 7), (2, 9), (2, 11)],
    "three":     [(0, TALL)] * 3 + [(-1, SHORT)] * 2,
    "twopair":   [(0, TALL)] * 2 + [(1, TALL)] * 2 + [(-1, SHORT)],
    "pair":      [(0, TALL)] * 2 + [(-1, SHORT)] * 3,
    "nothing":   [(-1, SHORT)] * 5,
}
GROUP_COLOUR = {0: 'o', 1: 'c', 2: 'g', -1: 'e'}

# Two wide with two clear between, which lands the five bars on 1..18 and
# leaves the dilated outline a column to live in on both sides.
BAR_W = 2
BAR_PITCH = 4
BAR_X0 = 1
BASELINE = 16


def hand_art(bars):
    grid = [['.'] * CELL for _ in range(CELL)]
    for i, (group, height) in enumerate(bars):
        colour = GROUP_COLOUR[group]
        x0 = BAR_X0 + i * BAR_PITCH
        for y in range(BASELINE - height + 1, BASELINE + 1):
            for x in range(x0, x0 + BAR_W):
                grid[y][x] = colour
    return ["".join(row) for row in grid]


def hand_sheet():
    return join([render(hand_art(HANDS[name])) for name in HAND_ORDER])


def write_png(path, pixels):
    height = len(pixels)
    width = len(pixels[0])
    raw = b"".join(
        b"\x00" + b"".join(struct.pack("BBBB", *px) for px in row)
        for row in pixels)

    def chunk(tag, data):
        body = tag + data
        return (struct.pack(">I", len(data)) + body +
                struct.pack(">I", zlib.crc32(body) & 0xFFFFFFFF))

    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(raw, 9))
    png += chunk(b"IEND", b"")
    with open(path, "wb") as handle:
        handle.write(png)


def main(argv):
    here = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out",
                        default=os.path.join(here, "games", "jokerreels",
                                             "assets"))
    args = parser.parse_args(argv)

    problems = validate()
    if problems:
        for problem in problems:
            sys.stderr.write("gen_jokerreels_jokers: %s\n" % problem)
        return 1

    os.makedirs(args.out, exist_ok=True)
    families = [
        ("jokers.png", sheet(ORDER), len(ORDER)),
        ("items.png", sheet(ITEM_ORDER), len(ITEM_ORDER)),
        ("extras.png", sheet(EXTRA_ORDER), len(EXTRA_ORDER)),
        ("hands.png", hand_sheet(), len(HAND_ORDER)),
    ]
    for name, pixels, count in families:
        path = os.path.join(args.out, name)
        write_png(path, pixels)
        sys.stderr.write("wrote %s (%d cells of %dx%d)\n"
                         % (path, count, CELL, CELL))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
