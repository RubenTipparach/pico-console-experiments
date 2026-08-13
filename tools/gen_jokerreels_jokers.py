#!/usr/bin/env python3
"""Draw Joker Reels' eight joker icons, as one sheet.

20x20 a cell, eight cells across, written out as a single RGBA PNG and
committed. The build turns it into a pse::Sprite through add_sprite, so nothing
here runs in CI. Re-run it when an icon changes.

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
}


def validate():
    problems = []
    for name, art in ART.items():
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
    missing = [n for n in ORDER if n not in ART]
    if missing:
        problems.append("no art for: %s" % ", ".join(missing))
    extra = [n for n in ART if n not in ORDER]
    if extra:
        problems.append("art with no place in ORDER: %s" % ", ".join(extra))
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


def sheet():
    """Every icon side by side, in ORDER. Cell n is joker n."""
    cells = [render(ART[name]) for name in ORDER]
    rows = []
    for y in range(CELL):
        row = []
        for cell in cells:
            row.extend(cell[y])
        rows.append(row)
    return rows


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
    path = os.path.join(args.out, "jokers.png")
    write_png(path, sheet())
    sys.stderr.write("wrote %s (%d cells of %dx%d)\n"
                     % (path, len(ORDER), CELL, CELL))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
