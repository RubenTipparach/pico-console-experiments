#!/usr/bin/env python3
"""The house style of Picomon's people.

The cast is chibi: a head that is half the character, eyes large enough to
carry an expression, stubby arms, short legs. That is not decoration, it is
what makes a 12 x 20 figure readable at 10 pixels a tile. A head drawn to
realistic proportion is about five rows, and five rows cannot hold two eyes
that read as a face, so the character arrives on screen as a smudge with a
shirt.

The rules here are the ones that go wrong quietly. Every one of them was a
real state of this art at some point in an afternoon:

  - a character composed from parts whose rows do not add to 20, which lines
    up perfectly and stands a few pixels lower than everybody else
  - a face with no eye whites, which reads as blank at any distance
  - eyes drawn touching, which merge into a single dark bar
  - one character drawn to different proportions from the rest, which reads
    as a different game rather than a different person

build_art.py raises on a row of the wrong width, so that is not retested
here. What it cannot see is style, and style is the thing four separate dicts
of pixel strings will drift on.

Usage:
    test_picomon_sprites.py
"""
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
ART = os.path.join(ROOT, "games", "picomon", "art")
sys.path.insert(0, ART)

import build_art as art


def fail(msg):
    print(f"FAIL: {msg}")
    sys.exit(1)


def test_the_parts_fill_the_frame():
    total = art.HEAD_H + art.BODY_H + art.LEGS_H
    if total != art.H:
        fail(f"head {art.HEAD_H} + body {art.BODY_H} + legs {art.LEGS_H} = "
             f"{total}, but a frame is {art.H} rows. Parts that do not fill "
             "the frame compose without complaint and stand the character at "
             "the wrong height")
    print(f"  {art.HEAD_H} head + {art.BODY_H} body + {art.LEGS_H} legs "
          f"= {art.H} rows")


def test_the_head_is_half_the_character():
    """The one number the whole style rests on."""
    if art.HEAD_H * 2 < art.H:
        fail(f"the head is {art.HEAD_H} of {art.H} rows. Under half and there "
             "is no room for eyes that read, which is the entire reason "
             "these characters are chibi rather than scaled down adults")
    if art.HEAD_H * 3 > art.H * 2:
        fail(f"the head is {art.HEAD_H} of {art.H} rows, over two thirds. "
             "Past that the body cannot hold a torso and a walk, and the "
             "character reads as a balloon on legs")


def test_every_part_is_the_height_it_claims():
    for name, rows in sorted(art.HEAD.items()):
        if len(rows) != art.HEAD_H:
            fail(f"head {name!r} is {len(rows)} rows, not {art.HEAD_H}")
    for name, poses in sorted(art.BODY.items()):
        for pose, rows in sorted(poses.items()):
            if len(rows) != art.BODY_H:
                fail(f"body {name!r} pose {pose!r} is {len(rows)} rows, "
                     f"not {art.BODY_H}")
    for name, poses in sorted(art.LEGS.items()):
        for pose, rows in sorted(poses.items()):
            if len(rows) != art.LEGS_H:
                fail(f"legs {name!r} pose {pose!r} is {len(rows)} rows, "
                     f"not {art.LEGS_H}")
    print(f"  {len(art.HEAD)} heads, {len(art.BODY)} bodies, "
          f"{len(art.LEGS)} sets of legs, all the right height")


def faces_forward(rows):
    """A head with skin in it is a head that is looking at the player."""
    return any("s" in r for r in rows)


def test_every_face_has_eyes_with_whites():
    for name, rows in sorted(art.HEAD.items()):
        if not faces_forward(rows):
            continue                      # the back of a head has no face
        pupils = sum(r.count("e") for r in rows)
        whites = sum(r.count("i") for r in rows)
        if pupils == 0:
            fail(f"head {name!r} has skin but no pupils, so it is a blank "
                 "face. This is exactly what the pre-chibi art shipped")
        if whites == 0:
            fail(f"head {name!r} has pupils but no eye whites. Without them "
                 "the eye is a dark blob on skin and the face reads as blank "
                 "at the size it is actually seen")
    print(f"  every face that is turned toward the player has eyes")


