#!/usr/bin/env python3
"""Tests for the bundle composer.

A wrong bundle is a console that boots into the wrong thing, or does not boot,
and the only diagnosis available is watching a black screen. So the checks that
refuse a bad bundle matter more than the happy path, and most of this file is
about them.
"""

import os
import struct
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
TOOLS = os.path.dirname(HERE)
sys.path.insert(0, TOOLS)

import game_meta      # noqa: E402
import make_bundle    # noqa: E402

FAILURES = []


def check(condition, what):
    if condition:
        return
    FAILURES.append(what)
    sys.stderr.write("FAIL: %s\n" % what)


def write_uf2(path, address, size, with_meta=None):
    """A plausible image: a vector table at +0x100, then padding, then the
    metadata block if asked for."""
    image = bytearray(b"\x00" * size)
    image[0x100:0x104] = struct.pack("<I", 0x20042000)
    image[0x104:0x108] = struct.pack("<I", address + 0x109)
    if with_meta is not None:
        block = game_meta.pack(with_meta, with_meta.title(), "v1.0.0",
                               game_meta.placeholder_icon(with_meta))
        image[0x1000:0x1000 + len(block)] = block

    blocks = [(address + i, bytes(image[i:i + 256]))
              for i in range(0, len(image), 256)]
    make_bundle.write_blocks(path, blocks)


def run(work, *args):
    return subprocess.run(
        [sys.executable, os.path.join(TOOLS, "make_bundle.py")] + list(args),
        capture_output=True, text=True, cwd=work)


def test_a_good_bundle_round_trips():
    with tempfile.TemporaryDirectory() as work:
        launcher = os.path.join(work, "launcher.uf2")
        one = os.path.join(work, "one.uf2")
        two = os.path.join(work, "two.uf2")
        out = os.path.join(work, "bundle.uf2")

        write_uf2(launcher, make_bundle.FLASH_BASE, 0x8000)
        write_uf2(one, make_bundle.slot_address(1), 0x8000, with_meta="kingfisher")
        write_uf2(two, make_bundle.slot_address(2), 0x8000, with_meta="dustrider")

        result = run(work, "--launcher", launcher, "--out", out,
                     "--game", "kingfisher=1:" + one,
                     "--game", "dustrider=2:" + two)
        check(result.returncode == 0, "a good bundle is written: " + result.stderr)
        check(os.path.isfile(out), "the bundle file exists")

        # Every block has to be numbered for the bundle, not for the image it
        # came from, or the flashing tool sees three separate transfers.
        blocks = make_bundle.read_blocks(out)
        with open(out, "rb") as handle:
            data = handle.read()
        total = len(data) // 512
        numbers = [struct.unpack("<I", data[i * 512 + 20:i * 512 + 24])[0]
                   for i in range(total)]
        counts = {struct.unpack("<I", data[i * 512 + 24:i * 512 + 28])[0]
                  for i in range(total)}
        check(numbers == list(range(total)), "blocks are renumbered in order")
        check(counts == {total}, "every block agrees on the bundle's length")
        check(len(blocks) == total, "no blocks were lost")

        listing = run(work, "--list", out)
        check("Kingfisher" in listing.stdout and "Dustrider" in listing.stdout,
              "listing names both games:\n" + listing.stdout)
        check("slot  0  launcher" in listing.stdout,
              "listing shows the launcher in slot 0")


def test_a_game_built_for_the_wrong_slot_is_refused():
    with tempfile.TemporaryDirectory() as work:
        launcher = os.path.join(work, "launcher.uf2")
        game = os.path.join(work, "game.uf2")
        out = os.path.join(work, "bundle.uf2")
        write_uf2(launcher, make_bundle.FLASH_BASE, 0x8000)
        # Linked for slot 1, bundled into slot 2: on the device this boots
        # into whatever is at slot 2, which is nothing.
        write_uf2(game, make_bundle.slot_address(1), 0x8000, with_meta="a")

        result = run(work, "--launcher", launcher, "--out", out,
                     "--game", "a=2:" + game)
        check(result.returncode != 0, "a mislinked game is refused")
        check("-DPICO_SLOT=2" in result.stderr,
              "the error says how to fix it: " + result.stderr)
        check(not os.path.isfile(out), "nothing is written when it fails")


def test_two_games_cannot_share_a_slot():
    with tempfile.TemporaryDirectory() as work:
        launcher = os.path.join(work, "launcher.uf2")
        a = os.path.join(work, "a.uf2")
        b = os.path.join(work, "b.uf2")
        out = os.path.join(work, "bundle.uf2")
        write_uf2(launcher, make_bundle.FLASH_BASE, 0x8000)
        write_uf2(a, make_bundle.slot_address(1), 0x8000, with_meta="a")
        write_uf2(b, make_bundle.slot_address(1), 0x8000, with_meta="b")

        result = run(work, "--launcher", launcher, "--out", out,
                     "--game", "a=1:" + a, "--game", "b=1:" + b)
        check(result.returncode != 0, "two games in one slot is refused")
        check("claimed by both" in result.stderr,
              "the error names the collision: " + result.stderr)


