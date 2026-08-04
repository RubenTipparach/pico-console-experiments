#!/usr/bin/env python3
"""Build and read the metadata block every game carries in its own flash.

A PicoSystem `.uf2` says almost nothing about itself. The pico port's binary
info carries a program name and an author string and nothing else: no title
for a menu, and definitely no picture. So a launcher browsing installed games,
or a desktop tool listing what is on a device, has nothing to show.

This puts a fixed size block inside the game binary, in its own `.pse_meta`
section, so the picture and the name travel with the `.uf2` itself. Nothing
extra to ship, nothing to keep in sync, and an existing `.uf2` can be read
back by scanning for the magic.

The block is `const`, so it lives in flash and costs no RAM (CLAUDE.md rule 8).

Layout, little endian, 4704 bytes total:

    offset  size  field
    0       8     magic, "PSEGAME1"
    8       2     block size in bytes
    10      2     icon width
    12      2     icon height
    14      2     format, 1 = RGB565
    16      24    slug, NUL padded
    40      32    title, NUL padded
    72      16    version, NUL padded
    88      8     reserved
    96      4608  icon pixels, RGB565 little endian, 48x48

Commands:
    game_meta.py emit --game games/kingfisher --out meta.cpp
    game_meta.py extract --uf2 kingfisher.uf2 [--icon out.png]

PNG decoding is done here rather than with Pillow on purpose: this runs inside
every game build, and a pip install on every CI runner is exactly the cost this
repo avoids elsewhere (see the tiny YAML reader in build_plan.py).
"""

import argparse
import json
import os
import struct
import sys
import zlib

MAGIC = b"PSEGAME1"
ICON_W = 48
ICON_H = 48
FORMAT_RGB565 = 1
HEADER_SIZE = 96
BLOCK_SIZE = HEADER_SIZE + ICON_W * ICON_H * 2

SLUG_MAX = 24
TITLE_MAX = 32
VERSION_MAX = 16

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))


class MetaError(Exception):
    """Raised when a game cannot be described or a block cannot be read."""


# ---- PNG reading, the subset our own thumbnails are written in ----

def read_png(path):
    """Return (width, height, rows) with rows as lists of (r, g, b).

    Handles 8 bit truecolour and truecolour+alpha, non interlaced, which is
    what every tool in this repo writes. Anything else is an error rather
    than a silent wrong colour.
    """
    with open(path, "rb") as handle:
        data = handle.read()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise MetaError("%s: not a PNG" % path)

    pos = 8
    width = height = depth = colour = None
    interlace = 0
    idat = []
    while pos + 8 <= len(data):
        length, kind = struct.unpack(">I4s", data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + length]
        pos += 12 + length          # 4 length, 4 kind, body, 4 crc
        if kind == b"IHDR":
            width, height, depth, colour, _, _, interlace = struct.unpack(
                ">IIBBBBB", body)
        elif kind == b"IDAT":
            idat.append(body)
        elif kind == b"IEND":
            break

    if width is None:
        raise MetaError("%s: no IHDR" % path)
    if depth != 8 or colour not in (2, 6) or interlace != 0:
        raise MetaError("%s: need 8 bit RGB or RGBA, not interlaced "
                        "(depth %s, colour type %s, interlace %s)"
                        % (path, depth, colour, interlace))

    channels = 3 if colour == 2 else 4
    raw = zlib.decompress(b"".join(idat))
    stride = width * channels
    rows = []
    previous = bytearray(stride)
    pos = 0
    for _ in range(height):
        filter_type = raw[pos]
        pos += 1
        line = bytearray(raw[pos:pos + stride])
        pos += stride
        _unfilter(filter_type, line, previous, channels)
        rows.append([tuple(line[x * channels:x * channels + 3])
                     for x in range(width)])
        previous = line
    return width, height, rows


