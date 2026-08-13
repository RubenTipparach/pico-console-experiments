#!/usr/bin/env python3
"""Draw Joker Reels' how to play diagrams, as SVG the game's page inlines.

Some rules are pictures pretending to be sentences. "The middle row, the top,
the bottom and the two diagonals" is a shape, and a paragraph is the worst way
to hand somebody a shape. "The best line sets the multiplier and every other
paying line adds its chips" is a sum, and a sum written as prose is a sum
nobody does.

tools/gen_shell.py inlines games/jokerreels/tutorial/<heading>.svg into the
panel whose rule heading matches, so adding a picture is adding a file.

Three things about this are not decoration:

**Every number is read out of sim.cpp, never typed here.** A diagram claiming
TWO PAIR is 45 chips and 4 mult is a diagram that goes wrong silently the
first time somebody rebalances the ladder: it still draws, still looks right,
and now teaches the wrong game. The hand table, the symbol chips, the joker
list and the payline rows are all parsed out of the rules and drawn from
there, so a rebalance makes the committed SVG stale and
tools/tests/test_jokerreels_art.py fails until it is redrawn.

**The worked example is scored the way the game scores.** The hands and the
total on the chips-and-mult diagram come from running the real scoring shape
over the example grid, and games/jokerreels/tests/preview.cpp puts the SAME
grid through the REAL scorer and checks it agrees. The art test binds the two
grids together. A diagram of a sum the game does not do would otherwise be
the most convincing wrong thing on the page.

**The symbols are the shipped art.** The 16x16 PNGs are embedded as data URIs
rather than redrawn, so what a player sees on the page is exactly what lands
on a drum.

Usage:
    gen_jokerreels_diagrams.py [--out games/jokerreels/tutorial]
"""

import argparse
import base64
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
GAME = os.path.join(REPO, "games", "jokerreels")

# The page's own palette, which is web/shell.html's. Text and rules use
# currentColor so they follow the panel, and only the things that mean
# something specific carry a colour of their own.
CHIP = "#5bc8ff"
MULT = "#ff4d7d"
BEST = "#ffb300"
ALSO = "#8b7fd4"
DIM = "#7a7590"

FONT = ("font-family:ui-monospace,SFMono-Regular,Menlo,Consolas,monospace;"
        "font-size:%dpx")


# ---------------------------------------------------------------------------
# What the rules say, read out of the rules
# ---------------------------------------------------------------------------

def read(*parts):
    with open(os.path.join(*parts), encoding="utf-8") as handle:
        return handle.read()


def parse_tables():
    """The hand ladder, the symbols, the jokers and the paylines, from sim.cpp.

    Regexes rather than a parser, and deliberately narrow: a table that stops
    matching raises here instead of quietly yielding an empty diagram.
    """
    src = read(GAME, "src", "sim.cpp")
    tables = {}

    block = re.search(r"k_symbols_table\[k_symbols\] = \{(.*?)\};", src, re.S)
    if not block:
        raise SystemExit("gen_jokerreels_diagrams: no k_symbols_table")
    tables["symbols"] = [(m.group(1), int(m.group(2))) for m in
                         re.finditer(r'\{"([A-Z ]+)",\s*(\d+)\}',
                                     block.group(1))]

    block = re.search(r"k_hands_table\[k_hands\] = \{(.*?)\};", src, re.S)
    if not block:
        raise SystemExit("gen_jokerreels_diagrams: no k_hands_table")
    tables["hands"] = [(m.group(1), int(m.group(2)), int(m.group(3))) for m in
                       re.finditer(r'\{"([A-Z ]+)",\s*(\d+),\s*(\d+),',
                                   block.group(1))]

    block = re.search(r"k_jokers_table\[k_jokers\] = \{(.*?)\};", src, re.S)
    if not block:
        raise SystemExit("gen_jokerreels_diagrams: no k_jokers_table")
    tables["jokers"] = [(m.group(1), m.group(2)) for m in
                        re.finditer(r'\{"([A-Z ]+)",\s*"([^"]+)",', block.group(1))]

    block = re.search(r"k_payline_rows\[k_lines\]\[k_drums\] = \{(.*?)\};",
                      src, re.S)
    names = re.search(r"k_payline_names\[k_lines\] = \{(.*?)\};", src, re.S)
    if not block or not names:
        raise SystemExit("gen_jokerreels_diagrams: no payline table")
    tables["paylines"] = [
        [int(n) for n in row.split(",") if n.strip()]
        for row in re.findall(r"\{([0-9,\s]+)\}", block.group(1))]
    tables["payline_names"] = re.findall(r'"([A-Z]+)"', names.group(1))

    slots = re.search(r"constexpr int k_max_jokers = (\d+);",
                      read(GAME, "src", "sim.hpp"))
    if not slots:
        raise SystemExit("gen_jokerreels_diagrams: no k_max_jokers")
    tables["max_jokers"] = int(slots.group(1))

    for key in ("symbols", "hands", "jokers", "paylines"):
        if not tables[key]:
            raise SystemExit("gen_jokerreels_diagrams: %s came out empty" % key)
    return tables


