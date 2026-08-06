#!/usr/bin/env python3
"""Author the Picomon character art and export it.

Run this to regenerate everything:

    python3 build_art.py

It writes, next to itself:
  <name>.aseprite   editable source: three layers (fill, shade, outline),
                    one frame per pose, and a tag per animation
  <name>.png        horizontal sprite strip, one frame per cell
  sheets.json       frame size, frame count and tag ranges for every sheet
  sheets.js         the same strips as base64 data URIs, for pasting into
                    index.html so the mockup stays a single file

PROPORTIONS.md, next to this file, is the blueprint: the row budget, where it
came from, what the references measure, and the two things currently wrong
with this art. Read it before moving a row boundary. `measure.py` reprints its
numbers from the sheets this file emits.

The art itself lives in this file as character-per-pixel strings, which is
what makes it reviewable in a diff. Frames are 12 x 20: two tiles tall at the
overworld camera's 10 pixels per tile.

Inside those 20 rows the people are chibi, and the row budget is the whole
style: 10 rows of head, 5 of body, 5 of legs. Half the character is head,
which is what Mother 3 and the Pokemon overworld sprites do and what a
realistically proportioned figure cannot do at this size. The reason is
legibility, not cuteness: at 12 x 20 a head drawn to scale is about five rows,
and five rows cannot hold two eyes that read as a face. Ten rows can, so a
person on this screen has an expression instead of a smudge.

The eyes are what the height buys, and they are drawn to be read: two pixels
of white with two of pupil directly under them, kept two pixels apart. Both
halves matter. Without the white the eye is a dark blob against skin and the
face reads as blank; without the gap the eyes merge into one bar at a glance.
Arms are stubs, one skin pixel at each shoulder, moved a row up or down across
the walk. At six pixels of torso that is all the swing there is room for, and
it is enough, because the legs carry the walk.
"""
import argparse
import base64
import json
import os
import struct
import zlib

import aseprite

W, H = 12, 20
HERE = os.path.dirname(os.path.abspath(__file__))

# --- palettes -------------------------------------------------------------
# Every value is a multiple of 0x11 so it survives the PicoSystem's four bits
# per channel unchanged. A colour that does not is a colour that will shift on
# the device and nowhere else, which is the worst place to find out.

BASE = {
    "k": (0x22, 0x11, 0x22),   # outline
    "e": (0x22, 0x11, 0x22),   # pupil
    "i": (0xFF, 0xFF, 0xFF),   # eye white
    "s": (0xFF, 0xCC, 0x99),   # skin
    "S": (0xDD, 0xAA, 0x77),   # skin shade
    "b": (0x44, 0x33, 0x22),   # boots
    "B": (0x66, 0x55, 0x44),   # boot highlight
}

PALETTES = {
    "hero": dict(BASE, **{
        "h": (0x55, 0x33, 0x22), "H": (0x77, 0x55, 0x33),
        "c": (0xEE, 0x55, 0x77), "C": (0xAA, 0x33, 0x55), "w": (0xFF, 0xDD, 0xEE),
        "p": (0x33, 0x44, 0x88), "P": (0x22, 0x33, 0x66),
    }),
    "villager": dict(BASE, **{
        "h": (0x88, 0x66, 0x44), "H": (0xAA, 0x88, 0x55),
        "c": (0x44, 0xAA, 0xBB), "C": (0x22, 0x77, 0x88), "w": (0xDD, 0xEE, 0xFF),
        "p": (0x22, 0x77, 0x88), "P": (0x11, 0x55, 0x66),
    }),
    "trainer": dict(BASE, **{
        "s": (0xEE, 0xBB, 0x88), "S": (0xCC, 0x99, 0x66),
        "h": (0x55, 0x33, 0x11), "H": (0x77, 0x44, 0x22),
        "r": (0xCC, 0x33, 0x33), "R": (0x99, 0x22, 0x22),
        "c": (0xEE, 0xAA, 0x22), "C": (0xBB, 0x77, 0x11), "w": (0xFF, 0xEE, 0x99),
        "p": (0x55, 0x33, 0x22), "P": (0x44, 0x22, 0x11),
        "b": (0x33, 0x33, 0x22), "B": (0x55, 0x55, 0x33),
    }),
    "healer": dict(BASE, **{
        "h": (0xAA, 0xAA, 0xAA), "H": (0xCC, 0xCC, 0xCC),
        "c": (0xEE, 0xEE, 0xEE), "C": (0xBB, 0xBB, 0xBB), "w": (0xEE, 0x33, 0x55),
        "p": (0x55, 0x66, 0x77), "P": (0x33, 0x44, 0x55),
        "b": (0x33, 0x33, 0x44), "B": (0x55, 0x55, 0x66),
    }),
}

