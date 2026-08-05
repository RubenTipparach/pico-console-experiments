#!/usr/bin/env python3
"""Author the Picomon character art and export it.

Run this to regenerate everything:

    python3 build_art.py

It writes, next to itself:
  <name>.aseprite   editable source: three layers (fill, shade, outline),
                    one frame per pose, and a tag per animation
  <name>.png        horizontal sprite strip, one frame per cell
  sheets.json       frame size, frame count and tag ranges for every sheet
  sheets.js         the same strips as base64 data URIs, for pasting into
                    index.html so the mockup stays a single file

The art itself lives in this file as character-per-pixel strings, which is
what makes it reviewable in a diff. Frames are 12 x 20: two tiles tall at the
overworld camera's 10 pixels per tile, which is the proportion Black and White
uses for its own characters.
"""
import argparse
import base64
import json
import os
import struct
import zlib

import aseprite

W, H = 12, 20
HERE = os.path.dirname(os.path.abspath(__file__))

# --- palettes -------------------------------------------------------------
# Every value is a multiple of 0x11 so it survives the PicoSystem's four bits
# per channel unchanged. A colour that does not is a colour that will shift on
# the device and nowhere else, which is the worst place to find out.

BASE = {
    "k": (0x22, 0x11, 0x22),   # outline
    "e": (0x22, 0x11, 0x22),   # eye
    "s": (0xFF, 0xCC, 0x99),   # skin
    "S": (0xDD, 0xAA, 0x77),   # skin shade
    "b": (0x44, 0x33, 0x22),   # boots
    "B": (0x66, 0x55, 0x44),   # boot highlight
}

PALETTES = {
    "hero": dict(BASE, **{
        "h": (0x55, 0x33, 0x22), "H": (0x77, 0x55, 0x33),
        "c": (0xEE, 0x55, 0x77), "C": (0xAA, 0x33, 0x55), "w": (0xFF, 0xDD, 0xEE),
        "p": (0x33, 0x44, 0x88), "P": (0x22, 0x33, 0x66),
    }),
    "villager": dict(BASE, **{
        "h": (0x88, 0x66, 0x44), "H": (0xAA, 0x88, 0x55),
        "c": (0x44, 0xAA, 0xBB), "C": (0x22, 0x77, 0x88), "w": (0xDD, 0xEE, 0xFF),
        "p": (0x22, 0x77, 0x88), "P": (0x11, 0x55, 0x66),
    }),
    "trainer": dict(BASE, **{
        "s": (0xEE, 0xBB, 0x88), "S": (0xCC, 0x99, 0x66),
        "h": (0x55, 0x33, 0x11), "H": (0x77, 0x44, 0x22),
        "r": (0xCC, 0x33, 0x33), "R": (0x99, 0x22, 0x22),
        "c": (0xEE, 0xAA, 0x22), "C": (0xBB, 0x77, 0x11), "w": (0xFF, 0xEE, 0x99),
        "p": (0x55, 0x33, 0x22), "P": (0x44, 0x22, 0x11),
        "b": (0x33, 0x33, 0x22), "B": (0x55, 0x55, 0x33),
    }),
    "healer": dict(BASE, **{
        "h": (0xAA, 0xAA, 0xAA), "H": (0xCC, 0xCC, 0xCC),
        "c": (0xEE, 0xEE, 0xEE), "C": (0xBB, 0xBB, 0xBB), "w": (0xEE, 0x33, 0x55),
        "p": (0x55, 0x66, 0x77), "P": (0x33, 0x44, 0x55),
        "b": (0x33, 0x33, 0x44), "B": (0x55, 0x55, 0x66),
    }),
}

# --- parts ----------------------------------------------------------------
# Heads occupy rows 0-7, bodies rows 8-14, legs rows 15-19.

