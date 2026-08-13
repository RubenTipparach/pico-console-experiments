#!/usr/bin/env python3
"""One symbol order, spelled in four places, checked to be the same order.

A drum face's texture is chosen by index: sim.hpp's Symbol enum says CHERRY is
0, render.cpp turns that into texture index 1, that resolves against the table
built from textures.cmake's list, and the pictures come from
gen_jokerreels_symbols.py's ORDER.

Nothing in the build notices if those disagree. Every symbol still has a
picture, every picture is still a symbol, and the game compiles, boots and
plays: a CHERRY simply shows a BELL and is scored as a CHERRY. That is the
worst shape a bug can have, so it is checked here rather than left to somebody
noticing a plum was worth 75.

Usage:
    test_jokerreels_art.py
"""

import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
TOOLS = os.path.dirname(HERE)
REPO_ROOT = os.path.dirname(TOOLS)
GAME = os.path.join(REPO_ROOT, "games", "jokerreels")

sys.path.insert(0, TOOLS)

import gen_jokerreels_diagrams as diagrams  # noqa: E402
import gen_jokerreels_jokers as jokers  # noqa: E402
import gen_jokerreels_symbols as gen  # noqa: E402

failures = []


def check(ok, what):
    if not ok:
        failures.append(what)
        print("FAIL %s" % what)


def read(*parts):
    with open(os.path.join(*parts), encoding="utf-8") as handle:
        return handle.read()


# 1. The generator's own order, which is what the PNGs are drawn from.
order = list(gen.ORDER)
check(len(order) == 8, "eight symbols")

# 2. sim.hpp's enum. This is the numbering the rules and the renderer share.
enum_src = re.search(r"enum Symbol : uint8_t \{(.*?)\};",
                     read(GAME, "src", "sim.hpp"), re.S)
check(enum_src is not None, "sim.hpp has a Symbol enum")
if enum_src:
    names = re.findall(r"k([A-Z][A-Za-z]*)", enum_src.group(1))
    enum_order = [n.lower() for n in names]
    check(enum_order == order,
          "sim.hpp's Symbol enum is in the generator's order:\n"
          "      enum: %s\n generator: %s" % (enum_order, order))

# 3. textures.cmake, which is what the build compiles and in what order.
cmake_src = re.search(r"set\(jokerreels_texture_files(.*?)\)",
                      read(GAME, "textures.cmake"), re.S)
check(cmake_src is not None, "textures.cmake lists the textures")
if cmake_src:
    listed = [os.path.splitext(p)[0]
              for p in cmake_src.group(1).split() if p.endswith(".png")]
    check(listed == order,
          "textures.cmake is in the generator's order:\n"
          "     cmake: %s\n generator: %s" % (listed, order))

# 4. render.cpp's table, which is the array those indices actually land in.
table_src = re.search(
    r"const pse::Texture k_textures\[jr::k_symbols\] = \{(.*?)\};",
    read(GAME, "src", "render.cpp"), re.S)
check(table_src is not None, "render.cpp has a texture table")
if table_src:
    used = re.findall(r"models::jokerreels::(\w+)", table_src.group(1))
    check(used == order,
          "render.cpp's texture table is in the generator's order:\n"
          "    render: %s\n generator: %s" % (used, order))

# 5. The PNGs exist and are the size the rasterizer needs.
for name in order:
    path = os.path.join(GAME, "assets", "%s.png" % name)
    check(os.path.isfile(path), "%s.png is committed" % name)
    if not os.path.isfile(path):
        continue
    with open(path, "rb") as handle:
        data = handle.read()
    width = int.from_bytes(data[16:20], "big")
    height = int.from_bytes(data[20:24], "big")
    check(width == gen.SIZE and height == gen.SIZE,
          "%s.png is %dx%d, and pse::Texture needs a power of two"
          % (name, width, height))

# 6. The committed PNG is still what the generator would draw. A picture that
#    has drifted from its source is one nobody can regenerate, and the drift
#    is invisible: it is a symbol that looks slightly wrong to nobody.
for name in order:
    path = os.path.join(GAME, "assets", "%s.png" % name)
    if not os.path.isfile(path):
        continue
    scratch = path + ".check"
    gen.write_png(scratch, gen.render(gen.ART[name]))
    with open(path, "rb") as a, open(scratch, "rb") as b:
        same = a.read() == b.read()
    os.remove(scratch)
    check(same, "%s.png is what gen_jokerreels_symbols.py draws today" % name)

# 7. And the art still passes the generator's own rules, so a symbol that grew
#    into its outline margin fails here rather than losing its outline on one
#    side quietly.
for problem in gen.validate():
    check(False, "symbol art: %s" % problem)


