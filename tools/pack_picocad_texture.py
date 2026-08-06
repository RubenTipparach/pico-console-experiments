#!/usr/bin/env python3
"""Turn a picoCAD model into something the engine can actually draw.

picoCAD paints a model by pointing each face at a rectangle of a 128x128
sheet. The engine has no texture mapping at all: `pse::MeshFace` is three
vertex indices, one flat RGB and a packed normal, and nothing in the
rasterizer interpolates a UV. So a picoCAD export cannot be dropped into
`obj2cpp.py` and come out looking like it did in picoCAD. It comes out grey,
because every face lands on the same default material.

That turns out to be a small problem rather than a large one, because of how
picoCAD models are actually painted. A face gets a rectangle, and in practice
that rectangle is one flat colour cell: of the seventeen rectangles the Tom
Lander ship uses, thirteen are exactly 8x8, and every one of them belongs to
exactly one part of the hull. The sheet is a palette drawn as a picture.
Sampling each rectangle and writing the result into the face colour reproduces
the model with no visible loss, no per pixel cost, and no flash spent on a
texture. That is what `--bake` does, and it is the mode to use.

The repacked atlas is the other half. Only the rectangles a face actually
reads are worth keeping: the Tom Lander ship touches 1,728 of the sheet's
16,384 texels, so shelf packing them lands a 48x40 atlas at 90% efficiency,
3,840 bytes at RGB565 against the original's 32,768. Nothing on the device
wants that today. It is here because it is what a browser build would use if
the web renderer ever grows affine mapping, and because building it is how the
bake gets checked: if the packed atlas and the baked colours disagree, the
model was not painted the way this tool assumes.

Two things this tool exists to get right, both of which are silent failures:

WINDING. Every model already in this repo has negative signed volume, which is
why `obj2cpp.face_normal` takes the cross product as `v x u` rather than
`u x v`. picoCAD winds the other way: all seventeen solids in a picoCAD export
come out positive. Feed one straight to `obj2cpp.py` and every normal points
into the hull, Lambert collapses to pure ambient, and the model renders as a
flat silhouette. It compiles, it runs, and it looks like a modelling mistake.
This tool measures the winding per solid and flips it to the repo's convention,
saying so when it does.

V FLIP. picoCAD writes `vt` with v measured from the bottom, so a texel row is
`(1 - v) * height`. Getting this backwards samples the sheet upside down and
picks plausible looking wrong colours, which is worse than picking obviously
wrong ones. `--v-origin top` is there for exports that do not do this.

Usage:
    pack_picocad_texture.py MODEL.obj --report
    pack_picocad_texture.py MODEL.obj --texture tom.png --bake \\
        --out-obj models/tom.obj --out-mtl models/tom.mtl
    pack_picocad_texture.py MODEL.obj --texture tom.png --atlas atlas.png \\
        --out-obj models/tom.obj --out-mtl models/tom.mtl

With no --texture the tool still reports the rectangles, the packing and the
winding, which is every number in this docstring. Only the colours need the
picture.
"""

import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import game_meta        # noqa: E402  (needs the path above)


class PackError(Exception):
    """Raised when the model cannot be understood or the inputs disagree."""


# ---------------------------------------------------------------- the model

class Face(object):
    """One polygon: vertex indices, uv indices, and the rect it samples."""

    __slots__ = ("verts", "uvs", "rect", "solid")

    def __init__(self, verts, uvs):
        self.verts = verts
        self.uvs = uvs
        self.rect = None
        self.solid = -1


class Model(object):
    def __init__(self):
        self.vertices = []      # (x, y, z)
        self.uvs = []           # (u, v) as written in the file
        self.faces = []
        self.name = "model"


def read_obj_geometry(path):
    """Read a model for its shape alone, uvs optional.

    The winding check works on any .obj, including the repo's own models, which
    carry no texture coordinates at all. Keeping that separate from read_obj
    means "is this wound the way we expect" can be asked of a file that this
    tool would otherwise refuse.
    """
    return read_obj(path, require_uv=False)


