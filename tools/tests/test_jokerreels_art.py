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

if failures:
    print("\n%d check(s) failed" % len(failures))
    sys.exit(1)
print("test_jokerreels_art: all checks pass")