# --- parts ----------------------------------------------------------------
# Heads occupy rows 0-11, bodies rows 12-15, legs rows 16-19. compose() reads
# those splits from HEAD_H and BODY_H rather than from three magic numbers.
#
# The budget is 12 / 4 / 4, which is 60 / 20 / 20, and it is measured rather
# than chosen. PROPORTIONS.md has the working: three overworld sheets in this
# idiom average 63% head, and this game shipped 50% for an afternoon on the
# strength of a rule of thumb.
#
# Every head is built on one column plan, which is what keeps four characters
# looking like one cast: an outline column at 0 and 11, hair at 1 and 10, and
# eight columns of face between them. The face reads left to right as seven
# lit pixels and one shade pixel, so the light always comes from the same
# place.
#
# Every head also closes with a row of outline under the chin. That row is
# part of the head, not the top of the shirt, and without it the head bleeds
# into the torso with no edge between them. The references all have it and
# this art did not.

HEAD_H = 12
BODY_H = 4
LEGS_H = 4

HEAD = {
    "hero.down": [
        "....kkkk....",
        "..kkhhhhkk..",
        ".khhhhhhhhk.",
        "khhHHhhhhhhk",
        "khhhhhhhhhhk",
        "kkhhhhhhhhkk",
        "khsssssssShk",
        "khsiessieShk",
        "khseesseeShk",
        "khsssssssShk",
        ".khsssssShk.",
        "...kkkkkk...",
    ],
    # Facing away there is no face to draw, so the whole head is hair. It is
    # the same silhouette as the front, which is what stops the character
    # appearing to change size when they turn round.
    "hero.up": [
        "....kkkk....",
        "..kkhhhhkk..",
        ".khhhhhhhhk.",
        "khhHHhhhhhhk",
        "khhhhhhhhhhk",
        "khhhhhhhhhhk",
        "khhhhhhhhhhk",
        "khhhhhhhhhhk",
        "khhhhhhhhhhk",
        ".khhhhhhhhk.",
        "..khhhhhhk..",
        "...kkkkkk...",
    ],
    # In profile it is four columns of hair behind six of face. Splitting it
    # down the middle was an earlier attempt and the head read as a hair blob
    # with a skin patch stuck on it: from the side the hair is behind the
    # face, not half of it.
    "hero.side": [
        "....kkkk....",
        "..kkhhhhkk..",
        ".khhhhhhhhk.",
        "khhHHhhhhhhk",
        "khhhhhhhhhhk",
        "kkhhhhhhhhkk",
        "khhhhssssssk",
        "khhhhssiesSk",
        "khhhhsseesSk",
        "khhhhssssSSk",
        ".khhhsssssk.",
        "...kkkkkk...",
    ],
    # Longer hair: the only difference from the hero is that it stays beside
    # the cheeks a row further down, so the chin outline is wider to match.
    "villager.down": [
        "....kkkk....",
        "..kkhhhhkk..",
        ".khhhhhhhhk.",
        "khhHHhhhhhhk",
        "khhhhhhhhhhk",
        "kkhhhhhhhhkk",
        "khsssssssShk",
        "khsiessieShk",
        "khseesseeShk",
        "khsssssssShk",
        "khhsssssShhk",
        "..kkkkkkkk..",
    ],
    # The cap costs the forehead a row, so the eyes sit one row lower than
    # everyone else's. That is the point of a cap.
    "trainer.down": [
        "....kkkk....",
        "..kkrrrrkk..",
        ".krrrrrrrrk.",
        "krrRRrrrrrrk",
        "krrrrrrrrrrk",
        "kkRRRRRRRRkk",
        "khsssssssShk",
        "khsiessieShk",
        "khseesseeShk",
        "khsssssssShk",
        ".khsssssShk.",
        "...kkkkkk...",
    ],
    "trainer.up": [
        "....kkkk....",
        "..kkrrrrkk..",
        ".krrrrrrrrk.",
        "krrRRrrrrrrk",
        "krrrrrrrrrrk",
        "kkRRRRRRRRkk",
        "khhhhhhhhhhk",
        "khhhhhhhhhhk",
        "khhhhhhhhhhk",
        "khhhhhhhhhhk",
        ".khhhhhhhhk.",
        "...kkkkkk...",
    ],
    # A white cap with the same red cross the coat carries, so the nurse is
    # recognisable from the top of the head down.
    "healer.down": [
        "....kkkk....",
        "..kkcccckk..",
        ".kccwwcccck.",
        "kcccccccccck",
        "kchhhhhhhhck",
        "kkhhhhhhhhkk",
        "khsssssssShk",
        "khsiessieShk",
        "khseesseeShk",
        "khsssssssShk",
        ".khsssssShk.",
        "...kkkkkk...",
    ],
}

