# Picomon character art: the blueprint

The rules the overworld people are drawn against, and the measurements they
come from. Read this before editing `build_art.py`. Every number here is
re-derivable: `python3 measure.py` prints the per-row map and the band split
for our own sheets, and the reference numbers below carry the exact pixel rows
they were read from.

A frame is **14 x 20**, drawn at 10 pixels a tile, so a character is two tiles
tall. The height is fixed by the camera. The width is not, and it is 14
because of section 1.


## 1. The construction, transplanted from the reference

The parts overlap, and the whole arrangement is read off a Gen 3 overworld
sprite pixel by pixel rather than paraphrased from memory. Brendan's facts:

- His head is 13 of his 21 inked rows and **the face is its bottom third**:
  cap and hair 8 rows, face 5, eyes on the 10th and 11th rows of 13, which
  is 70 to 77 per cent down the head.
- His face (8 wide) is **narrower than his cap** (12 of 16 columns), and his
  arms hang in the difference, tucked under the hair's overhang, running
  unbroken from beside the face to the torso hem, hands at torso level.
- His torso is 5 rows. His legs are **3**.

Ours, one row shorter overall:

```
rows  0 .. 11    head: hair dome 0-6, face 7-11, eyes on rows 8 and 9
rows  7 .. 16    body: arms at the flanks, torso core 8 wide from row 12
rows 17 .. 19    legs
```

The skull is an egg, not a box. The crown widens one two-pixel step a row,
4 to 8 to 10 to 12, and the hairline is an arc: hair wraps the forehead's
top corners, so the face's first row is six wide and its eye rows eight.
That gradual widening is the reference's silhouette (boy1's skull steps
6, 10, 12, 14 down to its hairline), and it is the whole difference
between an egg and a box with rounded corners.

Five rows of overlap, 7 to 11. The arm's top rows sit **behind** the face,
which is twelve wide there, so the visible arm begins exactly where the chin
tapers, and the shoulders cannot end up at the ears no matter what the head
does. `compose()` draws the body first and the head over it; reversing that
paints the shirt across the chin.

One deviation from the reference, on purpose: Brendan's arms are bare skin,
a different tone from his face. In a fifteen colour budget the arm skin and
face skin are the same entry, and a bare arm merged with the face into one
run of skin down the side of the head. The arms are sleeved in shirt colour
instead, with a skin hand at the wrist: the same colour break his second
skin tone buys him.

### The column plan

```
columns  0 .. 1    arm: outline outside, sleeve inside
column   2         head outline at the face rows
column   3         hair beside the face
columns  4 .. 9    face interior, eyes at 5 and 8
column   10        skin shade
column   11        hair
column   12        head outline
```

The boundaries are declared, never inferred. Do not try to detect them from
the bitmap: that was attempted twice and failed twice, scoring rows by skin
puts the chin wherever the hands are, and looking for where the silhouette
splits finds the gap between the boots.

### What we have, and what the references have

| sheet | head rows | body rows | legs rows | height | head | body | legs |
|---|---|---|---|---|---|---|---|
| Pokemon Gen 3, Brendan | 10-23 | 24-27 | 28-30 | 21 | 14 (67%) | 4 (19%) | 3 (14%) |
| Pokemon Gen 3, May | 11-23 | 24-27 | 28-30 | 20 | 13 (65%) | 4 (20%) | 3 (15%) |
| RPG Maker style, boy1 | 12-22 | 23-26 | 27-30 | 19 | 11 (58%) | 4 (21%) | 4 (21%) |
| **Picomon, current** | **0-11** | **7-16** | **17-19** | **20** | **12 (60%)** | overlaps | **3 (15%)** |

Picomon's body row range overlaps its head's by five rows, which is why its
columns do not add up like the others: the references' rows were read as
disjoint bands off a ruler, and only the pixel dump showed that they are not.

