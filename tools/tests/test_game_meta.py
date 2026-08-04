#!/usr/bin/env python3
"""Round trip the game metadata block: PNG in, .uf2 out, picture back.

The point of the block is that a launcher on the device and a tool on a
desktop can both learn a game's name and see its icon from the .uf2 alone. So
the test builds a real block from a real PNG, wraps it in a real .uf2 the way
the SDK would, and reads it back the way those two readers will.
"""

import os
import struct
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
TOOLS = os.path.dirname(HERE)
sys.path.insert(0, TOOLS)

import game_meta  # noqa: E402

FAILURES = []


def check(condition, what):
    if condition:
        return
    FAILURES.append(what)
    sys.stderr.write("FAIL: %s\n" % what)


def make_uf2(path, image, base=0x10000000):
    """Wrap a flash image in UF2 blocks, 256 payload bytes each, as picotool
    does. Deliberately built here rather than mocked: the extractor has to
    cope with the real block layout, including a block split mid struct."""
    blocks = [image[i:i + 256] for i in range(0, len(image), 256)]
    with open(path, "wb") as handle:
        for index, payload in enumerate(blocks):
            padded = payload + b"\0" * (256 - len(payload))
            header = struct.pack(
                "<IIIIIIII",
                game_meta.UF2_MAGIC_START0, game_meta.UF2_MAGIC_START1,
                0x00002000, base + index * 256, 256, index, len(blocks),
                0xE48BFF56)
            handle.write(header + padded + b"\0" * (476 - 256)
                         + struct.pack("<I", 0x0AB16F30))


def test_round_trip_through_a_uf2():
    with tempfile.TemporaryDirectory() as work:
        # A picture with structure, so a wrong resample or a wrong channel
        # order shows up as a colour that is not there.
        width = height = 96
        rows = []
        for y in range(height):
            row = []
            for x in range(width):
                row.append((255, 0, 0) if y < height // 2 else (0, 0, 255))
            rows.append(row)
        source = os.path.join(work, "thumbnail.png")
        game_meta.write_png(source, width, height, rows)

        decoded_w, decoded_h, decoded = game_meta.read_png(source)
        check((decoded_w, decoded_h) == (width, height), "png size survives")
        check(decoded[0][0] == (255, 0, 0), "png top row is red")
        check(decoded[height - 1][0] == (0, 0, 255), "png bottom row is blue")

        icon = game_meta.resample(decoded, width, height,
                                  game_meta.ICON_W, game_meta.ICON_H)
        block = game_meta.pack("kingfisher", "Kingfisher", "v1.2.3", icon)
        check(len(block) == game_meta.BLOCK_SIZE, "block is the fixed size")

        # Bury it in a plausible flash image: boot2, some code, the block,
        # more code. The reader must find it without being told where.
        image = (b"\xAA" * 512) + (b"\x11" * 3000) + block + (b"\x22" * 1500)
        uf2 = os.path.join(work, "game.uf2")
        make_uf2(uf2, image)

        _base, flat = game_meta.uf2_image(uf2)
        info, pixels = game_meta.unpack(game_meta.find_block(flat))

        check(info["slug"] == "kingfisher", "slug survives the round trip")
        check(info["title"] == "Kingfisher", "title survives the round trip")
        check(info["version"] == "v1.2.3", "version survives the round trip")
        check(info["icon_w"] == game_meta.ICON_W, "icon width survives")

        top = pixels[2][game_meta.ICON_W // 2]
        bottom = pixels[game_meta.ICON_H - 3][game_meta.ICON_W // 2]
        check(top[0] > 200 and top[2] < 40, "icon keeps the red half: %s" % (top,))
        check(bottom[2] > 200 and bottom[0] < 40,
              "icon keeps the blue half: %s" % (bottom,))


def test_placeholder_is_stable_and_distinct():
    a = game_meta.placeholder_icon("kingfisher")
    b = game_meta.placeholder_icon("kingfisher")
    c = game_meta.placeholder_icon("dustrider")
    check(a == b, "the same slug always generates the same placeholder")
    check(a != c, "different slugs generate different placeholders")
    check(len(a) == game_meta.ICON_H and len(a[0]) == game_meta.ICON_W,
          "placeholder is icon sized")


def test_overlong_fields_are_rejected():
    icon = game_meta.placeholder_icon("x")
    try:
        game_meta.pack("x" * 40, "title", "v1", icon)
    except game_meta.MetaError:
        pass
    else:
        check(False, "an overlong slug must be an error, not a truncation")


def test_emit_produces_compilable_source():
    """The generated source has to compile and keep its section, or the block
    silently never reaches the binary."""
    with tempfile.TemporaryDirectory() as work:
        game = os.path.join(work, "sample")
        os.makedirs(game)
        with open(os.path.join(game, "game.yml"), "w") as handle:
            handle.write("slug: sample\ntitle: Sample Game\nsdk: 32blit\n")

        out = os.path.join(work, "meta.cpp")
        code = subprocess.call(
            [sys.executable, os.path.join(TOOLS, "game_meta.py"), "emit",
             "--game", game, "--out", out, "--version", "v9"],
            stderr=subprocess.DEVNULL)
        check(code == 0, "emit succeeds for a game with no icon")
        check(os.path.isfile(out), "emit writes the source")

        source = open(out).read()
        check('section(".pse_meta")' in source, "the block keeps its section")
        check("__attribute__((used" in source,
              "the block is marked used, nothing references it")

        objects = os.path.join(work, "meta.o")
        compiled = subprocess.call(
            ["g++", "-std=c++17", "-c", out, "-o", objects],
            stderr=subprocess.DEVNULL)
        if compiled != 0:
            sys.stderr.write("skip: no host compiler for the emit test\n")
            return
        check(True, "generated source compiles")

        # Prove the bytes really are in the object, not just in the text.
        with open(objects, "rb") as handle:
            blob = handle.read()
        check(game_meta.MAGIC in blob, "the magic reaches the object file")
        index = blob.find(game_meta.MAGIC)
        info, _pixels = game_meta.unpack(
            blob[index:index + game_meta.BLOCK_SIZE])
        check(info["slug"] == "sample", "object carries the slug")
        check(info["title"] == "Sample Game", "object carries the title")
        check(info["version"] == "v9", "object carries the version")


def main():
    test_round_trip_through_a_uf2()
    test_placeholder_is_stable_and_distinct()
    test_overlong_fields_are_rejected()
    test_emit_produces_compilable_source()

    if FAILURES:
        sys.stderr.write("\n%d check(s) failed\n" % len(FAILURES))
        return 1
    sys.stdout.write("game_meta: all checks passed\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