HEAD = {
    "hero.down": [
        "....kkkk....",
        "..kkhhhhkk..",
        ".khHHhhhhhk.",
        ".khhsssshhk.",
        ".khsesseshk.",
        ".khSssssShk.",
        "..kssssssk..",
        "...kssssk...",
    ],
    "hero.up": [
        "....kkkk....",
        "..kkhhhhkk..",
        ".khhhhhhhhk.",
        ".khHHhhhhhk.",
        ".khhhhhhhhk.",
        ".khhhhhhhhk.",
        "..khhhhhhk..",
        "...khhhhk...",
    ],
    "hero.side": [
        "....kkkk....",
        "..kkhhhhkk..",
        ".khHHhhhhhk.",
        ".khhhhssssk.",
        ".khhhsessSk.",
        ".khhhssssSk.",
        "..khsssssk..",
        "...kssssk...",
    ],
    "villager.down": [
        "....kkkk....",
        "..kkhhhhkk..",
        ".khHHhhhhhk.",
        ".khhsssshhk.",
        ".khsesseshk.",
        ".khSssssShk.",
        ".khsssssshk.",
        "..khssssk...",
    ],
    "trainer.down": [
        "....kkkk....",
        "..kkrrrrkk..",
        ".krrrrrrrrk.",
        ".kRRRRRRRRk.",
        ".khsesseshk.",
        ".khSssssShk.",
        "..kssssssk..",
        "...kssssk...",
    ],
    "trainer.up": [
        "....kkkk....",
        "..kkrrrrkk..",
        ".krrrrrrrrk.",
        ".krRRRRRRrk.",
        ".khhhhhhhhk.",
        ".khhhhhhhhk.",
        "..khhhhhhk..",
        "...khhhhk...",
    ],
    "healer.down": [
        "....kkkk....",
        "..kkhhhhkk..",
        ".khHHhhhhhk.",
        ".khsssssshk.",
        ".khsesseshk.",
        ".khSssssShk.",
        "..kssssssk..",
        "...kssssk...",
    ],
}

# Bodies come in three arm poses. The hands are the skin pixels at the outer
# columns; moving them one row up or down is the whole arm swing, and at twelve
# pixels wide it is all the swing there is room for.
BODY = {
    "hero.front": {
        "mid": [
            "..kcccccck..",
            ".kcccccccck.",
            "ksccccccccsk",
            "ksccccccccsk",
            ".kccwwwwcck.",
            ".kcCCCCCCck.",
            "..kppppppk..",
        ],
        "down": [
            "..kcccccck..",
            ".kcccccccck.",
            ".kcccccccck.",
            "ksccccccccsk",
            "kscwwwwwwcsk",
            ".kcCCCCCCck.",
            "..kppppppk..",
        ],
        "up": [
            "..kcccccck..",
            "ksccccccccsk",
            "ksccccccccsk",
            ".kcccccccck.",
            ".kccwwwwcck.",
            ".kcCCCCCCck.",
            "..kppppppk..",
        ],
    },
    "hero.back": {
        "mid": [
            "..kcccccck..",
            ".kcccccccck.",
            "ksccccccccsk",
            "ksccccccccsk",
            ".kcccccccck.",
            ".kcCCCCCCck.",
            "..kppppppk..",
        ],
        "down": [
            "..kcccccck..",
            ".kcccccccck.",
            ".kcccccccck.",
            "ksccccccccsk",
            "ksccccccccsk",
            ".kcCCCCCCck.",
            "..kppppppk..",
        ],
        "up": [
            "..kcccccck..",
            "ksccccccccsk",
            "ksccccccccsk",
            ".kcccccccck.",
            ".kcccccccck.",
            ".kcCCCCCCck.",
            "..kppppppk..",
        ],
    },
    "hero.side": {
        "mid": [
            "..kCcccccck.",
            ".kCcccccccck",
            ".kCcccccccsk",
            ".kCcccccccsk",
            ".kCcwwwwcck.",
            ".kCCCCCCCck.",
            "..kPppppppk.",
        ],
        "down": [
            "..kCcccccck.",
            ".kCcccccccck",
            ".kCcccccccck",
            ".kCcccccccsk",
            ".kCcwwwwccsk",
            ".kCCCCCCCck.",
            "..kPppppppk.",
        ],
        "up": [
            "..kCcccccck.",
            ".kCcccccccsk",
            ".kCcccccccsk",
            ".kCcccccccck",
            ".kCcwwwwcck.",
            ".kCCCCCCCck.",
            "..kPppppppk.",
        ],
    },
    "villager.front": {
        "mid": [
            "..khcccchk..",
            ".khcccccchk.",
            "kshcccccchsk",
            "ksccccccccsk",
            ".kccwwwwcck.",
            ".kcCCCCCCck.",
            "..kcccccck..",
        ],
        "down": [
            "..khcccchk..",
            ".khcccccchk.",
            ".khcccccchk.",
            "ksccccccccsk",
            ".kccwwwwcck.",
            ".kcCCCCCCck.",
            "..kcccccck..",
        ],
        "up": [
            "..khcccchk..",
            "kshcccccchsk",
            "kshcccccchsk",
            ".kcccccccck.",
            ".kccwwwwcck.",
            ".kcCCCCCCck.",
            "..kcccccck..",
        ],
    },
    "healer.front": {
        "mid": [
            "..kcccccck..",
            ".kcccccccck.",
            "kscccwwcccsk",
            "kscwwwwwwcsk",
            ".kcccwwccck.",
            ".kcCCCCCCck.",
            "..kppppppk..",
        ],
        "down": [
            "..kcccccck..",
            ".kcccccccck.",
            ".kcccwwccck.",
            "kscwwwwwwcsk",
            ".kcccwwccck.",
            ".kcCCCCCCck.",
            "..kppppppk..",
        ],
        "up": [
            "..kcccccck..",
            "kscccwwcccsk",
            "kscwwwwwwcsk",
            ".kcccwwccck.",
            ".kcccccccck.",
            ".kcCCCCCCck.",
            "..kppppppk..",
        ],
    },
}
BODY["trainer.front"] = BODY["hero.front"]
BODY["trainer.back"] = BODY["hero.back"]