# Bodies are four rows under a twelve row head: a torso eight pixels wide with
# a one pixel hand at each shoulder. The three poses differ only in which row
# carries those hands, and that is the entire arm swing. On a body this short
# an arm that travelled further would read as a wing, and it does not need to,
# because the legs carry the walk now.
#
# There is no neck. The chin outline meets the shoulders directly, which is
# what makes the head sit on the body rather than float above it.
BODY = {
    "hero.front": {
        "mid": [
            ".kcccccccck.",
            "ksccccccccsk",
            ".kccwwwwcck.",
            ".kcCCCCCCck.",
        ],
        "down": [
            ".kcccccccck.",
            ".kcccccccck.",
            "kscwwwwwwcsk",
            ".kcCCCCCCck.",
        ],
        "up": [
            "ksccccccccsk",
            ".kcccccccck.",
            ".kccwwwwcck.",
            ".kcCCCCCCck.",
        ],
    },
    "hero.back": {
        "mid": [
            ".kcccccccck.",
            "ksccccccccsk",
            ".kcccccccck.",
            ".kcCCCCCCck.",
        ],
        "down": [
            ".kcccccccck.",
            ".kcccccccck.",
            "ksccccccccsk",
            ".kcCCCCCCck.",
        ],
        "up": [
            "ksccccccccsk",
            ".kcccccccck.",
            ".kcccccccck.",
            ".kcCCCCCCck.",
        ],
    },
    # In profile only the near arm is drawn and the far side of the torso
    # carries the shade colour, which is what gives a flat eight pixel block a
    # front and a back.
    "hero.side": {
        "mid": [
            ".kCccccccck.",
            ".kCcccccccsk",
            ".kCcwwwwcck.",
            ".kCCCCCCCck.",
        ],
        "down": [
            ".kCccccccck.",
            ".kCccccccck.",
            ".kCcwwwwccsk",
            ".kCCCCCCCck.",
        ],
        "up": [
            ".kCcccccccsk",
            ".kCccccccck.",
            ".kCcwwwwcck.",
            ".kCCCCCCCck.",
        ],
    },
    "villager.front": {
        "mid": [
            ".kcccccccck.",
            "ksccccccccsk",
            ".kcccwwccck.",
            ".kcCCCCCCck.",
        ],
        "down": [
            ".kcccccccck.",
            ".kcccccccck.",
            "kscwwwwwwcsk",
            ".kcCCCCCCck.",
        ],
        "up": [
            "ksccccccccsk",
            ".kcccccccck.",
            ".kcccwwccck.",
            ".kcCCCCCCck.",
        ],
    },
    # The cross keeps its three rows whatever the arms are doing: an upright
    # above the bar, the bar, an upright below. Drawn as a bar alone it reads
    # as a red stripe on a coat, which is a uniform detail and not a medical
    # cross, and the nurse stops being identifiable as the thing the building
    # is for.
    "healer.front": {
        "mid": [
            ".kcccwwccck.",
            "ksccwwwwccsk",
            ".kcccwwccck.",
            ".kcCCCCCCck.",
        ],
        "down": [
            ".kcccwwccck.",
            ".kccwwwwcck.",
            "kscccwwcccsk",
            ".kcCCCCCCck.",
        ],
        "up": [
            "kscccwwcccsk",
            ".kccwwwwcck.",
            ".kcccwwccck.",
            ".kcCCCCCCck.",
        ],
    },
}
BODY["trainer.front"] = BODY["hero.front"]
BODY["trainer.back"] = BODY["hero.back"]