# ---------------------------------------------------------------------------
# The worked example
# ---------------------------------------------------------------------------
#
# Five reels by three rows, chosen so exactly two lines pay and they pay
# differently: the middle row makes three of a kind and the top row makes a
# pair, so the diagram can show the best line carrying the mult and the other
# one adding its chips. The same grid is scored by the real rules in
# games/jokerreels/tests/preview.cpp, and the art test binds the two together.
#
# Rows are top, payline, bottom. Names, not indices, so the grid can be read.
EXAMPLE = [
    ["SEVEN",  "SEVEN", "DIAMOND", "PLUM",  "BAR"],
    ["BELL",   "BELL",  "BELL",    "CROWN", "PLUM"],
    ["CLOVER", "PLUM",  "DIAMOND", "BAR",   "CHERRY"],
]


def hand_of(symbols, tables):
    """The shape five symbols make, the way sim.cpp's hand_of tests them.

    Best to worst, first fit wins, which is why a full house has to be looked
    for before three of a kind.
    """
    order = [name for name, _ in tables["symbols"]]
    index = [order.index(s) for s in symbols]
    counts = {}
    for i in index:
        counts[i] = counts.get(i, 0) + 1
    ranked = sorted(counts.values(), reverse=True)
    best_n = ranked[0]
    second_n = ranked[1] if len(ranked) > 1 else 0
    distinct = len(counts)
    is_run = distinct == len(symbols) and max(index) - min(index) == len(symbols) - 1

    if best_n == 5:
        return "FIVE OF A KIND"
    if best_n == 4:
        return "FOUR OF A KIND"
    if best_n == 3 and second_n == 2:
        return "FULL HOUSE"
    if is_run:
        return "RUN"
    if best_n == 3:
        return "THREE OF A KIND"
    if best_n == 2 and second_n == 2:
        return "TWO PAIR"
    if best_n == 2:
        return "PAIR"
    return "NOTHING"


def matching(symbols, tables):
    """Which reels took part, the way hand_groups picks them."""
    hand = hand_of(symbols, tables)
    if hand == "NOTHING":
        return []
    if hand == "RUN":
        return list(range(len(symbols)))
    counts = {}
    for s in symbols:
        counts[s] = counts.get(s, 0) + 1
    repeated = {s for s, n in counts.items() if n >= 2}
    return [i for i, s in enumerate(symbols) if s in repeated]