The three references average **63% head**. Picomon shipped at 50% for an
afternoon, on a rule of thumb rather than a measurement, and it was out in the
direction that costs the most: at this size the head is the only part of a
character anybody reads.

**`HEAD_H 12, BODY_H 4, LEGS_H 4`**, which is 60 / 20 / 20.

That is deliberately short of the 63% average. Brendan and May spend only
three rows on legs, but they have 21 rows to spend and we have 20, and the
fourth leg row is what lets a stepping pose read at all (see section 3).
13 / 4 / 3 matches May exactly and would be right only if the walk went back
to being static.

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

Picomon had no chin outline at all: row 9 was skin and row 10 was shirt, so
the head bled into the torso with no edge between them. Every head now closes
with one, and `test_every_head_closes_with_an_outline` fails the build if one
stops doing so.


## 2. The face

Twelve rows of head is what buys a readable face, and that is the entire
argument for chibi proportions here. It is a legibility argument rather than
a stylistic one: a head drawn to realistic proportion on a 20 row figure is
about five rows, and five rows cannot hold two eyes that read as a face.

The face sits LOW: eyes on rows 8 and 9 of a 12 row head, the reference's 70
per cent, and the thing that makes a big head read as a child's head rather
than an egg with a face in the middle. The column plan is in section 1.
Within the face the light always comes from the same side: the last face
column before the hair is the shade column.

### Eyes

**One pixel wide, two rows tall, solid dark, two columns of cheek between
the pair.** That is Brendan's eye exactly: a single column of #102039, two
rows deep, on light skin. No white catchlight, and this document previously
required one; that rule was derived from our own failed art, where a 2 x 2
dark blob on a cramped face read as blank, not from the reference, which
has none. On an eight wide face with the eyes at their measured height the
solid 1 x 2 reads perfectly.

What still holds, learned the hard way: two pixels of gap is the minimum,
because under that the pair merges into one dark bar at the size the sprite
is actually seen.

`tools/tests/test_picomon_sprites.py` fails the build on a face whose eyes
are missing, merged, or not the reference's 1 x 2 construction.


## 3. The walk

Four frames a direction: stance, step, stance, the other step.

```
frame 0   legs stand    arms mid
frame 1   legs step_l   arms down
frame 2   legs stand    arms mid
frame 3   legs step_r   arms up
```

`step_l` and `step_r` are **mirror silhouettes of each other**, and that is
the whole thing. The planted foot reaches the bottom row and the trailing
foot stops one row short, so the silhouette is uneven, and an uneven
silhouette alternating left and right is a walk. At four rows there are no
hips to swing, so the step has to read through the feet.

Pair each step with the opposite arm, because a person swings the arm
opposite the leading leg.

### What this replaced

The first version of this art had three leg poses, `apart`, `stand` and
`together`, and **every one of them was left to right symmetric**. No frame
ever put one leg in front of the other: the feet spread and closed on the
spot and the character sprang up and down while sliding forward, which is the
exact tell that a sprite is being moved by code rather than walking. It
shipped that way, because a symmetric pose is a perfectly valid drawing and
nothing was looking for the asymmetry.

Pokemon Gen 3 does it the same way this file now does. Its 144 x 32 sheet is
nine frames: three stills (down, up, side) at 0 to 2, then the walking pairs,
and the two walking frames for a facing are **exact mirrors of each other**.
Checked on the sheet: frames 3 and 4 mirror, and so do 5 and 6. Frames 7 and 8
do not, because a side view cannot be mirrored without turning the character
round.

Three tests hold this: `test_the_legs_actually_step`,
`test_the_two_steps_are_mirrors` and `test_the_cycle_uses_both_steps`.

The stance appears twice in the cycle on purpose, so the Aseprite tag plays it
correctly when the file is opened rather than only looking right inside the
game. Keep that.


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
player has pupils and whites, no character's eyes touch, the four front facing
characters share one eye shape, every head closes with a chin outline, and the
legs have a stepping pose whose mirror the cycle also plays. If you change the row budget here,
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