# Legs, and this is where the walk actually happens.
#
# Three poses: a stance and two steps that are mirror images of each other.
# That mirroring is the whole point, and it is what the previous version of
# this file did not have. Its three poses were `apart`, `stand` and
# `together`, and every one of them was left to right symmetric, so no frame
# ever put one leg in front of the other: the feet spread and closed on the
# spot and the character sprang up and down while sliding forward.
#
# Pokemon Gen 3 solves it in four rows too, and its two walking frames for a
# facing are exact mirrors of each other. Checked, on the sheet: frames 3 and
# 4 mirror, and so do 5 and 6.
#
# The step reads through the feet rather than through the hips, because at
# four rows there are no hips. The planted foot reaches the bottom row and
# the trailing foot stops one row short, so the silhouette is uneven, and an
# uneven silhouette alternating left and right is a walk.
LEGS = {
    "trousers": {
        "stand": [
            "..kppkkppk..",
            "..kppkkppk..",
            "..kbBkkBbk..",
            "...kk..kk...",
        ],
        "step_l": [
            "..kppkkppk..",
            "..kppkkbBk..",
            "..kbBkkkk...",
            "...kk.......",
        ],
        "step_r": [
            "..kppkkppk..",
            "..kbBkkppk..",
            "...kkkkBbk..",
            ".......kk...",
        ],
    },
    "skirt": {
        "stand": [
            ".kcccccccck.",
            "kcCCCCCCCCck",
            "..kbBkkBbk..",
            "...kk..kk...",
        ],
        "step_l": [
            ".kcccccccck.",
            "kcCCCCCCCCck",
            "..kbBkkkk...",
            "...kk.......",
        ],
        "step_r": [
            ".kcccccccck.",
            "kcCCCCCCCCck",
            "...kkkkBbk..",
            ".......kk...",
        ],
    },
    "shorts": {
        "stand": [
            "..kppkkppk..",
            "..ksskkssk..",
            "..kbBkkBbk..",
            "...kk..kk...",
        ],
        "step_l": [
            "..kppkkppk..",
            "..ksskkbBk..",
            "..kbBkkkk...",
            "...kk.......",
        ],
        "step_r": [
            "..kppkkppk..",
            "..kbBkkssk..",
            "...kkkkBbk..",
            ".......kk...",
        ],
    },
}

# --- composition ----------------------------------------------------------

# A walk reads from four beats: stance, step, stance, the other step. The
# stance appears twice on purpose, so the Aseprite tag plays the cycle
# correctly when you open the file rather than only looking right in the game.
#
# The two steps are mirrors, which is what makes this a walk rather than a
# bounce. Pair each with the opposite arm, because a person swings the arm
# opposite the leading leg.
CYCLE = [("stand", "mid"), ("step_l", "down"), ("stand", "mid"), ("step_r", "up")]

SHEETS = {
    "hero": {
        "palette": "hero",
        "legs": "trousers",
        "dirs": [("down", "hero.down", "hero.front"),
                 ("up", "hero.up", "hero.back"),
                 ("side", "hero.side", "hero.side")],
    },
    "villager": {
        "palette": "villager",
        "legs": "skirt",
        "dirs": [("down", "villager.down", "villager.front")],
    },
    "trainer": {
        "palette": "trainer",
        "legs": "shorts",
        "dirs": [("down", "trainer.down", "trainer.front"),
                 ("up", "trainer.up", "trainer.back")],
    },
    "healer": {
        "palette": "healer",
        "legs": "trousers",
        "dirs": [("down", "healer.down", "healer.front")],
    },
}



# --- scenery --------------------------------------------------------------
#
# Trees are sprites, not meshes. A screenful of Route 1 is up to 87 tree tiles
# and the mesh form cost about twenty triangles each, which is more triangles
# on trees alone than the whole rest of the frame. A sprite is one blit, and
# at this camera it is also the better picture: the lens is long enough that a
# tile is 10 pixels across near the player and 8 at the far edge of the
# window, so a fixed size sprite is very nearly the right size everywhere.
# Two sizes cover the rest of the falloff, which is the same near and far
# split the meshes already used.
#
# Three shapes per size, picked by a hash of the tile, so a forest is not one
# tree stamped in a grid.