def test_the_eyes_do_not_touch():
    """Two eyes with no gap are one bar."""
    for name, rows in sorted(art.HEAD.items()):
        # A profile shows one eye, so there is nothing for it to touch. Skip
        # it by name rather than by counting the pixels: counting would also
        # skip a front face whose eyes had shrunk to one pixel each, which is
        # a thing worth failing on.
        if not faces_forward(rows) or name.endswith(".side"):
            continue
        for i, row in enumerate(rows):
            cols = [x for x, ch in enumerate(row) if ch in "ei"]
            if not cols:
                continue
            runs = []
            for x in cols:
                if runs and x == runs[-1][-1] + 1:
                    runs[-1].append(x)
                else:
                    runs.append([x])
            if len(runs) < 2:
                fail(f"head {name!r} row {i} draws its eyes as one run of "
                     f"{len(cols)} pixels. Two eyes with nothing between them "
                     "read as a visor")
            gap = runs[1][0] - runs[0][-1] - 1
            if gap < 2:
                fail(f"head {name!r} row {i} leaves {gap} pixels between the "
                     "eyes. Under two and they merge at a glance")
    print("  no character's eyes merge into one bar")


def test_the_cast_shares_one_face():
    """Four characters, one set of proportions.

    The eyes have to sit on the same rows relative to the head and be the
    same size, or the cast reads as coming from different games. A hat is
    allowed to push a face down, so the comparison is of shape, not of
    position.
    """
    shapes = {}
    for name, rows in sorted(art.HEAD.items()):
        if not faces_forward(rows):
            continue
        eye_rows = [r for r in rows if "i" in r or "e" in r]
        if not eye_rows:
            continue
        shapes[name] = tuple(
            tuple(1 if ch in "ei" else 0 for ch in r) for r in eye_rows)
    fronts = {n: s for n, s in shapes.items() if not n.endswith(".side")}
    distinct = set(fronts.values())
    if len(distinct) > 1:
        names = ", ".join(sorted(fronts))
        fail(f"the front facing heads ({names}) draw their eyes in "
             f"{len(distinct)} different shapes. One cast, one face")
    print(f"  {len(fronts)} front facing characters share one eye shape")


def test_the_art_still_builds():
    """The palettes have room for the eye whites.

    A sheet is indexed at four bits, so fifteen colours and no more. Adding
    white to a character that was already at fourteen is a build failure and
    not a slightly worse picture, and the trainer sits at exactly fifteen.
    """
    import contextlib
    import io
    import tempfile
    out = io.StringIO()
    argv = sys.argv[:]
    # Into a temp directory. build_art.py's default output lands next to
    # itself, and a test that rewrites the tree it is testing is a test that
    # can only be run once.
    with tempfile.TemporaryDirectory() as tmp:
        sys.argv = ["build_art.py", "--out-dir", tmp]
        try:
            with contextlib.redirect_stdout(out):
                art.main()
        except SystemExit as e:
            fail(f"build_art.py refused to build the art: {e}")
        finally:
            sys.argv = argv
    for name, (pal, frames) in art.CPP_FRAMES.items():
        used = len({c for c in pal})
        if used > 16:
            fail(f"sheet {name!r} needs {used} palette entries, and four bits "
                 "an index holds 16")
    print(f"  {len(art.CPP_FRAMES)} sheets pack into 16 colour palettes")


def main():
    test_the_parts_fill_the_frame()
    test_the_head_is_half_the_character()
    test_every_part_is_the_height_it_claims()
    test_every_face_has_eyes_with_whites()
    test_the_eyes_do_not_touch()
    test_the_cast_shares_one_face()
    test_the_art_still_builds()
    print("picomon sprite style tests passed")


if __name__ == "__main__":
    main()