def score_example(tables):
    """The example's paying lines, and what the whole spin is worth.

    The shape sim.cpp's score() has: a line's chips are its hand's plus the
    chips of the symbols that actually made it, the BEST line's hand sets the
    mult, and every paying line's chips go on the same pile.
    """
    chips = dict(tables["symbols"])
    hands = {name: (c, m) for name, c, m in tables["hands"]}
    ladder = [name for name, _, _ in tables["hands"]]

    paying = []
    for line, rows in enumerate(tables["paylines"]):
        symbols = [EXAMPLE[rows[reel]][reel] for reel in range(len(rows))]
        hand = hand_of(symbols, tables)
        if hand == "NOTHING":
            continue
        total = hands[hand][0]
        for reel in matching(symbols, tables):
            total += chips[symbols[reel]]
        paying.append({"line": line,
                       "name": tables["payline_names"][line],
                       "hand": hand,
                       "chips": total,
                       "reels": matching(symbols, tables)})

    if not paying:
        raise SystemExit("gen_jokerreels_diagrams: the example pays nothing")
    paying.sort(key=lambda p: ladder.index(p["hand"]))
    best = paying[0]
    mult = hands[best["hand"]][1]
    pile = sum(p["chips"] for p in paying)
    return paying, best, mult, pile, pile * mult


# ---------------------------------------------------------------------------
# Drawing
# ---------------------------------------------------------------------------

def data_uri(path):
    with open(path, "rb") as handle:
        return "data:image/png;base64," + base64.b64encode(
            handle.read()).decode("ascii")


def svg(width, height, title, body):
    # A <title> rather than a bare role="img": these are inlined into a page a
    # screen reader walks, and a picture with no name is a picture that is not
    # there at all to anyone using one.
    return (
        '<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 %g %g" '
        'role="img" width="100%%" style="height:auto;display:block">\n'
        '<title>%s</title>\n%s\n</svg>\n'
        % (width, height, escape(title), body))


def escape(s):
    return (str(s).replace("&", "&amp;").replace("<", "&lt;")
            .replace(">", "&gt;"))


# The font is monospace, so a string's width is its length times the advance,
# and every monospace face worth naming advances 0.6 of the size. That makes
# rule 9's "measure text, never place it by eye" possible in a picture: a
# label's box is known before it is drawn, so a row can be laid out left to
# right and a column can be right aligned, rather than nudged until a
# screenshot looks right.
#
# textLength pins it rather than trusting the estimate. lengthAdjust="spacing"
# stretches the gaps between characters and never the characters, so a font
# that advances slightly differently comes out the right WIDTH with its glyphs
# untouched. The first version of this file had no measuring at all and the
# chips column printed straight through the mult column.
ADVANCE = 0.6


def adv(s, size):
    return len(s) * size * ADVANCE


def text(x, y, s, size=9, fill="currentColor", anchor="start", weight=400):
    return ('<text x="%s" y="%s" textLength="%g" lengthAdjust="spacing" '
            'style="%s;font-weight:%d" fill="%s" text-anchor="%s">%s</text>'
            % (x, y, adv(s, size), FONT % size, weight, fill, anchor,
               escape(s)))


def run(x, y, pieces, anchor="start", gap=4.0):
    """A row of differently coloured, differently sized pieces, in order.

    `pieces` is (string, size, colour, weight). With anchor="end" the whole
    run is right aligned at x, which is what puts a chips column under a chips
    column whatever the numbers in it turn out to be.

    The space between pieces is `gap` and never a space character. SVG
    collapses leading and trailing whitespace inside a <text>, so " chips"
    draws as "chips" and the run comes out as 115chips: a gap has to be
    geometry here, not typography.
    """
    width = (sum(adv(s, size) for s, size, _, _ in pieces) +
             gap * (len(pieces) - 1))
    cursor = x - width if anchor == "end" else x
    out = []
    for i, (s, size, colour, weight) in enumerate(pieces):
        if i:
            cursor += gap
        out.append(text(cursor, y, s, size, colour, "start", weight))
        cursor += adv(s, size)
    return "".join(out), width


