#!/usr/bin/env python3
"""Measure the character sheets and print what PROPORTIONS.md asserts.

    python3 measure.py            every character sheet
    python3 measure.py hero       one of them
    python3 measure.py --md       the tables, as markdown, for pasting

PROPORTIONS.md is the blueprint the art is drawn against, and every number in
it comes out of here. Run this after changing a head, a body or a set of legs
and the doc's "what we have" section is re-derivable in one command, which is
the only way a written down proportion survives contact with an afternoon of
pixel pushing.

What it prints, per sheet:

  a per row map      one character a pixel, so a boundary is a fact you can
                     count rather than a judgement about where a chin looks
                     like it is
  the band split     head, body and legs as pixel rows, counts and percentages

The map's alphabet is deliberately four symbols and not sixteen. What decides
a boundary is whether a row is outline, skin, some other fill, or nothing;
which particular green a jacket is has never once been the question.

The outline belongs to the part it outlines. A head's last row is the dark
rule under the chin, not the last row of skin. Counting it the other way
under-measures every outlined sprite by a row, and it under-measured all
three references this game was drawn against before anybody noticed.
"""
import os
import struct
import sys
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import build_art as art

HERE = os.path.dirname(os.path.abspath(__file__))

# The character sheets, in the order build_art.py emits them. Scenery has no
# head and no legs, so it is not measured here.
SHEETS = ["hero", "villager", "trainer", "healer"]


def read_png(path):
    """Return (w, h, rows of (r, g, b, a)). Enough PNG for our own output."""
    data = open(path, "rb").read()
    i, idat, w, h, depth, ctype, plte, trns = 8, b"", 0, 0, 0, 0, None, None
    while i < len(data):
        n = struct.unpack(">I", data[i:i + 4])[0]
        tag, body = data[i + 4:i + 8], data[i + 8:i + 8 + n]
        if tag == b"IHDR":
            w, h, depth, ctype = struct.unpack(">IIBB", body[:10])[:4]
        elif tag == b"PLTE":
            plte = body
        elif tag == b"tRNS":
            trns = body
        elif tag == b"IDAT":
            idat += body
        i += 12 + n
    chans = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}[ctype]
    stride = (w * chans * depth + 7) // 8
    bpp = max(1, chans * depth // 8)
    raw = zlib.decompress(idat)
    out, prev = [], bytearray(stride)
    for y in range(h):
        f = raw[y * (stride + 1)]
        line = bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        for x in range(stride):
            a = line[x - bpp] if x >= bpp else 0
            b = prev[x]
            c = prev[x - bpp] if x >= bpp else 0
            if f == 1:
                line[x] = (line[x] + a) & 255
            elif f == 2:
                line[x] = (line[x] + b) & 255
            elif f == 3:
                line[x] = (line[x] + (a + b) // 2) & 255
            elif f == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                line[x] += a if pa <= pb and pa <= pc else b if pb <= pc else c
                line[x] &= 255
        row = []
        for x in range(w):
            if ctype == 3:
                ix = ((line[x // 2] >> (4 if x % 2 == 0 else 0)) & 15
                      if depth == 4 else line[x])
                r, g, bl = plte[ix * 3:ix * 3 + 3]
                al = trns[ix] if trns and ix < len(trns) else 255
            elif ctype == 6:
                r, g, bl, al = line[x * 4:x * 4 + 4]
            elif ctype == 2:
                r, g, bl = line[x * 3:x * 3 + 3]
                al = 255
            else:
                r = g = bl = line[x]
                al = 255
            row.append((r, g, bl, al))
        out.append(row)
        prev = line
    return w, h, out


def classify(r, g, b, a):
    """One character a pixel: outline, skin, other fill, nothing."""
    if a < 128:
        return "."
    if (r * 299 + g * 587 + b * 114) // 1000 < 70:
        return "#"
    if r > 185 and g > 140 and b > 100 and r >= g >= b and r - b < 125:
        return "s"
    return "+"


def frame_map(px, x0, fw, fh):
    return ["".join(classify(*px[y][x]) for x in range(x0, x0 + fw))
            for y in range(fh)]


def bands():
    """Where the head, the body and the legs start and stop.

    Not detected. Declared. compose() stacks three parts of exactly HEAD_H,
    BODY_H and LEGS_H rows, so for our own art the boundaries are known
    exactly and there is nothing to infer.

    Inferring them was tried twice and failed twice, which is worth writing
    down because it is a tempting thing to try a third time. Scoring rows by
    skin puts a chin wherever the hands are; looking for the row where the
    silhouette splits finds the gap between the boots. Anatomy is not
    recoverable from a 12 pixel wide bitmap by rule, which is exactly why the
    reference numbers in PROPORTIONS.md were read off a magnified ruler by a
    person and written down rather than computed.
    """
    return (0, art.HEAD_H - 1,
            art.BODY_TOP, art.LEGS_TOP - 1,
            art.LEGS_TOP, art.H - 1)


def report(name, md=False):
    path = os.path.join(HERE, f"{name}.png")
    w, h, px = read_png(path)
    fw, fh = art.W, art.H
    rows = frame_map(px, fw, fw, fh)    # frame 1: the standing pose
    h0, h1, b0, b1, l0, l1 = bands()
    head, body, legs = h1 - h0 + 1, b1 - b0 + 1, l1 - l0 + 1
    overlap = h1 - b0 + 1
    if md:
        print(f"| {name} | {h0}-{h1} | {b0}-{b1} | {l0}-{l1} | {fh}"
              f" | {head} ({head * 100 // fh}%)"
              f" | {body} ({body * 100 // fh}%)"
              f" | {legs} ({legs * 100 // fh}%) |")
        return
    print(f"\n=== {name}  {fw}x{fh}, frame 1")
    print("     " + "".join(str(x % 10) for x in range(fw)))
    for y, r in enumerate(rows):
        mark = ("  <- head starts" if y == h0 else
                "  <- body starts, behind the head" if y == b0 else
                "  <- head ends" if y == h1 else
                "  <- legs start" if y == l0 else "")
        print(f"  {y:2d} {r}{mark}")
    print(f"  head {h0}-{h1} = {head} px ({head * 100 // fh}%)   "
          f"body {b0}-{b1} = {body} px   "
          f"legs {l0}-{l1} = {legs} px ({legs * 100 // fh}%)")
    print(f"  head and body overlap on rows {b0}-{h1}, {overlap} rows, which "
          "is where the arms come up beside the face")
    # The one thing worth checking rather than printing: a head with no eyes
    # in it means the declared split and the drawn art have come apart.
    if "s" in "".join(rows[h0:h1 + 1]) and "#" not in rows[(h0 + h1) // 2]:
        print("  WARNING: no outline in the middle of the head band, so the "
              "eyes are not where the split says the face is")


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("-")]
    md = "--md" in sys.argv
    names = args or SHEETS
    if md:
        print("| sheet | head rows | body rows | legs rows | inked | head | "
              "body | legs |")
        print("|---|---|---|---|---|---|---|---|")
    for n in names:
        report(n, md)


if __name__ == "__main__":
    main()
