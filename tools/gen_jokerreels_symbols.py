#!/usr/bin/env python3
"""Draw Joker Reels' eight symbol textures.

16x16 each, which is what pse::Texture wants: power of two, so sampling is a
shift and a mask and wrapping is free. Written out as PNGs and committed, so
the build pipeline sees ordinary art through add_texture and nothing here runs
in CI. Re-run it when a symbol changes.

Two things about these are not decoration:

**The background is white, not transparent.** pse::Texture is three bytes a
texel with no alpha, and it does not need one: the texel MULTIPLIES the face's
lit colour rather than replacing it (see texture.hpp), so a white texel comes
out as exactly the drum face's own shade. That is what makes a symbol read as
printed on the drum rather than stuck to it, and it is also what lets the game
tint a facet yellow to select it and have the tint reach the whole face.

**Every symbol is ringed in black, and the ring is dilated, not drawn.** A drum
face is lit near white, so a white bell on it is a shape you can only find by
squinting. Dilating means the outline cannot go stale when the art changes and
cannot have a gap in it. Each symbol is authored inside a 14x14 box so the ring
has a texel of margin to grow into, and art that reaches the edge is a hard
error rather than a symbol that quietly loses its outline on one side.

Usage:
    gen_jokerreels_symbols.py [--out games/jokerreels/assets]
"""

import argparse
import os
import struct
import sys
import zlib

SIZE = 16

# '.' is the drum face showing through: white, so the multiply leaves the face
# exactly as the light left it.
ART = {
"cherry": [
'................', '................', '........ggg.....', '.......g........',
'......g.g.......', '.....g...g......', '....g.....g.....', '...g.......g....',
'..rrrr....rrrr..', '.rwrrrr..rwrrrr.', '.rrrrrr..rrrrrr.', '.rrrrrr..rrrrrr.',
'.rrrrrr..rrrrrr.', '..rrrr....rrrr..', '................', '................'],
"bell": [
'................', '................', '.......oo.......', '......yyyy......',
'.....yyyyyy.....', '.....yyyyyy.....', '....ywyyyyyy....', '....yyyyyyyy....',
'...ywyyyyyyyy...', '...yyyyyyyyyy...', '..yyyyyyyyyyyy..', '.oooooooooooooo.',
'................', '.......oo.......', '................', '................'],
"plum": [
'................', '................', '........ggg.....', '.......g........',
'.....pppppp.....', '...pppppppppp...', '..ppwwpppppppp..', '..ppwppppppppp..',
'.pppppppppppppp.', '.pppppppppppppp.', '..pppppppppppp..', '..pppppppppppp..',
'...pppppppppp...', '.....pppppp.....', '................', '................'],
"bar": [
'................', '................', '................', '................',
'.wwwwwwwwwwwwww.', '.wbbbbbbbbbbbbw.', '.wbwwwwwwwwwwbw.', '.wbwbbbbbbbbwbw.',
'.wbwbbbbbbbbwbw.', '.wbwwwwwwwwwwbw.', '.wbbbbbbbbbbbbw.', '.wwwwwwwwwwwwww.',
'................', '................', '................', '................'],
"clover": [
'................', '................', '....gg....gg....', '...gggg..gggg...',
'..gggggggggggg..', '..gggggggggggg..', '...gggggggggg...', '....gggggggg....',
'...gggggggggg...', '..gggggggggggg..', '..gggggggggggg..', '...gggg..gggg...',
'....gg.kk.gg....', '.......kk.......', '.......kk.......', '................'],
"seven": [
'................', '................', '..oooooooooooo..', '..oooooooooooo..',
'..oooooooooooo..', '..........ooo...', '.........ooo....', '........ooo.....',
'.......ooo......', '......ooo.......', '.....ooo........', '.....ooo........',
'....ooo.........', '....ooo.........', '................', '................'],
"diamond": [
'................', '................', '.......cc.......', '......cccc......',
'.....wwcccc.....', '....cwcccccc....', '...cccccccccc...', '..cccccccccccc..',
'...cccccccccc...', '....cccccccc....', '.....cccccc.....', '......cccc......',
'.......cc.......', '................', '................', '................'],
"crown": [
'................', '................', '..yy...yy...yy..', '..yy...yy...yy..',
'..yy..yyyy..yy..', '..yyy.yyyy.yyy..', '..yyyyyyyyyyyy..', '..yyyyyyyyyyyy..',
'..yyyyyyyyyyyy..', '..yyrryyyyrryy..', '..yyyyyyyyyyyy..', '.oooooooooooooo.',
'.oooooooooooooo.', '................', '................', '................'],
}

