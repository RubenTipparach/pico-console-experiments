# Picomon character art: the blueprint

The rules the overworld people are drawn against, and the measurements they
come from. Read this before editing `build_art.py`. Every number here is
re-derivable: `python3 measure.py` prints the per-row map and the band split
for our own sheets, and the reference numbers below carry the exact pixel rows
they were read from.

A frame is **14 x 20**, drawn at 10 pixels a tile, so a character is two tiles
tall. The height is fixed by the camera. The width is not, and it is 14
because of section 1.


## 1. The head overlaps the body, and the arms live inside the skull

This is the construction, and everything else follows from it. The parts are
**not stacked**, and the overlap is not free-form either. Two rules, both
measured off the reference:

1. **The arms live inside the skull's footprint**, in the room the jaw taper
   frees up. Brendan's skull spans 14 of his 16 columns, his jaw narrows to
   10, and his arms tuck into the two column gap either side of the jaw. His
   arms appear beside the jaw, about two rows above the chin, with the hands
   arriving at torso level.
2. **The torso is narrower than the skull.** His torso core is about 8
   columns under a 14 column skull. Get this backwards, torso wider than the
   head and arms hung outside it, and the character reads as a little head on
   a table. That was a real revision of this art.

Scaled to our frame:

```
rows  0 .. 11    head: skull 12 wide with a rounded crown, jaw 8, chin 6
rows  9 .. 15    body: arms beside the jaw on rows 9-11, torso 10 wide
                 from row 12
rows 16 .. 19    legs
```

Three rows of overlap, 9 to 11. `compose()` draws the body first and the
head over it; reversing that order paints the shirt across the chin.

The proof row, Brendan frame 0 row 18, by real palette index:

```
     0123456789012345
  18 .BJADDDDDDDDAJB.
       ^^          ^^
```

`B` #7B4141 sleeve, `J` #FFC594 arm skin, `A` head outline, `D` #FFD5B4
face: arm columns directly beside jaw columns, inside the width his skull
occupies five rows higher.

### The column plan

Shared by every head, which is what keeps four characters reading as one
cast:

```
columns  1 .. 2    arm, on the overlap rows only (11 .. 12 mirrored)
column   1         skull outline on the full width rows
column   2         hair
columns  3 .. 10   face, eight wide: cheek, eye at 5, two of cheek,
                   eye at 8, cheek, shade at 10
column   11        hair
column   12        skull outline
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
| **Picomon, current** | **0-11** | **9-15** | **16-19** | **20** | **12 (60%)** | overlaps | **4 (20%)** |

Picomon's body row range overlaps its head's by three rows, which is why its
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
argument for chibi proportions here. It is a legibility argument rather than a
stylistic one: a head drawn to realistic proportion on a 20 row figure is
about five rows, and five rows cannot hold two eyes that read as a face.

The column plan is in section 1, because it is set by where the arms have to
go. Within the face, the six columns read left to right as five lit pixels
and one shade pixel, so the light always comes from the same place.

### Eyes

**One pixel wide, two rows tall, white above pupil, two pixels apart.** That
is Brendan's eye exactly: his are a single column of #102039 at columns 6 and
9, two rows deep, with two columns of cheek between them.

A six column face is what the overlap costs, and it will not hold the 2 x 2
eyes the 12 wide version had. Two things still hold from that version and
both were learned the hard way:

- Two pixels of gap is the minimum. Under that the pair merges into **one
  dark bar** at the size the sprite is actually seen.
- The white matters. Without it the eye is a dark dot on skin and the face
  reads as blank.

`tools/tests/test_picomon_sprites.py` fails the build on either.


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