PALETTES["tree"] = {
    "k": (0x22, 0x33, 0x22),   # outline
    "d": (0x22, 0x66, 0x33),   # shadow green
    "g": (0x33, 0x88, 0x44),   # body green
    "G": (0x55, 0xAA, 0x55),   # lit green
    "l": (0x77, 0xCC, 0x66),   # highlight
    "t": (0x55, 0x33, 0x22),   # trunk shadow
    "T": (0x77, 0x55, 0x33),   # trunk
}

TREE_NEAR = [
    # pine_tall
    [
        ".......k........",
        "......kgk.......",
        "......kgk.......",
        ".....kGggk......",
        ".....kGggk......",
        "....kGGggdk.....",
        "...kdddddddk....",
        ".....klggk......",
        "....klGggdk.....",
        "....kGGggdk.....",
        "...kGGGgggdk....",
        "...kGGGgggdk....",
        "..kGGGGgggddk...",
        ".kddklGggdkddk..",
        "...klGGgggdk....",
        "..kGGGGgggddk...",
        "..kGGGGgggddk...",
        ".kGGGGGggggddk..",
        ".kGGGGGggggddk..",
        "kGGGGGGggggdddk.",
        "kddddkTTtkddddk.",
        ".....kTTtk......",
        ".....kTTtk......",
        ".....kTTtk......",
        ".....kTTtk......",
        "....kTTTttk.....",
    ],
    # pine_squat
    [
        "................",
        ".......k........",
        "......kgk.......",
        ".....kGggk......",
        ".....kGggk......",
        "....kGGggdk.....",
        "...kGGGgggdk....",
        "...kGGGgggdk....",
        "..kdddddddddk...",
        ".....klggk......",
        "....klGggdk.....",
        "...kGGGgggdk....",
        "..kGGGGgggddk...",
        "..kGGGGgggddk...",
        ".kGGGGGggggddk..",
        ".kGGGGGggggddk..",
        "kGGGGGGggggdddk.",
        "kGGGGGGggggdddk.",
        "kddddkTTtkddddk.",
        ".....kTTtk......",
        ".....kTTtk......",
        ".....kTTtk......",
        ".....kTTtk......",
        ".....kTTtk......",
        ".....kTTtk......",
        "....kTTTttk.....",
    ],
    # broadleaf
    [
        "................",
        "................",
        "......kkkkk.....",
        ".....kllllGk....",
        ".kkkkllllGGGk...",
        ".klllllGGGGGGk..",
        "klllllGGGGGGGGk.",
        "kllllGGGGGGGgggk",
        "klllGGGGGGGggggk",
        ".klGGGGGGGgggggk",
        ".kGGGGGGGgggggk.",
        ".kGGGGGGgggggk..",
        ".kGGGGGggggggk..",
        "..kGGGggggggk...",
        "...kGggggggk....",
        "....kkTTtkk.....",
        ".....kTTtk......",
        ".....kTTtk......",
        ".....kTTtk......",
        ".....kTTtk......",
        ".....kTTtk......",
        ".....kTTtk......",
        ".....kTTtk......",
        ".....kTTtk......",
        ".....kTTtk......",
        "....kTTTttk.....",
    ],
]

TREE_FAR = [
    # pine_tall
    [
        ".....k......",
        "....kgk.....",
        "....kgk.....",
        "...kGggk....",
        "...kdddk....",
        "....kgk.....",
        "...klggk....",
        "..kGGggdk...",
        "..kklggkk...",
        "..klGggdk...",
        ".kGGGgggdk..",
        ".kGGGgggdk..",
        "kGGGGgggddk.",
        "kdddkTkdddk.",
        "....kTk.....",
        "....kTk.....",
        "....kTk.....",
        "...kTTtk....",
    ],
    # pine_squat
    [
        ".....k......",
        "....kgk.....",
        "...kGggk....",
        "...kGggk....",
        "..kdddddk...",
        "....kgk.....",
        "...klggk....",
        "..kGGggdk...",
        ".kGGGgggdk..",
        "kGGGGgggddk.",
        "kGGGGgggddk.",
        "kdddkTkdddk.",
        "....kTk.....",
        "....kTk.....",
        "....kTk.....",
        "....kTk.....",
        "....kTk.....",
        "...kTTtk....",
    ],
    # broadleaf
    [
        "............",
        "............",
        "............",
        ".kkkkk......",
        ".klllGkkkkk.",
        "klllGGGGGgk.",
        ".klGGGGGgggk",
        ".kGGGGGgggk.",
        ".kGGGGgggkk.",
        "..kGGgggk...",
        "..kkkTkkk...",
        "....kTk.....",
        "....kTk.....",
        "....kTk.....",
        "....kTk.....",
        "....kTk.....",
        "....kTk.....",
        "...kTTtk....",
    ],
]