# The console's own palette, so a symbol is a colour the panel can actually
# show rather than one that gets quantised on the way to it.
COLOURS = {
    'r': (0xFF, 0x00, 0x4D), 'g': (0x00, 0xE4, 0x36), 'w': (0xFF, 0xF1, 0xE8),
    'p': (0x7E, 0x25, 0x53), 'b': (0x1D, 0x2B, 0x53), 'o': (0xFF, 0xA3, 0x00),
    'c': (0x29, 0xAD, 0xFF), 'y': (0xFF, 0xEC, 0x27), 'k': (0x00, 0x87, 0x51),
}
FACE = (0xFF, 0xFF, 0xFF)     # left as the drum's own lit colour
OUTLINE = (0x00, 0x00, 0x00)

# The order the game indexes them in. sim.hpp's Symbol enum is this list, and
# tools/tests/test_jokerreels_art.py is what keeps the two in step.
ORDER = ["cherry", "bell", "plum", "bar", "clover", "seven", "diamond", "crown"]


def validate():
    problems = []
    for name, art in ART.items():
        if len(art) != SIZE:
            problems.append("%s: %d rows" % (name, len(art)))
            continue
        for y, row in enumerate(art):
            if len(row) != SIZE:
                problems.append("%s row %d: %d chars" % (name, y, len(row)))
                continue
            for x, ch in enumerate(row):
                if ch == '.':
                    continue
                if ch not in COLOURS:
                    problems.append('%s row %d: unknown "%s"' % (name, y, ch))
                if y in (0, SIZE - 1) or x in (0, SIZE - 1):
                    problems.append(
                        "%s: paints the edge at %d,%d, so the outline has "
                        "nowhere to go" % (name, x, y))
    missing = [n for n in ORDER if n not in ART]
    if missing:
        problems.append("no art for: %s" % ", ".join(missing))
    extra = [n for n in ART if n not in ORDER]
    if extra:
        problems.append("art with no place in ORDER: %s" % ", ".join(extra))
    return problems


def render(art):
    """Symbol, then the dilated ring, then the face colour everywhere else."""
    painted = [[art[y][x] != '.' for x in range(SIZE)] for y in range(SIZE)]
    out = []
    for y in range(SIZE):
        row = []
        for x in range(SIZE):
            ch = art[y][x]
            if ch != '.':
                row.append(COLOURS[ch])
                continue
            touches = any(
                painted[y + dy][x + dx]
                for dy in (-1, 0, 1) for dx in (-1, 0, 1)
                if 0 <= y + dy < SIZE and 0 <= x + dx < SIZE)
            row.append(OUTLINE if touches else FACE)
        out.append(row)
    return out


def write_png(path, pixels):
    raw = b"".join(
        b"\x00" + b"".join(struct.pack("BBB", *px) for px in row)
        for row in pixels)

    def chunk(tag, data):
        body = tag + data
        return (struct.pack(">I", len(data)) + body +
                struct.pack(">I", zlib.crc32(body) & 0xFFFFFFFF))

    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", SIZE, SIZE, 8, 2, 0, 0, 0))
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
            sys.stderr.write("gen_jokerreels_symbols: %s\n" % problem)
        return 1

    os.makedirs(args.out, exist_ok=True)
    for name in ORDER:
        path = os.path.join(args.out, "%s.png" % name)
        write_png(path, render(ART[name]))
        sys.stderr.write("wrote %s\n" % path)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