def cell_grid(x0, y0, cell, rows, cols, on, colour):
    """One payline shape as a little grid, and the path through it."""
    out = []
    for r in range(rows):
        for c in range(cols):
            lit = on[c] == r
            out.append('<rect x="%g" y="%g" width="%g" height="%g" '
                       'fill="%s" opacity="%s"/>'
                       % (x0 + c * cell, y0 + r * cell, cell - 1.5, cell - 1.5,
                          colour if lit else "currentColor",
                          "1" if lit else "0.16"))
    points = " ".join("%g,%g" % (x0 + c * cell + (cell - 1.5) / 2,
                                 y0 + on[c] * cell + (cell - 1.5) / 2)
                      for c in range(cols))
    out.append('<polyline points="%s" fill="none" stroke="%s" '
               'stroke-width="1.2" opacity="0.85"/>' % (points, colour))
    return "".join(out)


def draw_paylines(tables):
    """The five lines, as the five shapes they are."""
    lines = tables["paylines"]
    names = tables["payline_names"]
    cell = 11.0
    cols, rows = len(lines[0]), 3
    grid_w = cols * cell
    gap = 12.0
    total = len(lines) * grid_w + (len(lines) - 1) * gap
    x0 = (320 - total) / 2
    body = []
    for i, on in enumerate(lines):
        x = x0 + i * (grid_w + gap)
        colour = BEST if i % 2 == 0 else ALSO
        body.append(cell_grid(x, 6, cell, rows, cols, on, colour))
        body.append(text(x + grid_w / 2 - 0.75, 6 + rows * cell + 11,
                         names[i], 8, DIM, "middle"))
    return svg(320, 6 + rows * cell + 16,
               "The five paylines: %s" % ", ".join(names),
               "\n".join(body))


def symbol_defs(order):
    """Every symbol once, so a grid of fifteen cells is fifteen <use> tags."""
    out = ['<defs>']
    for name in order:
        path = os.path.join(GAME, "assets", "%s.png" % name.lower())
        out.append('<image id="s-%s" width="16" height="16" href="%s" '
                   'style="image-rendering:pixelated"/>'
                   % (name.lower(), data_uri(path)))
    out.append('</defs>')
    return "".join(out)


def draw_hands(tables):
    """The example grid, with the lines that pay drawn through it."""
    paying, best, _, _, _ = score_example(tables)
    order = [name for name, _ in tables["symbols"]]
    cell = 26.0
    cols, rows = 5, 3
    x0, y0 = 14.0, 8.0

    body = [symbol_defs(order)]
    for r in range(rows):
        for c in range(cols):
            body.append('<rect x="%g" y="%g" width="%g" height="%g" '
                        'fill="currentColor" opacity="0.10"/>'
                        % (x0 + c * cell, y0 + r * cell, cell - 2, cell - 2))
            body.append('<use href="#s-%s" x="%g" y="%g" width="20" '
                        'height="20"/>'
                        % (EXAMPLE[r][c].lower(),
                           x0 + c * cell + (cell - 2 - 20) / 2,
                           y0 + r * cell + (cell - 2 - 20) / 2))

    label_x = x0 + cols * cell + 14
    for i, pay in enumerate(paying):
        rows_of = tables["paylines"][pay["line"]]
        colour = BEST if pay is best else ALSO
        points = " ".join(
            "%g,%g" % (x0 + c * cell + (cell - 2) / 2,
                       y0 + rows_of[c] * cell + (cell - 2) / 2)
            for c in range(cols))
        body.append('<polyline points="%s" fill="none" stroke="%s" '
                    'stroke-width="2"/>' % (points, colour))
        for c in pay["reels"]:
            body.append('<rect x="%g" y="%g" width="%g" height="%g" '
                        'fill="none" stroke="%s" stroke-width="2"/>'
                        % (x0 + c * cell, y0 + rows_of[c] * cell, cell - 2,
                           cell - 2, colour))
        y = y0 + 12 + i * 18
        body.append('<rect x="%g" y="%g" width="8" height="8" fill="%s"/>'
                    % (label_x, y - 7, colour))
        body.append(text(label_x + 13, y, pay["name"], 9, colour, "start", 700))
        body.append(text(label_x + 13, y + 10, pay["hand"], 8, DIM))
    return svg(320, y0 + rows * cell + 8,
               "A five by three grid where two lines pay: %s"
               % ", ".join("%s makes %s"
                           % (p["name"].lower(), p["hand"].lower())
                           for p in paying),
               "\n".join(body))


