"""Parse the generated .aseprite files back from bytes and check that the
flattened result matches the exported PNG pixel for pixel.

Run after build_art.py:

    python3 verify.py

If this passes, the chunk sizes, offsets and compressed cels are all
internally consistent, which is what Aseprite itself checks when it opens a
file. Writing a binary format by hand without a reader to argue with it is how
you ship a file nobody can open."""
import os
import struct
import sys
import zlib

from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))


def read(path):
    d = open(path, "rb").read()
    (size, magic, nframes, w, h, depth, flags, speed,
     _z1, _z2, tindex, _ig, ncolors, pw, ph, gx, gy, gw, gh, _res) = struct.unpack(
        "<IHHHHHIHIIB3sHBBhhHH84s", d[:128])
    assert magic == 0xA5E0, hex(magic)
    assert size == len(d), (size, len(d))
    assert depth == 32, depth
    print(f"{path}: {nframes} frames, {w}x{h}, depth {depth}, size {size} == file {len(d)}")

    off = 128
    layers, frames, tags = [], [], []
    for fi in range(nframes):
        fsize, fmagic, oldn, dur, _r, nchunks = struct.unpack("<IHHH2sI", d[off:off + 16])
        assert fmagic == 0xF1FA, hex(fmagic)
        end = off + fsize
        p = off + 16
        cels = []
        for _ in range(nchunks):
            csize, ctype = struct.unpack("<IH", d[p:p + 6])
            body = d[p + 6:p + csize]
            if ctype == 0x2004:
                nlen = struct.unpack("<H", body[16:18])[0]
                layers.append(body[18:18 + nlen].decode())
            elif ctype == 0x2005:
                li, cx, cy, op, ct, zi = struct.unpack("<HhhBHh", body[:11])
                assert ct == 2, ct
                cw, ch = struct.unpack("<HH", body[16:20])
                px = zlib.decompress(body[20:])
                assert len(px) == cw * ch * 4, (len(px), cw * ch * 4)
                cels.append((li, cx, cy, cw, ch, px))
            elif ctype == 0x2018:
                n = struct.unpack("<H", body[:2])[0]
                q = 10
                for _t in range(n):
                    fr, to = struct.unpack("<HH", body[q:q + 4])
                    nl = struct.unpack("<H", body[q + 17:q + 19])[0]
                    nm = body[q + 19:q + 19 + nl].decode()
                    tags.append((nm, fr, to))
                    q += 19 + nl
            p += csize
        assert p == end, (p, end)
        frames.append(cels)
        off = end
    assert off == len(d), (off, len(d))
    return w, h, layers, frames, tags


# Every sheet build_art.py writes, read back out of the directory rather
# than listed here: a new sheet that nobody verifies is the one that ships
# broken.
import json as _json
with open(os.path.join(HERE, "sheets.json")) as _f:
    _names = sorted(_json.load(_f))
for name in _names:
    base = os.path.join(HERE, name)
    w, h, layers, frames, tags = read(base + ".aseprite")
    print("   layers:", layers, " tags:", tags)

    sheet = Image.open(base + ".png").convert("RGBA")
    assert sheet.size == (w * len(frames), h), (sheet.size, w * len(frames), h)

    bad = 0
    for fi, cels in enumerate(frames):
        flat = bytearray(w * h * 4)
        for (li, cx, cy, cw, ch, px) in sorted(cels, key=lambda c: c[0]):
            for i in range(0, len(px), 4):
                if px[i + 3]:
                    flat[i:i + 4] = px[i:i + 4]
        ref = sheet.crop((fi * w, 0, fi * w + w, h)).tobytes()
        if bytes(flat) != ref:
            bad += 1
    print(f"   {len(frames)} frames re-flattened, mismatches vs PNG: {bad}")
    if bad:
        sys.exit(1)
print("\nall files parse and round-trip clean")