LEGS = {
    "trousers": {
        "stand": [
            "..kppkkppk..",
            "..kppkkppk..",
            "..kPPkkPPk..",
            "..kbBkkBbk..",
            "..kkk..kkk..",
        ],
        "apart": [
            "..kppkkppk..",
            ".kppk..kppk.",
            ".kPPk..kPPk.",
            ".kbBk..kBbk.",
            ".kkk....kkk.",
        ],
        "together": [
            "..kppkkppk..",
            "..kppppppk..",
            "..kPPPPPPk..",
            "..kbBBBBbk..",
            "...kkkkkk...",
        ],
    },
    "skirt": {
        "stand": [
            ".kcccccccck.",
            "kcccccccccck",
            "kcCCCCCCCCck",
            "..kbBkkBbk..",
            "..kkk..kkk..",
        ],
        "apart": [
            ".kcccccccck.",
            "kcccccccccck",
            "kcCCCCCCCCck",
            ".kbBk..kBbk.",
            ".kkk....kkk.",
        ],
        "together": [
            ".kcccccccck.",
            "kcccccccccck",
            "kcCCCCCCCCck",
            "..kbBBBBbk..",
            "...kkkkkk...",
        ],
    },
    "shorts": {
        "stand": [
            "..kppkkppk..",
            "..ksskkssk..",
            "..kSSkkSSk..",
            "..kbBkkBbk..",
            "..kkk..kkk..",
        ],
        "apart": [
            "..kppkkppk..",
            ".kssk..kssk.",
            ".kSSk..kSSk.",
            ".kbBk..kBbk.",
            ".kkk....kkk.",
        ],
        "together": [
            "..kppkkppk..",
            "..kssssssk..",
            "..kSSSSSSk..",
            "..kbBBBBbk..",
            "...kkkkkk...",
        ],
    },
}

# --- composition ----------------------------------------------------------

# A walk reads from four beats, not two: step, pass, step, pass. Storing the
# passing pose twice is what lets the Aseprite tag play the cycle correctly
# when you open the file, rather than only looking right inside the game.
CYCLE = [("apart", "down"), ("stand", "mid"), ("together", "up"), ("stand", "mid")]

SHEETS = {
    "hero": {
        "palette": "hero",
        "legs": "trousers",
        "dirs": [("down", "hero.down", "hero.front"),
                 ("up", "hero.up", "hero.back"),
                 ("side", "hero.side", "hero.side")],
    },
    "villager": {
        "palette": "villager",
        "legs": "skirt",
        "dirs": [("down", "villager.down", "villager.front")],
    },
    "trainer": {
        "palette": "trainer",
        "legs": "shorts",
        "dirs": [("down", "trainer.down", "trainer.front"),
                 ("up", "trainer.up", "trainer.back")],
    },
    "healer": {
        "palette": "healer",
        "legs": "trousers",
        "dirs": [("down", "healer.down", "healer.front")],
    },
}


def check(rows, name):
    for i, r in enumerate(rows):
        if len(r) != W:
            raise SystemExit(f"{name} row {i} is {len(r)} wide, expected {W}: {r!r}")