def draw_chips_and_mult(tables):
    """The sum the example makes, line by line, the way the count plays it."""
    paying, best, mult, pile, total = score_example(tables)
    body = []

    # Two right aligned columns, so the chips sit under the chips whatever the
    # hands turn out to be, and the mult column is visibly empty on the lines
    # that do not carry one.
    chips_right = 236.0
    mult_right = 314.0
    y = 14.0
    for pay in paying:
        colour = BEST if pay is best else ALSO
        body.append('<rect x="6" y="%g" width="8" height="8" fill="%s"/>'
                    % (y - 7, colour))
        body.append(text(19, y, pay["name"], 9, colour, "start", 700))
        body.append(text(19 + adv("BOTTOM", 9) + 10, y, pay["hand"], 8, DIM))
        markup, _ = run(chips_right, y,
                        [("%d" % pay["chips"], 10, CHIP, 700),
                         ("chips", 8, DIM, 400)], "end")
        body.append(markup)
        if pay is best:
            markup, _ = run(mult_right, y,
                            [("x%d" % mult, 10, MULT, 700),
                             ("mult", 8, DIM, 400)], "end")
            body.append(markup)
        y += 19

    rule_y = y - 5
    body.append('<line x1="6" y1="%g" x2="314" y2="%g" stroke="currentColor" '
                'stroke-width="1" opacity="0.3"/>' % (rule_y, rule_y))

    label_w = adv("the score ", 8)
    y = rule_y + 18
    body.append(text(6, y, "the pile", 8, DIM))
    pieces = []
    for i, pay in enumerate(paying):
        if i:
            pieces.append(("+", 10, "currentColor", 400))
        pieces.append(("%d" % pay["chips"], 10, CHIP, 700))
    pieces.append(("=", 10, "currentColor", 400))
    pieces.append(("%d" % pile, 10, CHIP, 700))
    markup, _ = run(6 + label_w, y, pieces)
    body.append(markup)

    y += 21
    body.append(text(6, y, "the score", 8, DIM))
    markup, _ = run(6 + label_w, y,
                    [("%d" % pile, 11, CHIP, 700),
                     ("x", 11, "currentColor", 400),
                     ("%d" % mult, 11, MULT, 700)], "start", 7.0)
    body.append(markup)
    body.append(text(314, y, "= %d" % total, 13, BEST, "end", 700))
    return svg(320, y + 10,
               "Two paying lines make %d chips, the best of them sets a "
               "multiplier of %d, and the spin scores %d"
               % (pile, mult, total),
               "\n".join(body))