def _unfilter(filter_type, line, previous, channels):
    if filter_type == 0:
        return
    for i in range(len(line)):
        left = line[i - channels] if i >= channels else 0
        up = previous[i]
        if filter_type == 1:
            line[i] = (line[i] + left) & 0xFF
        elif filter_type == 2:
            line[i] = (line[i] + up) & 0xFF
        elif filter_type == 3:
            line[i] = (line[i] + (left + up) // 2) & 0xFF
        elif filter_type == 4:
            upleft = previous[i - channels] if i >= channels else 0
            line[i] = (line[i] + _paeth(left, up, upleft)) & 0xFF
        else:
            raise MetaError("unknown PNG filter %d" % filter_type)


def _paeth(a, b, c):
    p = a + b - c
    pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
    if pa <= pb and pa <= pc:
        return a
    return b if pb <= pc else c


def write_png(path, width, height, pixels):
    """Write 8 bit RGB. Used to hand an extracted icon back as a real file."""
    raw = bytearray()
    for y in range(height):
        raw.append(0)               # filter: none
        for x in range(width):
            raw.extend(pixels[y][x])

    def chunk(kind, body):
        head = struct.pack(">I", len(body)) + kind
        return head + body + struct.pack(">I", zlib.crc32(kind + body))

    with open(path, "wb") as handle:
        handle.write(b"\x89PNG\r\n\x1a\n")
        handle.write(chunk(b"IHDR", struct.pack(">IIBBBBB", width, height,
                                                8, 2, 0, 0, 0)))
        handle.write(chunk(b"IDAT", zlib.compress(bytes(raw), 9)))
        handle.write(chunk(b"IEND", b""))


# ---- the icon ----

def resample(rows, src_w, src_h, out_w, out_h):
    """Box average down to the icon size.

    Nearest neighbour on a 240 wide screenshot drops most of the picture and
    aliases hard; averaging keeps something recognisable at 48 pixels.
    """
    out = []
    for y in range(out_h):
        y0 = (y * src_h) // out_h
        y1 = max(y0 + 1, ((y + 1) * src_h) // out_h)
        row = []
        for x in range(out_w):
            x0 = (x * src_w) // out_w
            x1 = max(x0 + 1, ((x + 1) * src_w) // out_w)
            r = g = b = count = 0
            for sy in range(y0, y1):
                for sx in range(x0, x1):
                    pr, pg, pb = rows[sy][sx]
                    r += pr
                    g += pg
                    b += pb
                    count += 1
            row.append((r // count, g // count, b // count))
        out.append(row)
    return out


def placeholder_icon(slug):
    """A generated icon for a game with no picture yet.

    Deterministic from the slug, so a game keeps the same colour between
    builds, and clearly synthetic so nobody mistakes it for a screenshot.
    """
    seed = 0
    for char in slug.encode("utf-8"):
        seed = (seed * 131 + char) & 0xFFFFFFFF
    hue_r = 40 + (seed & 0x3F)
    hue_g = 40 + ((seed >> 8) & 0x3F)
    hue_b = 80 + ((seed >> 16) & 0x5F)

    rows = []
    for y in range(ICON_H):
        row = []
        for x in range(ICON_W):
            edge = x < 2 or y < 2 or x >= ICON_W - 2 or y >= ICON_H - 2
            fade = (y * 40) // ICON_H
            if edge:
                row.append((hue_r // 2, hue_g // 2, hue_b // 2))
            else:
                row.append((min(255, hue_r + fade), min(255, hue_g + fade),
                            min(255, hue_b + fade)))
        rows.append(row)

    # A diagonal slash so an unset icon is unmistakable at a glance.
    for i in range(6, ICON_W - 6):
        for w in range(2):
            y = i + w
            if 0 <= y < ICON_H:
                rows[y][i] = (235, 235, 240)
    return rows


def icon_for(game_dir, slug):
    """icon.png beats thumbnail.png beats a generated placeholder.

    icon.png is the authored square art; thumbnail.png is the screenshot the
    gallery already uses, which crops acceptably at this size.
    """
    for name in ("icon.png", "thumbnail.png"):
        path = os.path.join(game_dir, name)
        if os.path.isfile(path):
            width, height, rows = read_png(path)
            # Centre crop to square first, so a 240x240 screenshot and a
            # 320x240 one both give the middle of the picture.
            side = min(width, height)
            x0 = (width - side) // 2
            y0 = (height - side) // 2
            square = [row[x0:x0 + side] for row in rows[y0:y0 + side]]
            return resample(square, side, side, ICON_W, ICON_H), name
    return placeholder_icon(slug), "generated"


def to_rgb565(rows):
    out = bytearray()
    for row in rows:
        for r, g, b in row:
            value = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
            out.extend(struct.pack("<H", value))
    return bytes(out)


# ---- the block ----

def pack(slug, title, version, icon_rows):
    def field(text, size, name):
        raw = text.encode("utf-8")
        if len(raw) >= size:
            raise MetaError("%s is too long: %r needs under %d bytes"
                            % (name, text, size))
        return raw + b"\0" * (size - len(raw))

    block = bytearray()
    block += MAGIC
    block += struct.pack("<HHHH", BLOCK_SIZE, ICON_W, ICON_H, FORMAT_RGB565)
    block += field(slug, SLUG_MAX, "slug")
    block += field(title, TITLE_MAX, "title")
    block += field(version, VERSION_MAX, "version")
    block += b"\0" * 8
    assert len(block) == HEADER_SIZE, len(block)
    block += to_rgb565(icon_rows)
    assert len(block) == BLOCK_SIZE, len(block)
    return bytes(block)


def unpack(block):
    if block[:8] != MAGIC:
        raise MetaError("not a PSE metadata block")
    size, icon_w, icon_h, fmt = struct.unpack("<HHHH", block[8:16])
    if fmt != FORMAT_RGB565:
        raise MetaError("unknown icon format %d" % fmt)
    if len(block) < size:
        raise MetaError("truncated metadata block")

    def text(start, size_):
        return block[start:start + size_].split(b"\0", 1)[0].decode("utf-8")

    pixels = []
    pos = HEADER_SIZE
    for _ in range(icon_h):
        row = []
        for _ in range(icon_w):
            value = struct.unpack("<H", block[pos:pos + 2])[0]
            pos += 2
            r = (value >> 11) & 0x1F
            g = (value >> 5) & 0x3F
            b = value & 0x1F
            row.append(((r * 255) // 31, (g * 255) // 63, (b * 255) // 31))
        pixels.append(row)

    return {
        "slug": text(16, SLUG_MAX),
        "title": text(40, TITLE_MAX),
        "version": text(72, VERSION_MAX),
        "icon_w": icon_w,
        "icon_h": icon_h,
    }, pixels


# ---- uf2 ----

UF2_MAGIC_START0 = 0x0A324655
UF2_MAGIC_START1 = 0x9E5D5157
UF2_BLOCK = 512


def uf2_image(path):
    """Flatten a .uf2 into (base address, bytes), gaps filled with zero."""
    chunks = {}
    with open(path, "rb") as handle:
        while True:
            block = handle.read(UF2_BLOCK)
            if len(block) < UF2_BLOCK:
                break
            start0, start1, _flags, address, size = struct.unpack(
                "<IIIII", block[:20])
            if start0 != UF2_MAGIC_START0 or start1 != UF2_MAGIC_START1:
                raise MetaError("%s: not a UF2" % path)
            chunks[address] = block[32:32 + size]
    if not chunks:
        raise MetaError("%s: no UF2 blocks" % path)

    base = min(chunks)
    end = max(address + len(data) for address, data in chunks.items())
    image = bytearray(end - base)
    for address, data in chunks.items():
        image[address - base:address - base + len(data)] = data
    return base, bytes(image)


def find_block(image):
    index = image.find(MAGIC)
    if index < 0:
        raise MetaError("no PSE metadata block found")
    return image[index:index + BLOCK_SIZE]


# ---- commands ----

def cmd_emit(args):
    from build_plan import Game       # noqa: E402  (same directory)

    game = Game(os.path.abspath(args.game))
    icon_rows, source = icon_for(game.directory, game.slug)
    block = pack(game.slug, game.title, args.version, icon_rows)

    lines = [
        "// Generated by tools/game_meta.py. Do not edit, do not commit.",
        "//",
        "// The launcher and the desktop tool both read this block straight",
        "// out of the .uf2, so it must keep its own section and must not be",
        "// dropped as unused: nothing in the game references it.",
        "//",
        "// icon source: %s" % source,
        "",
        "#include <cstdint>",
        "",
        "// `retain` is the load bearing word. `used` only stops the",
        "// compiler discarding this; the pico SDK links with --gc-sections,",
        "// and nothing in the game references the block, so without",
        "// SHF_GNU_RETAIN the linker collects it and the .uf2 ships with no",
        "// name and no icon. cmake/game_meta.cmake also passes -u as a",
        "// second guarantee for toolchains that ignore the attribute.",
        'extern "C" __attribute__((used, retain, section(".pse_meta")))',
        "const uint8_t pse_game_meta[%d] = {" % len(block),
    ]
    for offset in range(0, len(block), 16):
        row = ", ".join("0x%02X" % byte for byte in block[offset:offset + 16])
        lines.append("    %s," % row)
    lines.append("};")
    lines.append("")

    directory = os.path.dirname(os.path.abspath(args.out))
    if directory:
        os.makedirs(directory, exist_ok=True)
    with open(args.out, "w", encoding="utf-8") as handle:
        handle.write("\n".join(lines))
    sys.stderr.write("game_meta: %s icon from %s\n" % (game.slug, source))
    return 0


def cmd_extract(args):
    _base, image = uf2_image(args.uf2)
    info, pixels = unpack(find_block(image))
    if args.icon:
        write_png(args.icon, info["icon_w"], info["icon_h"], pixels)
        info["icon"] = args.icon
    sys.stdout.write(json.dumps(info, indent=2, sort_keys=True) + "\n")
    return 0


def main(argv):
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)

    emit = sub.add_parser("emit")
    emit.add_argument("--game", required=True, help="path to a game directory")
    emit.add_argument("--out", required=True, help="C++ source to write")
    emit.add_argument("--version", default="dev")
    emit.set_defaults(func=cmd_emit)

    extract = sub.add_parser("extract")
    extract.add_argument("--uf2", required=True)
    extract.add_argument("--icon", help="write the icon here as a PNG")
    extract.set_defaults(func=cmd_extract)

    args = parser.parse_args(argv)
    try:
        return args.func(args)
    except MetaError as error:
        sys.stderr.write("game_meta: %s\n" % error)
        return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
