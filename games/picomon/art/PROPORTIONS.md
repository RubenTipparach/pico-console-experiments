# Picomon character art: the blueprint

The rules the overworld people are drawn against, and the measurements they
come from. Read this before editing `build_art.py`. Every number here is
re-derivable: `python3 measure.py` prints the per-row map and the band split
for our own sheets, and the reference numbers below carry the exact pixel rows
they were read from.

A frame is **12 x 20**, drawn at 10 pixels a tile, so a character is two tiles
tall. That is fixed by the camera and is not up for discussion here. What this
document decides is how those 20 rows are spent.


## 1. The row budget

```
rows  0 .. HEAD_H-1                head
rows  HEAD_H .. HEAD_H+BODY_H-1    body
rows  HEAD_H+BODY_H .. 19          legs
```

`compose()` stacks exactly those three parts and checks their heights, so the
boundaries are declared, never inferred. Do not try to detect them from the
bitmap. That was attempted twice and failed twice: scoring rows by skin tone
puts the chin wherever the hands are, and looking for the row where the
silhouette splits finds the gap between the boots. Anatomy is not recoverable
from a 12 pixel wide bitmap by rule.

### What we have, and what the references have

| sheet | head rows | body rows | legs rows | height | head | body | legs |
|---|---|---|---|---|---|---|---|
| Pokemon Gen 3, Brendan | 10-23 | 24-27 | 28-30 | 21 | 14 (67%) | 4 (19%) | 3 (14%) |
| Pokemon Gen 3, May | 11-23 | 24-27 | 28-30 | 20 | 13 (65%) | 4 (20%) | 3 (15%) |
| RPG Maker style, boy1 | 12-22 | 23-26 | 27-30 | 19 | 11 (58%) | 4 (21%) | 4 (21%) |
| **Picomon, current** | **0-9** | **10-14** | **15-19** | **20** | **10 (50%)** | **5 (25%)** | **5 (25%)** |

The three references average **63% head**. Picomon is at 50%, under all of
them, and it is the wrong direction to be out in: the head is where the
expression lives, and at this size the head is the only part of a character
anyone actually reads.

**Target: `HEAD_H 12, BODY_H 4, LEGS_H 4`**, which is 60 / 20 / 20.

That is short of the 63% average on purpose. Brendan and May spend only three
rows on legs, but they have 21 rows to spend and we have 20, and the fourth
leg row is what lets a stepping pose read at all (see section 3). 13 / 4 / 3
matches May exactly and is the right call only if the walk stays static.

### The outline belongs to the part it outlines

A head's last row is the dark rule under the chin, not the last row of skin.
This is not a technicality. It is a whole row out of twenty, and counting it
the other way under-measures every outlined sprite:

```
     0123456789012345
  21 ...+ssssssss+...    face
  22 ...##+ssss+##...    last row of skin
  23 ..#s########s#..    chin outline: this row is head
  24 .++s###++###s++.    shoulders: this row is body
```

Brendan, frame 0, rows 21 to 24. Row 23 is solid outline directly under the
face, and it is also where the hands first appear, at columns 3 and 12. In a
sprite this tight one row does double duty, and it reads as the bottom of the
head.

Counted without that row Brendan is 62% head; with it he is 67%. The same
correction applies to May.

**Picomon has no chin outline at all.** Row 9 is skin and row 10 is shirt, so
the head bleeds into the torso with no edge between them. Adding that row is
part of why the references read bigger than their raw row count suggests, and
it should come in with the new budget.


## 2. The face

Ten rows of head was chosen so that two eyes could fit in it and read. That is
the entire argument for chibi proportions here, and it is a legibility
argument rather than a stylistic one: a head drawn to realistic proportion on
a 20 row figure is about five rows, and five rows cannot hold a face.

Every head is built on one column plan, which is what keeps four characters
looking like one cast:

```
column  0        outline
column  1        hair
columns 2 .. 9   face, eight wide
column  10       hair
column  11       outline
```

The face reads left to right as seven lit pixels and one shade pixel, so the
light always comes from the same place.

### Eyes

A 2 x 2 pupil with a white catchlight at its top left, and **two pixels of
skin between the pair**. Both halves are load bearing, and both were wrong on
the first attempt:

- White sclera above a pupil reads as a **visor**, not as eyes.
- Eyes with fewer than two pixels between them merge into **one dark bar** at
  the size the sprite is actually seen.

`tools/tests/test_picomon_sprites.py` fails the build on either.


## 3. The walk

**Currently broken, and it is the most visible thing in this document.**

Four frames a direction, in the shape a walk should take: step, pass, step,
pass.

```
frame 0   legs apart      arms down
frame 1   legs stand      arms mid
frame 2   legs together   arms up
frame 3   legs stand      arms mid
```

All three leg poses (`apart`, `stand`, `together`) are **left to right
symmetric**. Compare each against its own reverse and they match. So no frame
puts one leg in front of the other: the feet spread and close on the spot, and
the character springs up and down while sliding forward. That is the exact
tell that a sprite is being moved by code rather than walking.

Brendan does it in three poses: stand, left lead, right lead, where the two
stepping poses are mirrors of each other.

**The fix costs nothing.** No new frames, no new flash: one stepping pose plus
its mirror replaces the symmetric pair, and `draw_sprite` already takes a flip
argument because the side view uses it.

The passing pose is stored twice on purpose, so the Aseprite tag plays the
cycle correctly when the file is opened rather than only looking right inside
the game. Keep that.


## 4. Poses

A reference sheet carries four rows: down, left, right, up. Ours does not.

| sheet | down | up | side | frames |
|---|---|---|---|---|
| hero | yes | yes | yes | 12 |
| trainer | yes | yes | mirrors front | 8 |
| villager | yes | no | no | 4 |
| healer | yes | no | no | 4 |

Mirroring the side view for left and right is correct and standard. Having no
side and no back at all is not: `render.cpp` asks for a back frame only when a
sheet has more than four, so a villager standing with their back to you looks
you in the eye.

That is survivable only because no NPC currently walks. The moment one does,
villager and healer need eight more frames between them.


## 5. Colour

Every channel is a multiple of `0x11`, because the panel is four bits a
channel. A colour that is not shifts on the device and nowhere else, which is
the worst place to find out.

A sheet is indexed at four bits, so **fifteen colours and no more**, with entry
0 transparent. The trainer sits at exactly fifteen. Adding a colour to that
character is a build failure, not a slightly worse picture.


## 6. How to check your work

```
python3 build_art.py        rebuild every sheet, .aseprite, .png and table
python3 measure.py          the per row map and the band split, per sheet
python3 verify.py           every file parses and round trips clean
python3 ../../../tools/tests/test_picomon_sprites.py
```

The test enforces what this document asserts: the parts sum to the frame, the
head is between a half and two thirds of it, every face turned toward the
player has pupils and whites, no character's eyes touch, and the four front
facing characters share one eye shape. If you change the row budget here,
change it in `build_art.py` and the test follows automatically, because it
reads `HEAD_H`, `BODY_H` and `LEGS_H` rather than hard coding them.


## Provenance

Reference boundaries were read off numbered rulers at 14x magnification, from
`brendan_walking.png` and `may_walking.png` (Pokemon Gen 3 overworld, 16 x 32)
and an RPG Maker style 16 x 32 sheet. Those files are **not committed**: they
are third party art, they are only needed to derive the numbers, and the
numbers are in the table above with the rows they came from. Our own sheets
are measured live by `measure.py`.

The symmetry result in section 3 comes from comparing each leg pose in
`build_art.py` against its own reverse.
