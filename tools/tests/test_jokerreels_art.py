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

if failures:
    print("\n%d check(s) failed" % len(failures))
    sys.exit(1)
print("test_jokerreels_art: all checks pass")