# ---------------------------------------------------------------------------
# The jokers, which have the same problem in a smaller shape.
#
# One sheet rather than eight files, so there is no cmake list and no table in
# render.cpp to keep in order: render.cpp asks for cell `joker * 20` and the
# cell IS the enum value. That leaves exactly one seam, the generator's ORDER
# against sim.hpp's Joker enum, and it fails the same way the symbols would.
# A player would see GREASER's oil can sitting in RATCHET's slot, shaking when
# RATCHET fired, and everything would still add up.
# ---------------------------------------------------------------------------

joker_order = list(jokers.ORDER)
check(len(joker_order) == 8, "eight jokers")

joker_enum = re.search(r"enum Joker : uint8_t \{(.*?)\};",
                       read(GAME, "src", "sim.hpp"), re.S)
check(joker_enum is not None, "sim.hpp has a Joker enum")
if joker_enum:
    names = re.findall(r"k([A-Z][A-Za-z]*)", joker_enum.group(1))
    enum_order = [n.lower() for n in names]
    check(enum_order == joker_order,
          "sim.hpp's Joker enum is in the generator's order:\n"
          "      enum: %s\n generator: %s" % (enum_order, joker_order))

sheet_path = os.path.join(GAME, "assets", "jokers.png")
check(os.path.isfile(sheet_path), "jokers.png is committed")
if os.path.isfile(sheet_path):
    with open(sheet_path, "rb") as handle:
        data = handle.read()
    width = int.from_bytes(data[16:20], "big")
    height = int.from_bytes(data[20:24], "big")
    colour_type = data[25]
    check(width == jokers.CELL * len(joker_order) and height == jokers.CELL,
          "jokers.png is %d cells of %d, not %dx%d"
          % (len(joker_order), jokers.CELL, width, height))
    # Colour type 6 is RGBA. pse::Sprite carries alpha as a mask and these are
    # drawn over a dark panel, a lit card and a black end screen, so an opaque
    # sheet would put a white box round every icon on all three.
    check(colour_type == 6, "jokers.png carries alpha (colour type %d)"
          % colour_type)

    scratch = sheet_path + ".check"
    jokers.write_png(scratch, jokers.sheet())
    with open(sheet_path, "rb") as a, open(scratch, "rb") as b:
        same = a.read() == b.read()
    os.remove(scratch)
    check(same, "jokers.png is what gen_jokerreels_jokers.py draws today")

for problem in jokers.validate():
    check(False, "joker art: %s" % problem)


# ---------------------------------------------------------------------------
# The other three sheets, each with the same seam and the same failure.
#
# items.png is indexed by sim.hpp's Item enum and hands.png by its Hand enum,
# so a sheet drawn in a different order puts LUCKY COIN's picture on SPARE
# SPIN's card, at SPARE SPIN's price, doing SPARE SPIN's job. Everything still
# adds up and the shop is simply lying about what it sells.
#
# extras.png has no enum: the two cells are render.cpp's own Extra, so that is
# what it is checked against.
# ---------------------------------------------------------------------------

def enum_order(name, source):
    block = re.search(r"enum %s : uint8_t \{(.*?)\};" % name, source, re.S)
    check(block is not None, "sim.hpp has a %s enum" % name)
    if not block:
        return []
    return [n.lower() for n in re.findall(r"k([A-Z][A-Za-z]*)", block.group(1))]


sim_hpp = read(GAME, "src", "sim.hpp")
render_cpp = read(GAME, "src", "render.cpp")

check(enum_order("Item", sim_hpp) == list(jokers.ITEM_ORDER),
      "sim.hpp's Item enum is in the generator's order:\n"
      "      enum: %s\n generator: %s"
      % (enum_order("Item", sim_hpp), jokers.ITEM_ORDER))

check(enum_order("Hand", sim_hpp) == list(jokers.HAND_ORDER),
      "sim.hpp's Hand enum is in the generator's order:\n"
      "      enum: %s\n generator: %s"
      % (enum_order("Hand", sim_hpp), jokers.HAND_ORDER))

extras_enum = re.search(r"enum Extra : uint8_t \{(.*?)\};", render_cpp, re.S)
check(extras_enum is not None, "render.cpp names the extras")
if extras_enum:
    found = [n.lower() for n in
             re.findall(r"kExtra([A-Z][A-Za-z]*)", extras_enum.group(1))]
    check(found == list(jokers.EXTRA_ORDER),
          "render.cpp's Extra order is the generator's:\n"
          "    render: %s\n generator: %s" % (found, jokers.EXTRA_ORDER))