# Which frames belong to which species of tree, in the order the data
# compiler's TreeKind enum lists them. A zone says which species grows there
# and the renderer varies only within it, so an area is one kind of forest
# rather than a mixture. The two lists are checked against each other at
# compile time in render.cpp; they are in two files because one is art and
# the other is level data, and neither owns the other.
TREE_KINDS = [
    ("pine", 0, 2),          # pine_tall, pine_squat
    ("broadleaf", 2, 1),
]

SCENERY = {
    "treenear": {"palette": "tree", "w": 16, "h": 26, "frames": TREE_NEAR},
    "treefar": {"palette": "tree", "w": 12, "h": 18, "frames": TREE_FAR},
}


def check(rows, name, w=None):
    w = W if w is None else w
    for i, r in enumerate(rows):
        if len(r) != w:
            raise SystemExit(f"{name} row {i} is {len(r)} wide, expected {w}: {r!r}")


def height(rows, name, want):
    if len(rows) != want:
        raise SystemExit(f"{name} is {len(rows)} rows, expected {want}")


def compose(head, body, legs):
    """Stack the three parts into one 12 x 20 frame.

    The row counts are checked rather than assumed. A head one row short used
    to compose perfectly well, borrowing a row of body to fill the gap, and
    the only symptom was a character standing slightly lower than everyone
    else, which nothing catches and nobody spots in a 120 pixel screenshot.
    """
    check(head, "head")
    check(body, "body")
    check(legs, "legs")
    height(head, "head", HEAD_H)
    height(body, "body", BODY_H)
    height(legs, "legs", LEGS_H)
    if HEAD_H + BODY_H + LEGS_H != H:
        raise SystemExit(f"the part heights sum to {HEAD_H + BODY_H + LEGS_H}, "
                         f"not the frame height {H}")
    grid = [["."] * W for _ in range(H)]
    for top, rows in ((0, head), (HEAD_H, body), (HEAD_H + BODY_H, legs)):
        for y, row in enumerate(rows):
            for x, ch in enumerate(row):
                if ch != ".":
                    grid[top + y][x] = ch
    return grid


# Layers, bottom to top. Splitting on what a pixel is for rather than where it
# sits means recolouring a jacket or softening the outline is one layer's work.
LAYERS = ["fill", "shade", "outline"]
SHADE_CHARS = set("SCPHRdt")


def layer_of(ch):
    if ch in "ke":
        return 2
    if ch in SHADE_CHARS:
        return 1
    return 0


def rasterize(grid, pal, w=None, h=None):
    w = W if w is None else w
    h = H if h is None else h
    planes = [bytearray(w * h * 4) for _ in LAYERS]
    for y in range(h):
        for x in range(w):
            ch = grid[y][x]
            if ch == ".":
                continue
            rgb = pal.get(ch)
            if rgb is None:
                raise SystemExit(f"no palette entry for {ch!r}")
            p = planes[layer_of(ch)]
            i = (y * w + x) * 4
            p[i], p[i + 1], p[i + 2], p[i + 3] = rgb[0], rgb[1], rgb[2], 255
    return [bytes(p) for p in planes]


def flatten(planes, w=None, h=None):
    w = W if w is None else w
    h = H if h is None else h
    out = bytearray(w * h * 4)
    for p in planes:
        for i in range(0, len(p), 4):
            if p[i + 3]:
                out[i:i + 4] = p[i:i + 4]
    return bytes(out)


def write_png(path, width, height, rgba):
    def chunk(tag, data):
        return (struct.pack(">I", len(data)) + tag + data
                + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF))
    raw = b"".join(b"\0" + rgba[y * width * 4:(y + 1) * width * 4]
                   for y in range(height))
    png = (b"\x89PNG\r\n\x1a\n"
           + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0))
           + chunk(b"IDAT", zlib.compress(raw, 9))
           + chunk(b"IEND", b""))
    with open(path, "wb") as f:
        f.write(png)
    return len(png)


