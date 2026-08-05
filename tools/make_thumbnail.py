#!/usr/bin/env python3
"""Turn a preview frame into the PNG a game ships as its screenshot.

The host preview harnesses render real frames through the real engine and
write them as PPM, which nothing else reads. This converts one of those frames
into `games/<slug>/thumbnail.png`, which is the single picture a game has: the
gallery copies it over any captured shot, and game_meta.py resamples it down to
the 48x48 icon compiled into the `.uf2` for the launcher.

Games draw at 120x120 and the PicoSystem scales that to its 240x240 panel with
no filtering, so the default 2x nearest neighbour enlargement is not an
invention. It is what the hardware does, and it keeps the pixels square in a
gallery card that reserves 240x240.

Pillow is deliberately not used. This runs from a checkout with nothing
installed, same reason game_meta.py decodes PNG by hand.

Usage:
    make_thumbnail.py --ppm preview_7_cactus.ppm --out games/dustrider/thumbnail.png
"""

import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from game_meta import write_png  # noqa: E402  (same directory)


class ThumbnailError(Exception):
    """Raised when a frame cannot be read."""


def read_ppm(path):
    """Return (width, height, rows) for a binary P6 PPM, 8 bits per channel.

    Only P6 at maxval 255, because that is the only thing the preview
    harnesses write. Anything else is an error rather than a wrong picture.
    """
    with open(path, "rb") as handle:
        data = handle.read()

    if data[:2] != b"P6":
        raise ThumbnailError("%s: not a binary PPM" % path)

    # The header is three whitespace separated numbers after the magic, with
    # comment lines allowed anywhere between them.
    fields = []
    pos = 2
    while len(fields) < 3:
        while pos < len(data) and data[pos:pos + 1].isspace():
            pos += 1
        if data[pos:pos + 1] == b"#":
            while pos < len(data) and data[pos:pos + 1] not in (b"\n", b"\r"):
                pos += 1
            continue
        start = pos
        while pos < len(data) and not data[pos:pos + 1].isspace():
            pos += 1
        if start == pos:
            raise ThumbnailError("%s: truncated PPM header" % path)
        fields.append(int(data[start:pos]))
    pos += 1                        # exactly one whitespace byte, per the spec

    width, height, maxval = fields
    if maxval != 255:
        raise ThumbnailError("%s: need 8 bit samples, got maxval %d"
                             % (path, maxval))

    need = width * height * 3
    body = data[pos:pos + need]
    if len(body) < need:
        raise ThumbnailError("%s: wanted %d pixel bytes, found %d"
                             % (path, need, len(body)))

    rows = []
    for y in range(height):
        base = y * width * 3
        rows.append([tuple(body[base + x * 3:base + x * 3 + 3])
                     for x in range(width)])
    return width, height, rows


def enlarge(rows, scale):
    """Nearest neighbour, because the console does not filter either."""
    if scale == 1:
        return rows
    out = []
    for row in rows:
        wide = []
        for pixel in row:
            wide.extend([pixel] * scale)
        out.extend([wide] * scale)
    return out


def main(argv):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--ppm", required=True, help="frame to convert")
    parser.add_argument("--out", required=True, help="PNG to write")
    parser.add_argument("--scale", type=int, default=2,
                        help="integer enlargement, default 2 for 120 to 240")
    args = parser.parse_args(argv)

    if args.scale < 1:
        sys.stderr.write("make_thumbnail: scale must be at least 1\n")
        return 1

    try:
        width, height, rows = read_ppm(args.ppm)
    except (OSError, ThumbnailError) as error:
        sys.stderr.write("make_thumbnail: %s\n" % error)
        return 1

    rows = enlarge(rows, args.scale)
    directory = os.path.dirname(os.path.abspath(args.out))
    if directory:
        os.makedirs(directory, exist_ok=True)
    write_png(args.out, width * args.scale, height * args.scale, rows)

    sys.stderr.write("make_thumbnail: %s -> %s (%dx%d, %d bytes)\n"
                     % (args.ppm, args.out, width * args.scale,
                        height * args.scale, os.path.getsize(args.out)))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