for sheet_name, order, pixels in (
        ("items", jokers.ITEM_ORDER, jokers.sheet(jokers.ITEM_ORDER)),
        ("extras", jokers.EXTRA_ORDER, jokers.sheet(jokers.EXTRA_ORDER)),
        ("hands", jokers.HAND_ORDER, jokers.hand_sheet())):
    path = os.path.join(GAME, "assets", "%s.png" % sheet_name)
    check(os.path.isfile(path), "%s.png is committed" % sheet_name)
    if not os.path.isfile(path):
        continue
    with open(path, "rb") as handle:
        data = handle.read()
    width = int.from_bytes(data[16:20], "big")
    height = int.from_bytes(data[20:24], "big")
    check(width == jokers.CELL * len(order) and height == jokers.CELL,
          "%s.png is %d cells of %d, not %dx%d"
          % (sheet_name, len(order), jokers.CELL, width, height))
    check(data[25] == 6, "%s.png carries alpha" % sheet_name)

    scratch = path + ".check"
    jokers.write_png(scratch, pixels)
    with open(path, "rb") as a_f, open(scratch, "rb") as b_f:
        same = a_f.read() == b_f.read()
    os.remove(scratch)
    check(same, "%s.png is what gen_jokerreels_jokers.py draws today"
          % sheet_name)

# Every sheet the game blits has to be in the build's list, or the header it
# includes simply does not exist. Cheap to check and impossible to notice from
# a screenshot, because a missing sheet is a build error and a sheet listed but
# never drawn is a silent 12 KB of flash.
cmake_src = re.search(r"set\(jokerreels_sprite_files(.*?)\)",
                      read(GAME, "sprites.cmake"), re.S)
check(cmake_src is not None, "sprites.cmake lists the sheets")
if cmake_src:
    listed = sorted(os.path.splitext(f)[0]
                    for f in cmake_src.group(1).split() if f.endswith(".png"))
    check(listed == ["extras", "hands", "items", "jokers"],
          "sprites.cmake lists every sheet: %s" % listed)

# The cell size the game cuts the sheet with. render.hpp names it once and the
# sheet is drawn to it; the two agreeing is the whole reason a cell lands on an
# icon rather than half way between two.
icon_src = re.search(r"constexpr int k_joker_icon = (\d+);",
                     read(GAME, "src", "render.hpp"))
check(icon_src is not None, "render.hpp names the icon size")
if icon_src:
    check(int(icon_src.group(1)) == jokers.CELL,
          "render.hpp cuts the sheet at %s, the sheet is drawn at %d"
          % (icon_src.group(1), jokers.CELL))


# ---------------------------------------------------------------------------
# The how to play diagrams, which are the art that carries NUMBERS.
#
# A symbol that is drawn wrong looks wrong. A diagram that says TWO PAIR pays
# 45 chips at 4 mult after somebody rebalanced the ladder looks exactly right
# and teaches the wrong game, on the page a player reads before their first
# spin. So none of those numbers is typed into the generator: they are parsed
# out of sim.cpp and drawn from there, and this fails when the committed SVG
# stops being what the generator would draw today.
# ---------------------------------------------------------------------------

tables = diagrams.parse_tables()
for name, draw in sorted(diagrams.DIAGRAMS.items()):
    path = os.path.join(GAME, "tutorial", "%s.svg" % name)
    check(os.path.isfile(path), "tutorial/%s.svg is committed" % name)
    if not os.path.isfile(path):
        continue
    with open(path, encoding="utf-8") as handle:
        committed = handle.read()
    check(committed == draw(tables),
          "tutorial/%s.svg is what gen_jokerreels_diagrams.py draws today "
          "(rerun it)" % name)

# The worked example, bound to the C++ that proves it.
#
# The generator computes the example's hands and its total by reimplementing
# the SHAPE of score() in Python, which keeps the numbers right when the ladder
# moves and does nothing about the shape being wrong. preview.cpp puts the same
# grid through the real scorer. That is only worth anything while the two grids
# are the same grid, which is what this checks.
preview = read(GAME, "tests", "preview.cpp")
grid_src = re.search(
    r"const uint8_t example\[jr::k_drums\]\[jr::k_rows\] = \{(.*?)\};",
    preview, re.S)
check(grid_src is not None, "preview.cpp has the worked example grid")
if grid_src:
    reels = [re.findall(r"jr::k([A-Z][a-z]+)", row)
             for row in re.findall(r"\{([^{}]*)\}", grid_src.group(1))]
    # preview.cpp lists a reel at a time, top to bottom; the generator lists a
    # row at a time, left to right. Same grid, read the other way round.
    from_preview = [[reel[row].upper() for reel in reels]
                    for row in range(len(reels[0]))] if reels else []
    check(from_preview == diagrams.EXAMPLE,
          "the grid on the page and the grid the rules score are the same:\n"
          "   preview.cpp: %s\n     generator: %s"
          % (from_preview, diagrams.EXAMPLE))

_, _, mult, pile, total = diagrams.score_example(tables)
for field, value in (("pile", pile), ("mult", mult), ("total", total)):
    found = re.search(r"const int k_example_%s = (\d+);" % field, preview)
    check(found is not None, "preview.cpp names the example's %s" % field)
    if found:
        check(int(found.group(1)) == value,
              "the picture's %s is %d, preview.cpp checks %s"
              % (field, value, found.group(1)))

if failures:
    print("\n%d check(s) failed" % len(failures))
    sys.exit(1)
print("test_jokerreels_art: all checks pass")