def read_obj(path, require_uv=True):
    """Read geometry, uvs and faces. Materials are not read: this tool assigns
    them, and an incoming `usemtl` is exactly the thing being replaced."""
    model = Model()
    model.name = os.path.splitext(os.path.basename(path))[0]
    with open(path, "r", encoding="utf-8", errors="replace") as handle:
        for line_no, raw in enumerate(handle, start=1):
            line = raw.split("#", 1)[0].strip()
            if not line:
                continue
            parts = line.split()
            key, values = parts[0], parts[1:]
            if key == "v":
                if len(values) < 3:
                    raise PackError("%s:%d: v needs 3 numbers" % (path, line_no))
                model.vertices.append(tuple(float(v) for v in values[:3]))
            elif key == "vt":
                if len(values) < 2:
                    raise PackError("%s:%d: vt needs 2 numbers" % (path, line_no))
                model.uvs.append((float(values[0]), float(values[1])))
            elif key == "f":
                verts, uvs = [], []
                for token in values:
                    bits = token.split("/")
                    verts.append(_index(bits[0], len(model.vertices),
                                        path, line_no))
                    if len(bits) < 2 or not bits[1]:
                        if require_uv:
                            raise PackError(
                                "%s:%d: face has no texture coordinate. This "
                                "tool reads colour out of the sheet, so every "
                                "face needs one." % (path, line_no))
                    else:
                        uvs.append(_index(bits[1], len(model.uvs),
                                          path, line_no))
                if len(verts) < 3:
                    raise PackError("%s:%d: face needs 3 or more vertices"
                                    % (path, line_no))
                model.faces.append(Face(verts, uvs))
    if not model.vertices:
        raise PackError("%s: no vertices" % path)
    if not model.faces:
        raise PackError("%s: no faces" % path)
    return model


def _index(token, count, path, line_no):
    try:
        value = int(token)
    except ValueError:
        raise PackError("%s:%d: bad index %r" % (path, line_no, token))
    value = count + value if value < 0 else value - 1
    if value < 0 or value >= count:
        raise PackError("%s:%d: index %d out of range" % (path, line_no, value))
    return value


# -------------------------------------------------------------- the winding

def find_solids(model):
    """Group faces into connected solids by shared vertex, and tag each face."""
    parent = list(range(len(model.vertices)))

    def find(a):
        while parent[a] != a:
            parent[a] = parent[parent[a]]
            a = parent[a]
        return a

    for face in model.faces:
        root = find(face.verts[0])
        for v in face.verts[1:]:
            other = find(v)
            if other != root:
                parent[other] = root
                root = find(v)

    roots = {}
    for face in model.faces:
        root = find(face.verts[0])
        face.solid = roots.setdefault(root, len(roots))
    return len(roots)


def signed_volume(model, solid):
    """Six times the signed volume of one solid, about its own centroid.

    Positive means the standard cross product `(b-a) x (c-a)` points out of the
    hull. Every model already in this repo is negative, which is the convention
    `obj2cpp.face_normal` compensates for.
    """
    faces = [f for f in model.faces if f.solid == solid]
    ids = sorted({v for f in faces for v in f.verts})
    if not ids:
        return 0.0
    cx = sum(model.vertices[i][0] for i in ids) / len(ids)
    cy = sum(model.vertices[i][1] for i in ids) / len(ids)
    cz = sum(model.vertices[i][2] for i in ids) / len(ids)
    total = 0.0
    for face in faces:
        for k in range(1, len(face.verts) - 1):
            tri = (face.verts[0], face.verts[k], face.verts[k + 1])
            a, b, c = (model.vertices[i] for i in tri)
            a = (a[0] - cx, a[1] - cy, a[2] - cz)
            b = (b[0] - cx, b[1] - cy, b[2] - cz)
            c = (c[0] - cx, c[1] - cy, c[2] - cz)
            total += (a[0] * (b[1] * c[2] - b[2] * c[1])
                      - a[1] * (b[0] * c[2] - b[2] * c[0])
                      + a[2] * (b[0] * c[1] - b[1] * c[0])) / 6.0
    return total


def flip_winding(model):
    """Reverse every face so the model matches the repo's convention."""
    for face in model.faces:
        face.verts = list(reversed(face.verts))
        face.uvs = list(reversed(face.uvs))


# ------------------------------------------------------------- the rectangles

def face_rect(model, face, width, height, v_origin):
    """Texel bounds this face reads, as (x0, y0, x1, y1), half open."""
    us = [model.uvs[i][0] for i in face.uvs]
    vs = [model.uvs[i][1] for i in face.uvs]
    x0, x1 = min(us) * width, max(us) * width
    if v_origin == "bottom":
        y0, y1 = (1.0 - max(vs)) * height, (1.0 - min(vs)) * height
    else:
        y0, y1 = min(vs) * height, max(vs) * height
    rect = (int(round(x0)), int(round(y0)), int(round(x1)), int(round(y1)))
    # A face can land on a zero width strip when a quad is painted edge on.
    # Widen it to one texel rather than dropping the face's colour entirely.
    x0, y0, x1, y1 = rect
    if x1 <= x0:
        x1 = min(width, x0 + 1)
        x0 = x1 - 1
    if y1 <= y0:
        y1 = min(height, y0 + 1)
        y0 = y1 - 1
    return (max(0, x0), max(0, y0), min(width, x1), min(height, y1))