def test_a_game_too_big_for_its_slot_is_refused():
    with tempfile.TemporaryDirectory() as work:
        launcher = os.path.join(work, "launcher.uf2")
        game = os.path.join(work, "game.uf2")
        out = os.path.join(work, "bundle.uf2")
        write_uf2(launcher, make_bundle.FLASH_BASE, 0x8000)
        write_uf2(game, make_bundle.slot_address(1),
                  make_bundle.SLOT_SIZE + 0x1000, with_meta="a")

        result = run(work, "--launcher", launcher, "--out", out,
                     "--game", "a=1:" + game)
        check(result.returncode != 0, "an oversized game is refused")
        check("a slot holds" in result.stderr,
              "the error explains the overflow: " + result.stderr)


def test_a_launcher_that_would_eat_slot_one_is_refused():
    with tempfile.TemporaryDirectory() as work:
        launcher = os.path.join(work, "launcher.uf2")
        out = os.path.join(work, "bundle.uf2")
        write_uf2(launcher, make_bundle.FLASH_BASE,
                  make_bundle.SLOT_SIZE + 0x1000)

        result = run(work, "--launcher", launcher, "--out", out)
        check(result.returncode != 0, "an oversized launcher is refused")
        check("slot 1" in result.stderr,
              "the error says what it would overwrite: " + result.stderr)


def test_a_game_with_no_metadata_is_refused():
    with tempfile.TemporaryDirectory() as work:
        launcher = os.path.join(work, "launcher.uf2")
        game = os.path.join(work, "game.uf2")
        out = os.path.join(work, "bundle.uf2")
        write_uf2(launcher, make_bundle.FLASH_BASE, 0x8000)
        write_uf2(game, make_bundle.slot_address(1), 0x8000)   # no block

        result = run(work, "--launcher", launcher, "--out", out,
                     "--game", "a=1:" + game)
        check(result.returncode != 0,
              "a game the launcher could not list is refused")
        check("would not list it" in result.stderr,
              "the error explains why it matters: " + result.stderr)


def test_slot_map_matches_the_firmware_and_the_tool():
    # launcher/src/library.hpp hardcodes the same three numbers, and
    # cmake/slot.cmake links against them. A silent disagreement here puts a
    # game at an address the launcher never looks at.
    header = os.path.join(os.path.dirname(TOOLS), "launcher", "src",
                          "library.hpp")
    text = open(header, encoding="utf-8").read()
    check("k_flash_base = 0x10000000u" in text,
          "the firmware agrees on the flash base")
    check("k_slot_size = 512u * 1024u" in text,
          "the firmware agrees on the slot size")
    check("k_max_slots = 23" in text, "the firmware agrees on the slot count")

    cmake = os.path.join(os.path.dirname(TOOLS), "cmake", "slot.cmake")
    text = open(cmake, encoding="utf-8").read()
    check("PICO_SLOT_SIZE 524288" in text, "the linker agrees on the slot size")
    check("PICO_SLOT_COUNT 23" in text, "the linker agrees on the slot count")

    # The desktop tool composes bundles too, so it is a fourth copy of the same
    # map. It cannot import this file, and a silent disagreement there writes a
    # bundle that boots into the wrong thing.
    flasher = os.path.join(os.path.dirname(TOOLS), "tools", "flasher",
                           "Bundle.cs")
    text = open(flasher, encoding="utf-8").read()
    check("FlashBase = 0x10000000" in text, "the tool agrees on the flash base")
    check("SlotSize = 512 * 1024" in text, "the tool agrees on the slot size")
    check("SlotCount = 23" in text, "the tool agrees on the slot count")

    meta = os.path.join(os.path.dirname(TOOLS), "tools", "flasher",
                        "GameMeta.cs")
    text = open(meta, encoding="utf-8").read()
    check("HeaderSize = %d" % game_meta.HEADER_SIZE in text,
          "the tool agrees on the metadata header size")
    check("IconWidth = %d" % game_meta.ICON_W in text,
          "the tool agrees on the icon width")
    check("IconHeight = %d" % game_meta.ICON_H in text,
          "the tool agrees on the icon height")
    for name, offset in (("OffsetSlug", 16), ("OffsetTitle", 40),
                         ("OffsetVersion", 72)):
        check("%s = %d" % (name, offset) in text,
              "the tool agrees on %s" % name)


def main():
    test_a_good_bundle_round_trips()
    test_a_game_built_for_the_wrong_slot_is_refused()
    test_two_games_cannot_share_a_slot()
    test_a_game_too_big_for_its_slot_is_refused()
    test_a_launcher_that_would_eat_slot_one_is_refused()
    test_a_game_with_no_metadata_is_refused()
    test_slot_map_matches_the_firmware_and_the_tool()

    if FAILURES:
        sys.stderr.write("\n%d check(s) failed\n" % len(FAILURES))
        return 1
    sys.stdout.write("make_bundle: all checks passed\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