def draw_jokers(tables):
    """Where a joker's number lands, and how many of them there are.

    Not a list of all eight with what each does. That is a screen the GAME
    has, behind B, with room to do it properly, and putting it here made this
    panel four times taller than any other. Every panel shares one grid cell
    so the block does not resize as you page, which means the tallest diagram
    sets the height of all of them: a list here bought eight rows of text at
    the cost of an empty half page under every other rule.

    What belongs here is the thing the rule is about, which is that a joker
    fires into ONE SIDE of the sum. The icons are a strip underneath, because
    the row on the console is pictures with no names and this is where a
    player first meets them.
    """
    jokers = tables["jokers"]
    sheet = data_uri(os.path.join(GAME, "assets", "jokers.png"))
    cell = 20
    n = len(jokers)
    _, _, mult, pile, total = score_example(tables)

    # The two numbers are real jokers' numbers, taken out of their own rule
    # text rather than invented, so the picture shows amounts the game can
    # actually pay.
    def first_award(unit):
        for _, effect in jokers:
            found = re.match(r"\+(\d+) %s" % unit, effect)
            if found:
                return "+" + found.group(1)
        raise SystemExit("gen_jokerreels_diagrams: no joker awards %s" % unit)

    chips_pop = first_award("CHIPS")
    mult_pop = first_award("MULT")
    # The sheet once, in defs, and eight <use> tags pointing into it. Emitting
    # the <image> per joker put the whole base64 payload in the file eight
    # times and made this the largest diagram by a factor of two, for one
    # picture.
    body = ['<defs><clipPath id="jcell"><rect x="0" y="0" width="%d" '
            'height="%d"/></clipPath>'
            '<image id="jsheet" href="%s" '
            'style="image-rendering:pixelated"/></defs>' % (cell, cell, sheet)]

    # The two pops, over the two halves of the sum they go into.
    pop_y = 6
    for x, label, colour in ((44, chips_pop, CHIP), (188, mult_pop, MULT)):
        body.append('<rect x="%d" y="%d" width="52" height="21" fill="none" '
                    'stroke="%s" stroke-width="1.5"/>' % (x, pop_y, colour))
        body.append(text(x + 26, pop_y + 15, label, 11, colour, "middle", 700))
        body.append('<path d="M%d %d L %d %d" fill="none" stroke="%s" '
                    'stroke-width="1.5" opacity="0.6"/>'
                    % (x + 26, pop_y + 21, x + 26, pop_y + 33, colour))

    # The sum they land in.
    sum_y = pop_y + 52
    body.append(text(44, sum_y - 13, "chips", 8, DIM))
    body.append(text(44, sum_y, "%d" % pile, 14, CHIP, "start", 700))
    body.append(text(126, sum_y, "x", 14, "currentColor"))
    body.append(text(188, sum_y - 13, "mult", 8, DIM))
    body.append(text(188, sum_y, "%d" % mult, 14, MULT, "start", 700))
    body.append(text(314, sum_y, "= %d" % total, 12, BEST, "end", 700))

    # And the cast, as the pictures the console row shows.
    strip_y = sum_y + 16
    pitch = 26
    x0 = (320 - (n * pitch - (pitch - cell))) / 2
    for i in range(n):
        body.append('<g transform="translate(%g,%g)" clip-path="url(#jcell)">'
                    '<use href="#jsheet" x="%d" y="0"/></g>'
                    % (x0 + i * pitch, strip_y, -i * cell))
    body.append(text(160, strip_y + cell + 12,
                     "%d of them, %d slots to hold them"
                     % (n, tables["max_jokers"]), 8, DIM, "middle"))
    return svg(320, strip_y + cell + 18,
               "A joker adds to one side of the sum: %s chips or %s mult"
               % (chips_pop, mult_pop),
               "\n".join(body))


DIAGRAMS = {
    "paylines": draw_paylines,
    "hands": draw_hands,
    "chips-and-mult": draw_chips_and_mult,
    "jokers": draw_jokers,
}


def main(argv):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", default=os.path.join(GAME, "tutorial"))
    args = parser.parse_args(argv)

    tables = parse_tables()
    os.makedirs(args.out, exist_ok=True)
    for name, draw in sorted(DIAGRAMS.items()):
        path = os.path.join(args.out, "%s.svg" % name)
        with open(path, "w", encoding="utf-8") as handle:
            handle.write(draw(tables))
        sys.stderr.write("wrote %s\n" % path)

    paying, best, mult, pile, total = score_example(tables)
    sys.stderr.write("worked example: %s, best %s, %d x %d = %d\n"
                     % (", ".join("%s %s" % (p["name"], p["hand"])
                                  for p in paying),
                        best["name"], pile, mult, total))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