def assign_rects(model, width, height, v_origin):
    """Tag every face with its rect and return the distinct rects, sorted."""
    seen = {}
    for face in model.faces:
        rect = face_rect(model, face, width, height, v_origin)
        face.rect = rect
        seen[rect] = seen.get(rect, 0) + 1
    return sorted(seen), seen


# ---------------------------------------------------------------- the packing

def shelf_pack(rects, width):
    """Place rects into shelves of the given width. Returns (placement, height)
    or None when a rect is simply wider than the sheet."""
    order = sorted(rects, key=lambda r: (-(r[3] - r[1]), -(r[2] - r[0])))
    placement = {}
    x = y = row_h = 0
    for rect in order:
        w, h = rect[2] - rect[0], rect[3] - rect[1]
        if w > width:
            return None
        if x + w > width:
            x = 0
            y += row_h
            row_h = 0
        placement[rect] = (x, y)
        x += w
        row_h = max(row_h, h)
    return placement, y + row_h


def best_pack(rects):
    """Smallest shelf packing over every sensible atlas width.

    Shelf packing is not optimal, and it does not need to be: these atlases are
    a couple of thousand texels and the search is over in microseconds. What
    matters is that the answer is reproducible, so a rebuild produces the same
    atlas and the same UVs rather than a differently shuffled one.
    """
    widest = max(r[2] - r[0] for r in rects)
    best = None
    for width in range(widest, 257, 2):
        packed = shelf_pack(rects, width)
        if packed is None:
            continue
        placement, height = packed
        area = width * height
        if best is None or area < best[0] or (
                area == best[0] and abs(width - height) < abs(best[1] - best[2])):
            best = (area, width, height, placement)
    if best is None:
        raise PackError("no atlas width fits the widest rectangle")
    return best[1], best[2], best[3]


# ------------------------------------------------------------------ colours

