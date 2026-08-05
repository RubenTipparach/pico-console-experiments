"""Minimal writer for the Aseprite v1.3 file format.

Enough of the spec to produce real, editable .aseprite files: RGBA colour
depth, named layers, one compressed cel per layer per frame, frame durations,
and animation tags. Files written by this open in Aseprite and in LibreSprite,
and the layers and tags are the ones named here.

Reference: https://github.com/aseprite/aseprite/blob/main/docs/ase-file-specs.md
"""
import struct
import zlib

CHUNK_LAYER = 0x2004
CHUNK_CEL = 0x2005
CHUNK_COLOR_PROFILE = 0x2007
CHUNK_TAGS = 0x2018

LOOP_FORWARD = 0


def _string(s):
    b = s.encode("utf-8")
    return struct.pack("<H", len(b)) + b


def _chunk(ctype, data):
    return struct.pack("<IH", len(data) + 6, ctype) + data


def _layer_chunk(name, visible=True):
    flags = 0
    if visible:
        flags |= 1          # visible
    flags |= 2              # editable
    return _chunk(CHUNK_LAYER, struct.pack(
        "<HHHHHHB3s", flags, 0, 0, 0, 0, 0, 255, b"\0\0\0") + _string(name))


def _color_profile_chunk():
    # type 1 = sRGB, no flags, gamma unused
    return _chunk(CHUNK_COLOR_PROFILE, struct.pack("<HHI8s", 1, 0, 0, b"\0" * 8))


def _cel_chunk(layer_index, x, y, w, h, rgba_bytes):
    body = struct.pack("<HhhBHh5s", layer_index, x, y, 255, 2, 0, b"\0" * 5)
    body += struct.pack("<HH", w, h)
    body += zlib.compress(rgba_bytes, 9)
    return _chunk(CHUNK_CEL, body)


def _tags_chunk(tags):
    body = struct.pack("<H8s", len(tags), b"\0" * 8)
    for name, first, last in tags:
        body += struct.pack("<HHBH6s3sB", first, last, LOOP_FORWARD, 0,
                            b"\0" * 6, b"\0\0\0", 0)
        body += _string(name)
    return _chunk(CHUNK_TAGS, body)


def write(path, width, height, layer_names, frames, tags=(), duration_ms=140):
    """frames: list of frames; each frame is a list (one entry per layer) of
    either None (no cel) or a bytes object of width*height*4 RGBA."""
    out = []
    for fi, frame in enumerate(frames):
        chunks = []
        if fi == 0:
            chunks.append(_color_profile_chunk())
            for name in layer_names:
                chunks.append(_layer_chunk(name))
            if tags:
                chunks.append(_tags_chunk(list(tags)))
        for li, pixels in enumerate(frame):
            if pixels is None:
                continue
            chunks.append(_cel_chunk(li, 0, 0, width, height, pixels))
        body = b"".join(chunks)
        header = struct.pack("<IHHH2sI", len(body) + 16, 0xF1FA,
                             min(len(chunks), 0xFFFF), duration_ms,
                             b"\0\0", len(chunks))
        out.append(header + body)

    frames_blob = b"".join(out)
    head = struct.pack(
        "<IHHHHHIHIIB3sHBBhhHH84s",
        128 + len(frames_blob),   # file size
        0xA5E0,                   # magic
        len(frames),              # frames
        width, height,
        32,                       # colour depth: RGBA
        1,                        # flags: layer opacity is valid
        duration_ms,              # deprecated speed
        0, 0,
        0, b"\0\0\0",             # transparent index, ignore
        0,                        # number of colours
        1, 1,                     # pixel aspect
        0, 0, 16, 16,             # grid
        b"\0" * 84)
    assert len(head) == 128, len(head)
    with open(path, "wb") as f:
        f.write(head + frames_blob)
    return 128 + len(frames_blob)
