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
what makes it reviewable in a diff. Frames are 14 x 20: two tiles tall at the
overworld camera's 10 pixels per tile.

The construction is transplanted from a Gen 3 overworld sprite and is
documented in PROPORTIONS.md: an egg shaped skull whose face is the bottom
third with the eyes at 70 per cent, solid 1 x 2 dark eyes, arms that flank
the torso with their tops hidden behind the face, three row legs, and a walk
whose two stepping frames are mirror silhouettes.
"""
import argparse
import base64
import json
import os
import struct
import zlib

import aseprite

HERE = os.path.dirname(os.path.abspath(__file__))

# --- palettes -------------------------------------------------------------
# Every value is a multiple of 0x11 so it survives the PicoSystem's four bits
# per channel unchanged. A colour that does not is a colour that will shift on
# the device and nowhere else, which is the worst place to find out.

BASE = {
    "k": (0x22, 0x11, 0x22),   # outline
    "e": (0x22, 0x11, 0x22),   # eye, solid dark, the reference has no white
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
# A frame is 14 x 20, and the construction is transplanted from a Gen 3
# overworld sprite read pixel by pixel, not paraphrased from memory. The
# facts, off Brendan's own sheet:
#
#   - His head is 13 of his 21 inked rows and the FACE IS ITS BOTTOM THIRD:
#     cap and hair 8 rows, face 5, eyes on the 10th and 11th rows of 13,
#     which is 70 to 77 per cent down the head.
#   - His eyes are one pixel wide, two rows tall, SOLID DARK. No catchlight.
#     Two columns of cheek between the pair, two more each side.
#   - His face (8 wide) is narrower than his cap (12), and his arms hang in
#     the difference, tucked under the hair's overhang, running UNBROKEN
#     from beside the face to the torso hem, hands at torso level.
#   - His torso is 5 rows and his legs are 3.
#
# Ours, one row shorter overall:
#
#   rows  0 .. 12    head: rows 0-1 empty, hair dome 2-7, face 8-12, eyes
#                    on 9 and 10. The empty rows are the point: the chin is
#                    pinned at 12 by the body construction, so a shorter
#                    head loses rows off the top, and the figure stands 18
#                    inked rows in the 20 row frame.
#   rows  8 .. 16    body: arms beside the face and down the flanks, four
#                    torso rows from 13, May's own count
#   rows 17 .. 19    legs
#
# Five rows of overlap, 7 to 11: the arm columns (0-1 and 12-13) are outside
# the face block (2-11), under the dome's overhang, exactly the reference's
# arrangement. compose() draws the body first and the head over it.
#
# The column plan, shared by every head so the cast reads as one cast:
#
#   columns  0 .. 1    arm: outline outside, bare skin inside
#   column   2         head outline at the face rows
#   column   3         hair beside the face
#   columns  4 .. 9    face interior, eyes at 5 and 8
#   column   10        skin shade
#   column   11        hair
#   column   12        head outline
#
# Eyes: one pixel wide, two rows tall, solid dark, two of cheek between.
# An earlier revision gave them white catchlights and called them mandatory;
# that rule came from this file's own failed art, not from the reference,
# which has none.

W, H = 14, 20
HEAD_H = 13
BODY_TOP = 8
BODY_H = 9
LEGS_TOP = 17
LEGS_H = 3

HEAD = {
    "hero.down": [
        "..............",
        "..............",
        ".....kkkk.....",
        "...kkhhhhkk...",
        "..khhhhhhhhk..",
        ".khHHhhhhhhhk.",
        ".khhhhhhhhhhk.",
        ".khhhhhhhhhhk.",
        ".khhsssssShhk.",
        ".khssessesShk.",
        ".khssessesShk.",
        "...ksssssSk...",
        "....kkkkkk....",
    ],
    # Facing away is all hair on the same egg, so the character does
    # not change size when they turn round.
    "hero.up": [
        "..............",
        "..............",
        ".....kkkk.....",
        "...kkhhhhkk...",
        "..khhhhhhhhk..",
        ".khHHhhhhhhhk.",
        ".khhhhhhhhhhk.",
        ".khhhhhhhhhhk.",
        ".khhhhhhhhhhk.",
        ".khhhhhhhhhhk.",
        ".khhhhhhhhhhk.",
        "...khhhhhhk...",
        "....kkkkkk....",
    ],
    # In profile the hair is behind the face, not half of it.
    "hero.side": [
        "..............",
        "..............",
        ".....kkkk.....",
        "...kkhhhhkk...",
        "..khhhhhhhhk..",
        ".khHHhhhhhhhk.",
        ".khhhhhhhhhhk.",
        ".khhhhhhhhhhk.",
        ".khhhhssssSk..",
        ".khhhhssesSk..",
        ".khhhhssesSk..",
        "...khhsssSk...",
        "....kkkkkk....",
    ],
    # Longer hair: it frames the jaw, so the jaw row keeps a hair
    # column the others do not.
    "villager.down": [
        "..............",
        "..............",
        ".....kkkk.....",
        "...kkhhhhkk...",
        "..khhhhhhhhk..",
        ".khHHhhhhhhhk.",
        ".khhhhhhhhhhk.",
        ".khhhhhhhhhhk.",
        ".khhsssssShhk.",
        ".khssessesShk.",
        ".khssessesShk.",
        "...khsssShk...",
        "....kkkkkk....",
    ],
    # The cap follows the same egg. Its brim costs the forehead a row,
    # and the eyes stay on the same rows as everyone else's.
    "trainer.down": [
        "..............",
        "..............",
        ".....kkkk.....",
        "...kkrrrrkk...",
        "..krrRRrrrrk..",
        ".kkRRRRRRRRkk.",
        ".khsssssssShk.",
        ".khsssssssShk.",
        ".khsssssssShk.",
        ".khssessesShk.",
        ".khssessesShk.",
        "...ksssssSk...",
        "....kkkkkk....",
    ],
    "trainer.up": [
        "..............",
        "..............",
        ".....kkkk.....",
        "...kkrrrrkk...",
        "..krrRRrrrrk..",
        ".kkrrrrrrrrkk.",
        ".khhhhhhhhhhk.",
        ".khhhhhhhhhhk.",
        ".khhhhhhhhhhk.",
        ".khhhhhhhhhhk.",
        ".khhhhhhhhhhk.",
        "...khhhhhhk...",
        "....kkkkkk....",
    ],
    # A white cap with the same red cross the coat carries, so the nurse
    # is recognisable from the top of the head down.
    "healer.down": [
        "..............",
        "..............",
        ".....kkkk.....",
        "...kkcccckk...",
        "..kccwwccck...",
        ".kcccccccccck.",
        ".kkhhhhhhhhkk.",
        ".khsssssssShk.",
        ".khsssssssShk.",
        ".khssessesShk.",
        ".khssessesShk.",
        "...ksssssSk...",
        "....kkkkkk....",
    ],
}

# Bodies run rows 8 to 16: four rows of arm behind the face, the connector
# at the chin, then a four row torso, May's own count. The arm is a sleeved
# strip at the flanks with a skin hand at the wrist; its top rows sit BEHIND
# the face, so the visible arm begins exactly where the chin tapers and
# shoulders at the ears are structurally impossible.
BODY = {
    "hero.front": {
        "mid": [
            ".kc........ck.",
            ".kc........ck.",
            ".kc........ck.",
            ".kc........ck.",
            ".kck......kck.",
            ".kckcccccckck.",
            ".kskcwwwwcksk.",
            "..kkcccccckk..",
            "...kCCCCCCk...",
        ],
        "down": [
            ".kc........ck.",
            ".kc........ck.",
            ".kc........ck.",
            ".kc........ck.",
            ".kck......kck.",
            ".kckcccccckck.",
            ".kckcwwwwckck.",
            ".kskccccccksk.",
            "..kkCCCCCCkk..",
        ],
        "up": [
            ".kc........ck.",
            ".kc........ck.",
            ".kc........ck.",
            ".kc........ck.",
            ".kck......kck.",
            ".kskccccccksk.",
            "..kkcwwwwckk..",
            "...kcccccck...",
            "...kCCCCCCk...",
        ],
    },
    "hero.back": {
        "mid": [
            ".kc........ck.",
            ".kc........ck.",
            ".kc........ck.",
            ".kc........ck.",
            ".kck......kck.",
            ".kckcccccckck.",
            ".kskccccccksk.",
            "..kkcccccckk..",
            "...kCCCCCCk...",
        ],
        "down": [
            ".kc........ck.",
            ".kc........ck.",
            ".kc........ck.",
            ".kc........ck.",
            ".kck......kck.",
            ".kckcccccckck.",
            ".kckcccccckck.",
            ".kskccccccksk.",
            "..kkCCCCCCkk..",
        ],
        "up": [
            ".kc........ck.",
            ".kc........ck.",
            ".kc........ck.",
            ".kc........ck.",
            ".kck......kck.",
            ".kskccccccksk.",
            "..kkcccccckk..",
            "...kcccccck...",
            "...kCCCCCCk...",
        ],
    },
    # In profile only the near arm shows, and the far side of the torso
    # carries the shade colour, which gives a flat block a front and a back.
    "hero.side": {
        "mid": [
            "...........ck.",
            "...........ck.",
            "...........ck.",
            "...........ck.",
            "..........kck.",
            "..kCcccccckck.",
            "..kCcwwwwcksk.",
            "..kkcccccckk..",
            "...kCCCCCCk...",
        ],
        "down": [
            "...........ck.",
            "...........ck.",
            "...........ck.",
            "...........ck.",
            "..........kck.",
            "..kCcccccckck.",
            "..kCcwwwwckck.",
            "..kCccccccksk.",
            "...kCCCCCCkk..",
        ],
        "up": [
            "...........ck.",
            "...........ck.",
            "...........ck.",
            "...........ck.",
            "..........kck.",
            "..kCccccccksk.",
            "..kCcwwwwckk..",
            "..kCcccccck...",
            "...kCCCCCCk...",
        ],
    },
    "villager.front": {
        "mid": [
            ".kc........ck.",
            ".kc........ck.",
            ".kc........ck.",
            ".kc........ck.",
            ".kck......kck.",
            ".kckcccccckck.",
            ".kskcwwwwcksk.",
            "..kkcccccckk..",
            "...kCCCCCCk...",
        ],
        "down": [
            ".kc........ck.",
            ".kc........ck.",
            ".kc........ck.",
            ".kc........ck.",
            ".kck......kck.",
            ".kckcccccckck.",
            ".kckcwwwwckck.",
            ".kskccccccksk.",
            "..kkCCCCCCkk..",
        ],
        "up": [
            ".kc........ck.",
            ".kc........ck.",
            ".kc........ck.",
            ".kc........ck.",
            ".kck......kck.",
            ".kskccccccksk.",
            "..kkcwwwwckk..",
            "...kcccccck...",
            "...kCCCCCCk...",
        ],
    },
    # The cross keeps its plus shape in every pose: upright, bar, upright.
    "healer.front": {
        "mid": [
            ".kc........ck.",
            ".kc........ck.",
            ".kc........ck.",
            ".kc........ck.",
            ".kck......kck.",
            ".kckccwwcckck.",
            ".kskwwwwwwksk.",
            "..kkccwwcckk..",
            "...kCCCCCCk...",
        ],
        "down": [
            ".kc........ck.",
            ".kc........ck.",
            ".kc........ck.",
            ".kc........ck.",
            ".kck......kck.",
            ".kckccwwcckck.",
            ".kckwwwwwwkck.",
            ".kskccwwccksk.",
            "..kkCCCCCCkk..",
        ],
        "up": [
            ".kc........ck.",
            ".kc........ck.",
            ".kc........ck.",
            ".kc........ck.",
            ".kck......kck.",
            ".kskccwwccksk.",
            "..kkwwwwwwkk..",
            "...kccwwcck...",
            "...kCCCCCCk...",
        ],
    },
}
BODY["trainer.front"] = BODY["hero.front"]
BODY["trainer.back"] = BODY["hero.back"]

# Legs: three rows, the reference's count. A stance and two steps that are
# mirror silhouettes; the planted foot reaches the bottom row, the trailing
# foot stops short, and the uneven silhouette alternating sides is the walk.
LEGS = {
    "trousers": {
        "stand": [
            "...kppkkppk...",
            "...kbBkkBbk...",
            "....kk..kk....",
        ],
        "step_l": [
            "...kppkkbBk...",
            "...kbBkkkk....",
            "....kk........",
        ],
        "step_r": [
            "...kbBkkppk...",
            "....kkkkBbk...",
            "........kk....",
        ],
    },
    "skirt": {
        "stand": [
            "..kcccccccck..",
            "...kbBkkBbk...",
            "....kk..kk....",
        ],
        "step_l": [
            "..kcccccccck..",
            "...kbBkkkk....",
            "....kk........",
        ],
        "step_r": [
            "..kcccccccck..",
            "....kkkkBbk...",
            "........kk....",
        ],
    },
    "shorts": {
        "stand": [
            "...ksskkssk...",
            "...kbBkkBbk...",
            "....kk..kk....",
        ],
        "step_l": [
            "...ksskkbBk...",
            "...kbBkkkk....",
            "....kk........",
        ],
        "step_r": [
            "...kbBkkssk...",
            "....kkkkBbk...",
            "........kk....",
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

# Tall grass, straight off the design mockup rather than reinvented. The tuft
# is mockups/picomon/index.html's TUFT, character for character, on its
# PAL_GRASS: the game and the mockup should be looking at the same field.
#
# The mockup's rows are ten and eleven characters, padded to eleven, and one
# of them carries a space where a gap was meant. Padding and space are both
# transparent there, so they are dots here.
#
# One frame. A pressed down second frame existed for a while, for a parting
# that cleared a ring around the player; the reference does the opposite,
# the player wades and the grass draws over them, so the tuft always
# stands.
PALETTES["grass"] = {
    "d": (0x22, 0x66, 0x33),   # shadow green
    "m": (0x33, 0x88, 0x44),   # body green
    "l": (0x55, 0xAA, 0x55),   # lit green
}

GRASS_TUFT = [
    [
        "..d....d...",
        ".dm...dm...",
        ".dm..dmm.d.",
        "dmm.dmml.dm",
        "dmlldmmlldm",
        "dmlldmllldm",
        "ddmmddmmddm",
        "..ddd..dd..",
    ],
]

SCENERY = {
    "treenear": {"palette": "tree", "w": 16, "h": 26, "frames": TREE_NEAR},
    "treefar": {"palette": "tree", "w": 12, "h": 18, "frames": TREE_FAR},
    "grass": {"palette": "grass", "w": 11, "h": 8, "frames": GRASS_TUFT},
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
    """Lay the three parts into one frame, body first and head over it.

    The order is the construction. The body starts at BODY_TOP, which is
    above the bottom of the head, so its top rows are behind the face and
    only its outer columns show: that is the arm beside the jaw. Draw the
    head first instead and the body paints over the chin.

    The row counts are checked rather than assumed. A head one row short
    composes perfectly well by borrowing a row from whatever is under it, and
    the only symptom is a character standing slightly lower than everyone
    else, which nothing catches and nobody spots in a 120 pixel screenshot.
    """
    check(head, "head")
    check(body, "body")
    check(legs, "legs")
    height(head, "head", HEAD_H)
    height(body, "body", BODY_H)
    height(legs, "legs", LEGS_H)
    if BODY_TOP + BODY_H != LEGS_TOP:
        raise SystemExit(f"the body ends at {BODY_TOP + BODY_H} and the legs "
                         f"start at {LEGS_TOP}, so there is a gap or an "
                         "overlap between them")
    if LEGS_TOP + LEGS_H != H:
        raise SystemExit(f"the legs end at {LEGS_TOP + LEGS_H}, not at the "
                         f"frame height {H}")
    if HEAD_H <= BODY_TOP:
        raise SystemExit(f"the head ends at {HEAD_H} and the body starts at "
                         f"{BODY_TOP}, so they never overlap and the arms "
                         "cannot come up beside the face")
    grid = [["."] * W for _ in range(H)]
    for top, rows in ((BODY_TOP, body), (0, head), (LEGS_TOP, legs)):
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