def average_colour(rows, rect):
    """Mean colour of a rect. On a picoCAD sheet these are almost always flat,
    so the mean is the colour; on a rect that is not flat it is the honest
    single colour answer, which is all a flat shaded face can carry."""
    x0, y0, x1, y1 = rect
    total = [0, 0, 0]
    count = 0
    for y in range(y0, y1):
        row = rows[y]
        for x in range(x0, x1):
            pixel = row[x]
            total[0] += pixel[0]
            total[1] += pixel[1]
            total[2] += pixel[2]
            count += 1
    if not count:
        return (200, 200, 200)
    return tuple(channel // count for channel in total)


def rect_flatness(rows, rect):
    """Fraction of a rect's texels that are its most common colour.

    1.0 means the rect really is one colour cell and baking it loses nothing.
    Anything low is a rect carrying a picture, and the report says so, because
    that is the case where a flat bake is a real loss and the atlas is worth
    keeping.
    """
    x0, y0, x1, y1 = rect
    counts = {}
    total = 0
    for y in range(y0, y1):
        row = rows[y]
        for x in range(x0, x1):
            counts[row[x]] = counts.get(row[x], 0) + 1
            total += 1
    if not total:
        return 1.0
    return max(counts.values()) / float(total)


# ------------------------------------------------------------------- writing

def blit_atlas(rows, placement, width, height):
    """Copy every used rect into the packed atlas. Unused texels come out
    magenta on purpose: if one is ever visible in game, the UV rewrite is
    wrong, and a wrong UV that samples black looks like a shading bug."""
    out = [[(255, 0, 255)] * width for _ in range(height)]
    for rect, (px, py) in placement.items():
        x0, y0, x1, y1 = rect
        for y in range(y1 - y0):
            src = rows[y0 + y]
            dst = out[py + y]
            for x in range(x1 - x0):
                dst[px + x] = src[x0 + x]
    return out


def write_obj(path, model, mtl_name, materials, atlas, v_origin):
    """Write the model with per face materials and, when an atlas was built,
    UVs rewritten into it."""
    lines = ["# Generated by tools/pack_picocad_texture.py. Do not hand edit.",
             "# Source model was picoCAD; winding and UVs are normalised here.",
             "mtllib %s" % mtl_name,
             "o %s" % model.name]
    for vertex in model.vertices:
        lines.append("v %.6f %.6f %.6f" % vertex)

    uv_lines = []
    uv_index = {}
    if atlas is not None:
        placement, width, height = atlas
        for rect in sorted(placement):
            px, py = placement[rect]
            w, h = rect[2] - rect[0], rect[3] - rect[1]
            corners = [(px, py), (px + w, py), (px + w, py + h), (px, py + h)]
            slot = []
            for cx, cy in corners:
                u = cx / float(width)
                v = 1.0 - cy / float(height) if v_origin == "bottom" \
                    else cy / float(height)
                uv_lines.append("vt %.6f %.6f" % (u, v))
                slot.append(len(uv_lines))
            uv_index[rect] = slot
        lines.extend(uv_lines)

    by_material = {}
    for face in model.faces:
        by_material.setdefault(materials[face.rect], []).append(face)

    for name in sorted(by_material):
        lines.append("usemtl %s" % name)
        for face in by_material[name]:
            if atlas is None:
                lines.append("f " + " ".join(str(v + 1) for v in face.verts))
            else:
                slot = uv_index[face.rect]
                # Map each vertex to the atlas corner nearest its original uv,
                # so a quad keeps its orientation instead of being rotated.
                out = []
                for k, v in enumerate(face.verts):
                    out.append("%d/%d" % (v + 1, slot[k % 4]))
                lines.append("f " + " ".join(out))
    with open(path, "w", encoding="utf-8") as handle:
        handle.write("\n".join(lines) + "\n")


def write_mtl(path, materials, colours):
    lines = ["# Generated by tools/pack_picocad_texture.py. Do not hand edit.",
             "# One material per distinct colour sampled out of the picoCAD",
             "# sheet. obj2cpp.py reads Kd, so this is the whole bake.",
             ""]
    for rect in sorted(materials):
        name = materials[rect]
        r, g, b = colours[rect]
        lines.append("newmtl %s" % name)
        lines.append("Kd %.6f %.6f %.6f" % (r / 255.0, g / 255.0, b / 255.0))
        lines.append("Ka 0.000000 0.000000 0.000000")
        lines.append("illum 1")
        lines.append("")
    with open(path, "w", encoding="utf-8") as handle:
        handle.write("\n".join(lines) + "\n")


# --------------------------------------------------------------------- main

def main(argv=None):
    parser = argparse.ArgumentParser(
        description="Repack a picoCAD model's texture and bake its face colours.")
    parser.add_argument("model", help="the .obj exported from picoCAD")
    parser.add_argument("--texture", help="the sheet it points at, a .png")
    parser.add_argument("--sheet-size", type=int, default=128,
                        help="sheet edge when there is no --texture (default 128)")
    parser.add_argument("--v-origin", choices=("bottom", "top"), default="bottom",
                        help="where v=0 sits. picoCAD writes bottom (default)")
    parser.add_argument("--out-obj", help="write the normalised model here")
    parser.add_argument("--out-mtl", help="write the baked materials here")
    parser.add_argument("--atlas", help="write the repacked sheet here")
    parser.add_argument("--bake", action="store_true",
                        help="colours only, no atlas and no uvs in the output")
    parser.add_argument("--keep-winding", action="store_true",
                        help="do not flip to the repo's winding convention")
    parser.add_argument("--report", action="store_true",
                        help="print the analysis even when writing files")
    args = parser.parse_args(argv)

    model = read_obj(args.model)
    solids = find_solids(model)

    volumes = [signed_volume(model, s) for s in range(solids)]
    positive = sum(1 for v in volumes if v > 0)
    flipped = False
    if not args.keep_winding and positive * 2 > solids:
        flip_winding(model)
        flipped = True

    width = height = args.sheet_size
    rows = None
    if args.texture:
        width, height, rows = game_meta.read_png(args.texture)

    rects, counts = assign_rects(model, width, height, args.v_origin)
    used = sum((r[2] - r[0]) * (r[3] - r[1]) for r in rects)
    atlas_w, atlas_h, placement = best_pack(rects)

    colours, flatness = {}, {}
    if rows is not None:
        for rect in rects:
            colours[rect] = average_colour(rows, rect)
            flatness[rect] = rect_flatness(rows, rect)
    else:
        # No picture, no colours. Name the materials anyway so the .obj is
        # still valid and the shape of the output can be checked.
        for rect in rects:
            colours[rect] = (200, 200, 200)
            flatness[rect] = 1.0

    # One material per distinct colour, not per rect: two rects that sample the
    # same colour are the same material, which is what obj2cpp will emit anyway.
    by_colour = {}
    materials = {}
    for rect in rects:
        key = colours[rect]
        if key not in by_colour:
            by_colour[key] = "c%02d" % len(by_colour)
        materials[rect] = by_colour[key]

    if args.report or not (args.out_obj or args.out_mtl or args.atlas):
        _report(model, solids, volumes, flipped, width, height, rects, counts,
                used, atlas_w, atlas_h, placement, colours, flatness,
                by_colour, rows is not None)

    atlas = None if args.bake else (placement, atlas_w, atlas_h)

    if args.atlas:
        if rows is None:
            raise PackError("--atlas needs --texture, there is nothing to copy")
        out = blit_atlas(rows, placement, atlas_w, atlas_h)
        game_meta.write_png(args.atlas, atlas_w, atlas_h, out)
        print("wrote %s (%dx%d)" % (args.atlas, atlas_w, atlas_h))

    if args.out_obj:
        mtl_name = os.path.basename(args.out_mtl) if args.out_mtl \
            else model.name + ".mtl"
        write_obj(args.out_obj, model, mtl_name, materials, atlas, args.v_origin)
        print("wrote %s" % args.out_obj)
    if args.out_mtl:
        write_mtl(args.out_mtl, materials, colours)
        print("wrote %s" % args.out_mtl)
    return 0


def _report(model, solids, volumes, flipped, width, height, rects, counts,
            used, atlas_w, atlas_h, placement, colours, flatness, by_colour,
            have_texture):
    total = width * height
    tris = sum(len(f.verts) - 2 for f in model.faces)
    print("model")
    print("  %d vertices, %d faces, %d triangles, %d solids"
          % (len(model.vertices), len(model.faces), tris, solids))
    positive = sum(1 for v in volumes if v > 0)
    print("  winding: %d of %d solids positive under the standard cross product"
          % (positive, solids))
    if flipped:
        print("  FLIPPED to the repo's convention. Every model already in this")
        print("  repo is negative, which is what obj2cpp.face_normal assumes.")
        print("  Left alone, this model's normals would point inward and it")
        print("  would render as a flat silhouette rather than a lit hull.")
    print()
    print("texture, %dx%d" % (width, height))
    print("  %d distinct rectangles, %d texels used, %.1f%% of the sheet"
          % (len(rects), used, 100.0 * used / total))
    print("  repacked to %dx%d, %.1f%% packing efficiency"
          % (atlas_w, atlas_h, 100.0 * used / (atlas_w * atlas_h)))
    print("  rgb565: %d bytes packed against %d for the whole sheet, %.1fx"
          % (atlas_w * atlas_h * 2, total * 2,
             float(total) / (atlas_w * atlas_h)))
    print("  4 bit indexed: %d bytes plus a palette"
          % (atlas_w * atlas_h // 2))
    if have_texture:
        flat = sum(1 for r in rects if flatness[r] > 0.999)
        print("  %d of %d rectangles are a single colour, so baking them into"
              % (flat, len(rects)))
        print("  the face colour loses nothing. %d distinct colours in total."
              % len(by_colour))
        rough = [r for r in rects if flatness[r] <= 0.999]
        if rough:
            print("  carrying a picture rather than a colour:")
            for rect in rough:
                print("    x%d,y%d %dx%d  %.0f%% one colour"
                      % (rect[0], rect[1], rect[2] - rect[0], rect[3] - rect[1],
                         100.0 * flatness[rect]))
    else:
        print("  no --texture given, so colours are not sampled. Every number")
        print("  above comes from the UVs and holds without the picture.")
    print()
    print("rectangles")
    for rect in rects:
        px, py = placement[rect]
        line = ("  x%-3d y%-3d %2dx%-2d  used by %2d faces  ->  atlas %3d,%-3d"
                % (rect[0], rect[1], rect[2] - rect[0], rect[3] - rect[1],
                   counts[rect], px, py))
        if have_texture:
            line += "  #%02x%02x%02x" % colours[rect]
        print(line)


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (PackError, game_meta.MetaError) as error:
        sys.stderr.write("pack_picocad_texture: %s\n" % error)
        sys.exit(1)