# The device cannot afford 32 bit pixels: 28 frames of 12 x 20 RGBA is 26 KB of
# flash for four characters. Indexed at four bits against a per sheet palette is
# 120 bytes a frame, 3.4 KB for all of it, and the art is flat colour so the
# palette costs nothing in quality.
CPP_FRAMES = {}


def emit_cpp(manifest, out_dir):
    hpp = ["// Generated by art/build_art.py. Do not edit, and do not commit.",
           "#pragma once", "", "#include <cstdint>", "", "namespace pm {", "",
           "// One sheet: a palette of at most 16 colours where entry 0 is",
           "// transparent, and frames packed two pixels to a byte.",
           "struct SpriteSheet {",
           "    const uint8_t* palette;   // 3 bytes an entry",
           "    const uint8_t* pixels;    // frame_count * (w * h / 2)",
           "    uint8_t w, h, frame_count;",
           "};", "",
           "extern const SpriteSheet k_sheets[];",
           f"constexpr int k_sheet_art_count = {len(manifest)};", ""]
    for i, name in enumerate(manifest):
        hpp.append(f"constexpr int art_{name} = {i};")
    hpp += ["",
            "// A species of tree is a run of frames in the tree sheets. The",
            "// order matches the data compiler's TreeKind, and render.cpp",
            "// static_asserts that the two agree.",
            "struct TreeKindFrames { uint8_t first, count; };",
            f"constexpr int k_tree_kind_count = {len(TREE_KINDS)};",
            "constexpr TreeKindFrames k_tree_kinds[k_tree_kind_count] = {"]
    for name, first, count in TREE_KINDS:
        hpp.append(f"    {{{first}, {count}}},   // {name}")
    hpp += ["};", "", "}  // namespace pm"]

    cpp = ["// Generated by art/build_art.py. Do not edit, and do not commit.",
           '#include "sprites.hpp"', "", "namespace pm {", ""]
    for name in manifest:
        pal, frames = CPP_FRAMES[name]
        cpp.append(f"static const uint8_t k_pal_{name}[] = {{")
        for c in pal:
            cpp.append(f"    0x{c[0]:02X}, 0x{c[1]:02X}, 0x{c[2]:02X},")
        cpp.append("};")
        cpp.append(f"static const uint8_t k_px_{name}[] = {{")
        for fr in frames:
            row = ", ".join(f"0x{b:02X}" for b in fr)
            cpp.append(f"    {row},")
        cpp.append("};")
        cpp.append("")
    cpp.append("const SpriteSheet k_sheets[] = {")
    for name, m in manifest.items():
        cpp.append(f"    {{k_pal_{name}, k_px_{name}, {m['frame_w']}, "
                   f"{m['frame_h']}, {m['frames']}}},")
    cpp.append("};")
    cpp += ["", "}  // namespace pm"]

    os.makedirs(out_dir, exist_ok=True)
    with open(os.path.join(out_dir, "sprites.hpp"), "w") as f:
        f.write("\n".join(hpp) + "\n")
    with open(os.path.join(out_dir, "sprites.cpp"), "w") as f:
        f.write("\n".join(cpp) + "\n")
    total = sum(len(fr) for _, frames in CPP_FRAMES.values() for fr in frames)
    print(f"sprites.cpp  {total} bytes of pixels, "
          f"{sum(len(p) for p, _ in CPP_FRAMES.values()) * 3} bytes of palettes")


def pack_indexed(flat_frames, name):
    """Quantise a sheet's RGBA frames to one shared 16 entry palette."""
    palette = [(0, 0, 0)]          # entry 0 is transparent
    lookup = {}
    packed = []
    for fr in flat_frames:
        out = bytearray()
        for i in range(0, len(fr), 8):     # two pixels a byte
            def index(off):
                a = fr[i + off + 3]
                if not a:
                    return 0
                key = (fr[i + off], fr[i + off + 1], fr[i + off + 2])
                if key not in lookup:
                    if len(palette) >= 16:
                        raise SystemExit(f"{name}: more than 15 colours")
                    lookup[key] = len(palette)
                    palette.append(key)
                return lookup[key]
            out.append((index(0) << 4) | index(4))
        packed.append(bytes(out))
    while len(palette) < 16:
        palette.append((0, 0, 0))
    return palette, packed


