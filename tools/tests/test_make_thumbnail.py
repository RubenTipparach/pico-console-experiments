#!/usr/bin/env python3
"""Prove a preview frame survives the trip to a committed screenshot.

A thumbnail is the only picture a game has: the gallery copies it over any
captured shot and the launcher icon is resampled from it. So the two things
that matter are that no colour changes on the way through, and that whatever
lands in the repo is something game_meta.py can actually read. This repo has
already shipped inverted colours to a device once, and a converter that
quietly swapped a channel would look exactly like a correct one.
"""

import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
TOOLS = os.path.dirname(HERE)
REPO = os.path.dirname(TOOLS)
sys.path.insert(0, TOOLS)

import game_meta        # noqa: E402
import make_thumbnail   # noqa: E402

FAILURES = []


def check(condition, what):
    if condition:
        return
    FAILURES.append(what)
    sys.stderr.write("FAIL: %s\n" % what)


def write_ppm(path, width, height, rows, comment=False):
    header = b"P6\n"
    if comment:
        header += b"# written by the preview harness\n"
    header += b"%d %d\n255\n" % (width, height)
    body = bytearray()
    for row in rows:
        for pixel in row:
            body.extend(bytes(pixel))
    with open(path, "wb") as handle:
        handle.write(header + bytes(body))


def swatch(width, height):
    """Colours chosen so a swapped channel or a flipped row cannot pass."""
    rows = []
    for y in range(height):
        row = []
        for x in range(width):
            row.append(((x * 37) % 256, (y * 53) % 256, (x * y * 7) % 256))
        rows.append(row)
    return rows


def test_colours_and_size_survive_the_round_trip():
    rows = swatch(9, 5)
    with tempfile.TemporaryDirectory() as work:
        ppm = os.path.join(work, "frame.ppm")
        png = os.path.join(work, "thumb.png")
        write_ppm(ppm, 9, 5, rows)

        code = make_thumbnail.main(["--ppm", ppm, "--out", png, "--scale", "1"])
        check(code == 0, "converting a frame succeeds")

        width, height, back = game_meta.read_png(png)
        check((width, height) == (9, 5), "the PNG keeps the frame's size")
        check(back == rows, "every pixel comes back with the same colour")


def test_enlargement_is_nearest_neighbour():
    """The console scales 120 to 240 without filtering, so this must not blur.

    A blended edge would be invisible in the gallery and obvious as mush in
    the 48x48 launcher icon, which is resampled from this file.
    """
    rows = [[(255, 0, 0), (0, 0, 255)]]
    with tempfile.TemporaryDirectory() as work:
        ppm = os.path.join(work, "frame.ppm")
        png = os.path.join(work, "thumb.png")
        write_ppm(ppm, 2, 1, rows)
        make_thumbnail.main(["--ppm", ppm, "--out", png, "--scale", "2"])

        width, height, back = game_meta.read_png(png)
        check((width, height) == (4, 2), "scale 2 doubles both axes")
        check(back == [[(255, 0, 0), (255, 0, 0), (0, 0, 255), (0, 0, 255)]] * 2,
              "each pixel becomes a solid block, with no blended edge")


def test_header_comments_are_tolerated():
    rows = swatch(4, 4)
    with tempfile.TemporaryDirectory() as work:
        ppm = os.path.join(work, "frame.ppm")
        write_ppm(ppm, 4, 4, rows, comment=True)
        width, height, back = make_thumbnail.read_ppm(ppm)
        check((width, height) == (4, 4), "a commented header still parses")
        check(back == rows, "a commented header does not shift the pixels")


def test_bad_input_is_refused_rather_than_guessed():
    with tempfile.TemporaryDirectory() as work:
        plain = os.path.join(work, "not.ppm")
        with open(plain, "wb") as handle:
            handle.write(b"P3\n2 2\n255\n0 0 0\n")
        try:
            make_thumbnail.read_ppm(plain)
            check(False, "an ASCII PPM is refused")
        except make_thumbnail.ThumbnailError:
            check(True, "an ASCII PPM is refused")

        deep = os.path.join(work, "deep.ppm")
        with open(deep, "wb") as handle:
            handle.write(b"P6\n1 1\n65535\n\0\0\0\0\0\0")
        try:
            make_thumbnail.read_ppm(deep)
            check(False, "a 16 bit PPM is refused rather than misread")
        except make_thumbnail.ThumbnailError:
            check(True, "a 16 bit PPM is refused rather than misread")

        short = os.path.join(work, "short.ppm")
        with open(short, "wb") as handle:
            handle.write(b"P6\n4 4\n255\n" + b"\x10" * 10)
        try:
            make_thumbnail.read_ppm(short)
            check(False, "a truncated frame is refused")
        except make_thumbnail.ThumbnailError:
            check(True, "a truncated frame is refused")


def test_the_cli_writes_a_file_game_meta_can_read():
    """The whole point: what this writes has to feed the icon in the .uf2."""
    rows = swatch(120, 120)
    with tempfile.TemporaryDirectory() as work:
        ppm = os.path.join(work, "frame.ppm")
        game = os.path.join(work, "sample")
        os.makedirs(game)
        write_ppm(ppm, 120, 120, rows)

        result = subprocess.run(
            [sys.executable, os.path.join(TOOLS, "make_thumbnail.py"),
             "--ppm", ppm, "--out", os.path.join(game, "thumbnail.png")],
            capture_output=True)
        check(result.returncode == 0,
              "the command line tool exits clean: %s"
              % result.stderr.decode("utf-8", "replace").strip())

        icon, source = game_meta.icon_for(game, "sample")
        check(source == "thumbnail.png",
              "game_meta takes the icon from the written thumbnail")
        check(len(icon) == game_meta.ICON_H and len(icon[0]) == game_meta.ICON_W,
              "the icon comes out at the block's 48x48")


def test_every_committed_thumbnail_is_readable():
    """A screenshot nobody can decode is worse than none: the build fails."""
    games = os.path.join(REPO, "games")
    found = 0
    for name in sorted(os.listdir(games)):
        path = os.path.join(games, name, "thumbnail.png")
        if not os.path.isfile(path):
            continue
        found += 1
        try:
            width, height, _rows = game_meta.read_png(path)
        except game_meta.MetaError as error:
            check(False, "%s is readable (%s)" % (path, error))
            continue
        check(width >= game_meta.ICON_W and height >= game_meta.ICON_H,
              "%s is at least icon sized, not %dx%d" % (path, width, height))
    sys.stdout.write("make_thumbnail: checked %d committed thumbnail(s)\n"
                     % found)


def main():
    test_colours_and_size_survive_the_round_trip()
    test_enlargement_is_nearest_neighbour()
    test_header_comments_are_tolerated()
    test_bad_input_is_refused_rather_than_guessed()
    test_the_cli_writes_a_file_game_meta_can_read()
    test_every_committed_thumbnail_is_readable()

    if FAILURES:
        sys.stderr.write("\n%d check(s) failed\n" % len(FAILURES))
        return 1
    sys.stdout.write("make_thumbnail: all checks passed\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