def compose(head, body, legs):
    check(head, "head")
    check(body, "body")
    check(legs, "legs")
    grid = [["."] * W for _ in range(H)]
    for y, row in enumerate(head):
        for x, ch in enumerate(row):
            if ch != ".":
                grid[y][x] = ch
    for y, row in enumerate(body):
        for x, ch in enumerate(row):
            if ch != ".":
                grid[8 + y][x] = ch
    for y, row in enumerate(legs):
        for x, ch in enumerate(row):
            if ch != ".":
                grid[15 + y][x] = ch
    return grid


# Layers, bottom to top. Splitting on what a pixel is for rather than where it
# sits means recolouring a jacket or softening the outline is one layer's work.
LAYERS = ["fill", "shade", "outline"]
SHADE_CHARS = set("SCPHR")


def layer_of(ch):
    if ch in "ke":
        return 2
    if ch in SHADE_CHARS:
        return 1
    return 0


def rasterize(grid, pal):
    planes = [bytearray(W * H * 4) for _ in LAYERS]
    for y in range(H):
        for x in range(W):
            ch = grid[y][x]
            if ch == ".":
                continue
            rgb = pal.get(ch)
            if rgb is None:
                raise SystemExit(f"no palette entry for {ch!r}")
            p = planes[layer_of(ch)]
            i = (y * W + x) * 4
            p[i], p[i + 1], p[i + 2], p[i + 3] = rgb[0], rgb[1], rgb[2], 255
    return [bytes(p) for p in planes]


def flatten(planes):
    out = bytearray(W * H * 4)
    for p in planes:
        for i in range(0, len(p), 4):
            if p[i + 3]:
                out[i:i + 4] = p[i:i + 4]
    return bytes(out)


def write_png(path, width, height, rgba):
    def chunk(tag, data):
        return (struct.pack(">I", len(data)) + tag + data
                + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF))
    raw = b"".join(b"\0" + rgba[y * width * 4:(y + 1) * width * 4]
                   for y in range(height))
    png = (b"\x89PNG\r\n\x1a\n"
           + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0))
           + chunk(b"IDAT", zlib.compress(raw, 9))
           + chunk(b"IEND", b""))
    with open(path, "wb") as f:
        f.write(png)
    return len(png)


# The device cannot afford 32 bit pixels: 28 frames of 12 x 20 RGBA is 26 KB of
# flash for four characters. Indexed at four bits against a per sheet palette is
# 120 bytes a frame, 3.4 KB for all of it, and the art is flat colour so the
# palette costs nothing in quality.
CPP_FRAMES = {}


def emit_cpp(manifest, out_dir):
    hpp = ["// Generated by art/build_art.py. Do not edit, and do not commit.",
           "#pragma once", "", "#include <cstdint>", "", "namespace pm {", "",
           "// One sheet: a palette of at most 16 colours where entry 0 is",
           "// transparent, and frames packed two pixels to a byte.",
           "struct SpriteSheet {",
           "    const uint8_t* palette;   // 3 bytes an entry",
           "    const uint8_t* pixels;    // frame_count * (w * h / 2)",
           "    uint8_t w, h, frame_count;",
           "};", "",
           "extern const SpriteSheet k_sheets[];",
           f"constexpr int k_sheet_art_count = {len(manifest)};", ""]
    for i, name in enumerate(manifest):
        hpp.append(f"constexpr int art_{name} = {i};")
    hpp += ["", "}  // namespace pm"]

    cpp = ["// Generated by art/build_art.py. Do not edit, and do not commit.",
           '#include "sprites.hpp"', "", "namespace pm {", ""]
    for name in manifest:
        pal, frames = CPP_FRAMES[name]
        cpp.append(f"static const uint8_t k_pal_{name}[] = {{")
        for c in pal:
            cpp.append(f"    0x{c[0]:02X}, 0x{c[1]:02X}, 0x{c[2]:02X},")
        cpp.append("};")
        cpp.append(f"static const uint8_t k_px_{name}[] = {{")
        for fr in frames:
            row = ", ".join(f"0x{b:02X}" for b in fr)
            cpp.append(f"    {row},")
        cpp.append("};")
        cpp.append("")
    cpp.append("const SpriteSheet k_sheets[] = {")
    for name, m in manifest.items():
        cpp.append(f"    {{k_pal_{name}, k_px_{name}, {m['frame_w']}, "
                   f"{m['frame_h']}, {m['frames']}}},")
    cpp.append("};")
    cpp += ["", "}  // namespace pm"]

    os.makedirs(out_dir, exist_ok=True)
    with open(os.path.join(out_dir, "sprites.hpp"), "w") as f:
        f.write("\n".join(hpp) + "\n")
    with open(os.path.join(out_dir, "sprites.cpp"), "w") as f:
        f.write("\n".join(cpp) + "\n")
    total = sum(len(fr) for _, frames in CPP_FRAMES.values() for fr in frames)
    print(f"sprites.cpp  {total} bytes of pixels, "
          f"{sum(len(p) for p, _ in CPP_FRAMES.values()) * 3} bytes of palettes")