def main():
    # The device tables go wherever the build asks for them, because nothing
    # generated is ever committed. The .aseprite and .png files are art and
    # stay here; rewriting them is harmless because the output is byte for
    # byte deterministic.
    ap = argparse.ArgumentParser()
    ap.add_argument("--out-dir", default=HERE)
    args = ap.parse_args()
    manifest = {}
    js = {}
    for name, spec in SHEETS.items():
        pal = PALETTES[spec["palette"]]
        legs = LEGS[spec["legs"]]
        frames, tags, flat = [], [], []
        for dir_name, head_key, body_key in spec["dirs"]:
            first = len(frames)
            for leg_pose, arm_pose in CYCLE:
                grid = compose(HEAD[head_key], BODY[body_key][arm_pose],
                               legs[leg_pose])
                planes = rasterize(grid, pal)
                frames.append(planes)
                flat.append(flatten(planes))
            tags.append((dir_name, first, len(frames) - 1))

        ase = os.path.join(HERE, f"{name}.aseprite")
        size = aseprite.write(ase, W, H, LAYERS, frames, tags)

        strip = bytearray(W * len(flat) * H * 4)
        for fi, fr in enumerate(flat):
            for y in range(H):
                src = fr[y * W * 4:(y + 1) * W * 4]
                dst = (y * W * len(flat) + fi * W) * 4
                strip[dst:dst + W * 4] = src
        png = os.path.join(HERE, f"{name}.png")
        pngsize = write_png(png, W * len(flat), H, bytes(strip))

        manifest[name] = {
            "frame_w": W, "frame_h": H, "frames": len(flat),
            "tags": {t[0]: [t[1], t[2]] for t in tags},
        }
        with open(png, "rb") as f:
            js[name] = "data:image/png;base64," + base64.b64encode(f.read()).decode()
        CPP_FRAMES[name] = pack_indexed(flat, name)
        print(f"{name:9s} {len(flat):2d} frames  {name}.aseprite {size:5d} B"
              f"  {name}.png {pngsize:4d} B")

    # Scenery: no walk cycle, no head and body and legs, just a grid at its
    # own size. One frame per shape, and the game picks between them by
    # hashing the tile it is standing a tree on.
    for name, spec in SCENERY.items():
        pal = PALETTES[spec["palette"]]
        w, h = spec["w"], spec["h"]
        frames, flat, tags = [], [], []
        for i, rows in enumerate(spec["frames"]):
            check(rows, f"{name} frame {i}", w)
            if len(rows) != h:
                raise SystemExit(f"{name} frame {i} is {len(rows)} tall, "
                                 f"expected {h}")
            grid = [list(r) for r in rows]
            planes = rasterize(grid, pal, w, h)
            frames.append(planes)
            flat.append(flatten(planes, w, h))
        tags.append(("shapes", 0, len(frames) - 1))

        ase = os.path.join(HERE, f"{name}.aseprite")
        size = aseprite.write(ase, w, h, LAYERS, frames, tags)

        strip = bytearray(w * len(flat) * h * 4)
        for fi, fr in enumerate(flat):
            for y in range(h):
                src = fr[y * w * 4:(y + 1) * w * 4]
                dst = (y * w * len(flat) + fi * w) * 4
                strip[dst:dst + w * 4] = src
        png = os.path.join(HERE, f"{name}.png")
        pngsize = write_png(png, w * len(flat), h, bytes(strip))

        manifest[name] = {
            "frame_w": w, "frame_h": h, "frames": len(flat),
            "tags": {t[0]: [t[1], t[2]] for t in tags},
        }
        with open(png, "rb") as f:
            js[name] = "data:image/png;base64," + base64.b64encode(f.read()).decode()
        CPP_FRAMES[name] = pack_indexed(flat, name)
        print(f"{name:9s} {len(flat):2d} frames  {name}.aseprite {size:5d} B"
              f"  {name}.png {pngsize:4d} B")

    emit_cpp(manifest, args.out_dir)
    with open(os.path.join(HERE, "sheets.json"), "w") as f:
        json.dump(manifest, f, indent=2)
        f.write("\n")
    with open(os.path.join(HERE, "sheets.js"), "w") as f:
        f.write("// Generated by build_art.py. Paste into index.html so the\n"
                "// mockup stays one self contained file.\n")
        f.write("const SHEETS = " + json.dumps(
            {k: dict(manifest[k], src=js[k]) for k in manifest}, indent=1) + ";\n")
    print("wrote sheets.json and sheets.js")


if __name__ == "__main__":
    main()