def pack_indexed(flat_frames, name):
    """Quantise a sheet's RGBA frames to one shared 16 entry palette."""
    palette = [(0, 0, 0)]          # entry 0 is transparent
    lookup = {}
    packed = []
    for fr in flat_frames:
        out = bytearray()
        for i in range(0, len(fr), 8):     # two pixels a byte
            def index(off):
                a = fr[i + off + 3]
                if not a:
                    return 0
                key = (fr[i + off], fr[i + off + 1], fr[i + off + 2])
                if key not in lookup:
                    if len(palette) >= 16:
                        raise SystemExit(f"{name}: more than 15 colours")
                    lookup[key] = len(palette)
                    palette.append(key)
                return lookup[key]
            out.append((index(0) << 4) | index(4))
        packed.append(bytes(out))
    while len(palette) < 16:
        palette.append((0, 0, 0))
    return palette, packed


def main():
    # The device tables go wherever the build asks for them, because nothing
    # generated is ever committed. The .aseprite and .png files are art and
    # stay here; rewriting them is harmless because the output is byte for
    # byte deterministic.
    ap = argparse.ArgumentParser()
    ap.add_argument("--out-dir", default=HERE)
    args = ap.parse_args()
    manifest = {}
    js = {}
    for name, spec in SHEETS.items():
        pal = PALETTES[spec["palette"]]
        legs = LEGS[spec["legs"]]
        frames, tags, flat = [], [], []
        for dir_name, head_key, body_key in spec["dirs"]:
            first = len(frames)
            for leg_pose, arm_pose in CYCLE:
                grid = compose(HEAD[head_key], BODY[body_key][arm_pose],
                               legs[leg_pose])
                planes = rasterize(grid, pal)
                frames.append(planes)
                flat.append(flatten(planes))
            tags.append((dir_name, first, len(frames) - 1))

        ase = os.path.join(HERE, f"{name}.aseprite")
        size = aseprite.write(ase, W, H, LAYERS, frames, tags)

        strip = bytearray(W * len(flat) * H * 4)
        for fi, fr in enumerate(flat):
            for y in range(H):
                src = fr[y * W * 4:(y + 1) * W * 4]
                dst = (y * W * len(flat) + fi * W) * 4
                strip[dst:dst + W * 4] = src
        png = os.path.join(HERE, f"{name}.png")
        pngsize = write_png(png, W * len(flat), H, bytes(strip))

        manifest[name] = {
            "frame_w": W, "frame_h": H, "frames": len(flat),
            "tags": {t[0]: [t[1], t[2]] for t in tags},
        }
        with open(png, "rb") as f:
            js[name] = "data:image/png;base64," + base64.b64encode(f.read()).decode()
        CPP_FRAMES[name] = pack_indexed(flat, name)
        print(f"{name:9s} {len(flat):2d} frames  {name}.aseprite {size:5d} B"
              f"  {name}.png {pngsize:4d} B")

    emit_cpp(manifest, args.out_dir)
    with open(os.path.join(HERE, "sheets.json"), "w") as f:
        json.dump(manifest, f, indent=2)
        f.write("\n")
    with open(os.path.join(HERE, "sheets.js"), "w") as f:
        f.write("// Generated by build_art.py. Paste into index.html so the\n"
                "// mockup stays one self contained file.\n")
        f.write("const SHEETS = " + json.dumps(
            {k: dict(manifest[k], src=js[k]) for k in manifest}, indent=1) + ";\n")
    print("wrote sheets.json and sheets.js")


if __name__ == "__main__":
    main()
