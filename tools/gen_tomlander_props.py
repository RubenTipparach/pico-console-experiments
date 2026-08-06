#!/usr/bin/env python3
"""Build Tom Lander's crate and its two building meshes.

Rule 11 says geometry lives in a `.obj` a modeller can open, not in a vertex
table in C++. These three are regular in a way that is tedious and error prone
to type by hand (a banded facade is twenty quads that all have to line up), so
this writes them, and what it writes is committed. Edit the `.obj` directly for
a one off tweak; come back here when the shape itself should change.

The engine has NO texture mapping. Every face colour a model has comes from
its `usemtl`, and that is what "textured" means for these buildings: the
facade is really cut into bands and the windows are really their own faces, so
a tower reads as a building rather than as a grey box, using nothing but the
per face colour the renderer already has.

Usage: tools/gen_tomlander_props.py [--out games/tomlander/models]
"""

import argparse
import math
import pathlib


def mtl(entries):
    """An .mtl in the shape obj2cpp reads: Kd is the face colour."""
    out = []
    for name, (r, g, b) in entries:
        out.append(f"\nnewmtl {name}")
        out.append("Ka 1.000000 1.000000 1.000000")
        out.append(f"Kd {r / 255:.6f} {g / 255:.6f} {b / 255:.6f}")
        out.append("Ks 0.000000 0.000000 0.000000")
        out.append("d 1.000000")
        out.append("illum 1")
    return "\n".join(out) + "\n"


class Mesh:
    """Vertices and material tagged quads, written out in .obj order.

    Winding matters and is not a style choice: this repo's models wind so the
    signed volume comes out NEGATIVE, and obj2cpp's face_normal takes v x u to
    match. A quad given here counter clockwise seen from OUTSIDE comes out
    facing outward, and the checker at the bottom of this file proves it for
    every mesh rather than trusting that it was typed correctly.
    """

    def __init__(self):
        self.verts = []
        self.faces = []          # (material, [index, ...], [uv, ...] or None)
        self._index = {}
        self.uvs = []
        self._uv_index = {}

    def vt(self, u, v):
        key = (round(u, 5), round(v, 5))
        if key not in self._uv_index:
            self.uvs.append(key)
            self._uv_index[key] = len(self.uvs)
        return self._uv_index[key]

    @property
    def textured(self):
        return any(f[2] is not None for f in self.faces)

    def v(self, x, y, z):
        key = (round(x, 5), round(y, 5), round(z, 5))
        if key not in self._index:
            self.verts.append(key)
            self._index[key] = len(self.verts)
        return self._index[key]

    def quad(self, material, a, b, c, d, uvs=None):
        # Reversed on the way in. The corner lists below are written counter
        # clockwise seen from outside, which is the ordinary convention and the
        # readable one; this repo's models are the other way round (tom.obj and
        # pad.obj both come out at a negative signed volume), so the flip
        # happens once here rather than in twenty hand ordered tuples.
        corners = [self.v(*p) for p in (d, c, b, a)]
        tex = None
        if uvs is not None:
            # Reversed alongside the corners, or the picture ends up mirrored
            # against the geometry it is stuck to.
            tex = [self.vt(*t) for t in (uvs[3], uvs[2], uvs[1], uvs[0])]
        self.faces.append((material, corners, tex))

    def box(self, material, x0, y0, z0, x1, y1, z1, skip=()):
        """An axis aligned box, every face wound outward."""
        sides = {
            "back":   ((x0, y0, z0), (x0, y1, z0), (x1, y1, z0), (x1, y0, z0)),
            "front":  ((x1, y0, z1), (x1, y1, z1), (x0, y1, z1), (x0, y0, z1)),
            "left":   ((x0, y0, z1), (x0, y1, z1), (x0, y1, z0), (x0, y0, z0)),
            "right":  ((x1, y0, z0), (x1, y1, z0), (x1, y1, z1), (x1, y0, z1)),
            "top":    ((x0, y1, z0), (x0, y1, z1), (x1, y1, z1), (x1, y1, z0)),
            "bottom": ((x0, y0, z1), (x0, y0, z0), (x1, y0, z0), (x1, y0, z1)),
        }
        for name, corners in sides.items():
            if name in skip:
                continue
            self.quad(material, *corners)

    def write(self, path, name, header, mtl_name):
        lines = [f"# {line}" for line in header.strip().split("\n")]
        lines.append(f"mtllib {mtl_name}")
        lines.append(f"o {name}")
        for x, y, z in self.verts:
            lines.append(f"v {x:.6f} {y:.6f} {z:.6f}")
        current = None
        for u, v in self.uvs:
            lines.append(f"vt {u:.6f} {v:.6f}")
        for material, idx, tex in self.faces:
            if material != current:
                lines.append(f"usemtl {material}")
                current = material
            if tex is None:
                lines.append("f " + " ".join(str(i) for i in idx))
            else:
                lines.append("f " + " ".join(
                    f"{i}/{t}" for i, t in zip(idx, tex)))
        path.write_text("\n".join(lines) + "\n")

    def signed_volume(self):
        """Six times the signed volume, by the divergence theorem over the
        triangle fan of every face. Negative is this repo's outward."""
        total = 0.0
        for _, idx, _tex in self.faces:
            p = [self.verts[i - 1] for i in idx]
            for k in range(1, len(p) - 1):
                a, b, c = p[0], p[k], p[k + 1]
                total += (a[0] * (b[1] * c[2] - b[2] * c[1])
                          - a[1] * (b[0] * c[2] - b[2] * c[0])
                          + a[2] * (b[0] * c[1] - b[1] * c[0]))
        return total

    def triangles(self):
        return sum(len(idx) - 2 for _, idx, _tex in self.faces)


# ---- the crate -------------------------------------------------------------
#
# A plain prism, 1.7 units on a side, with the detail in a 16x16 texture. It
# was a banded box of 28 triangles: a body, a strap ring standing proud of it,
# and a lid. Same trade the tower made, for the same reason. Those bands were
# slivers, and a sliver pays a whole per triangle bill (a bounding box, three
# edge setups, three divides) plus three software float vertex transforms to
# fill about twenty pixels.
#
# Sized by where it has to FIT rather than by how it looks alone: carried, it
# tucks under the hull between the legs, and the hull's feet reach 1.85 below
# its origin, so anything taller either hangs through the deck on touchdown or
# has to be drawn at a different size in the two places it appears.
#
# The bottom is skipped. It is either sitting on a deck or tucked against the
# hull, so nothing ever sees it, and two triangles is two triangles.

def build_cargo():
    m = Mesh()
    h, top = 0.85, 1.7
    # The texture is one picture on each wall, 0..1. Same one byte uv limit
    # the tower documents: a coordinate spans exactly one repeat, so tiling
    # has to live in the picture rather than in the coordinates.
    walls = [
        ("back",  ((-h, 0, -h), (-h, top, -h), (h, top, -h), (h, 0, -h))),
        ("front", ((h, 0, h), (h, top, h), (-h, top, h), (-h, 0, h))),
        ("left",  ((-h, 0, h), (-h, top, h), (-h, top, -h), (-h, 0, -h))),
        ("right", ((h, 0, -h), (h, top, -h), (h, top, h), (h, 0, h))),
    ]
    side_uv = ((0, 0), (0, 1), (1, 1), (1, 0))
    for _name, corners in walls:
        m.quad("lit", *corners, uvs=side_uv)
    # The lid samples the texture's top left corner, which the picture keeps as
    # a plain panel, so a crate seen from above reads as a lid and not as a
    # wall lying on its back.
    m.quad("lit", (-h, top, -h), (-h, top, h), (h, top, h), (h, top, -h),
           uvs=((0.02, 0.02), (0.02, 0.20), (0.20, 0.20), (0.20, 0.02)))
    return m


# ---- a plain block ---------------------------------------------------------
#
# The cube the brief asked for. One material, twelve triangles, drawn many
# times at different scales and tints. The cheapest thing that can stand in a
# valley and be a building.

# Both buildings are one unit half width and stand on y = 0, so the ONLY thing
# that varies per instance is a uniform scale. That is not a shortcut, it is
# what keeps the lighting right: draw_mesh turns the baked normal by the same
# basis it turns the vertex by, which is exact for a rotation and for a uniform
# scale, and wrong for a squashed one, because a squashed normal needs the
# inverse transpose. So the aspect lives in the mesh. A bigger building is also
# a wider one, which is how a skyline looks anyway.

BLOCK_HEIGHT = 1.6
TOWER_HEIGHT = 5.0


def build_block():
    m = Mesh()
    m.box("concrete", -1.0, 0.0, -1.0, 1.0, BLOCK_HEIGHT, 1.0, skip=("bottom",))
    return m


# ---- a plain tower, with its detail in a texture --------------------------
#
# This used to be a banded facade: five bands on four walls, 42 triangles, of
# which 32 were slivers covering about twenty pixels each. A sliver is the
# worst thing a scanline rasterizer can be handed. It pays the whole per
# triangle bill (a bounding box, three edge setups, three divides for the
# reciprocal depths) plus three software float vertex transforms, and then
# fills almost nothing.
#
# The same facade as a texture is one quad per wall. 42 triangles down to 10,
# and the detail actually went UP, because a picture can have windows in
# columns as well as rows and geometry at this budget could not.
#
# UVs run 0..1 across each wall, the whole picture on the whole wall.

TOWER_HEIGHT = 5.0

# The texture covers the WHOLE wall, once. That is a hard limit rather than a
# choice: a ScreenTriangle carries u and v as one byte each, 0..255 across the
# texture, so the span between two corners can express exactly one repeat.
# Asking for four made the v bytes run 255 down to 3, which is one texel of
# travel, and the windows came out as vertical stripes running the full height
# of the building. So facade.png holds all four storeys and the wall maps
# 0..1 onto it.
TOWER_STOREYS = 1


def build_tower():
    m = Mesh()
    h = TOWER_HEIGHT
    t = float(TOWER_STOREYS)
    # (name, four corners counter clockwise from outside, four uvs)
    walls = [
        ("back",  ((-1, 0, -1), (-1, h, -1), (1, h, -1), (1, 0, -1)),
                  ((0, 0), (0, t), (1, t), (1, 0))),
        ("front", ((1, 0, 1), (1, h, 1), (-1, h, 1), (-1, 0, 1)),
                  ((0, 0), (0, t), (1, t), (1, 0))),
        ("left",  ((-1, 0, 1), (-1, h, 1), (-1, h, -1), (-1, 0, -1)),
                  ((0, 0), (0, t), (1, t), (1, 0))),
        ("right", ((1, 0, -1), (1, h, -1), (1, h, 1), (1, 0, 1)),
                  ((0, 0), (0, t), (1, t), (1, 0))),
    ]
    for _name, corners, uvs in walls:
        m.quad("lit", *corners, uvs=uvs)
    # The roof samples one flat patch of the texture rather than a window band,
    # so it reads as a roof and not as a facade lying on its back.
    m.quad("lit", (-1, h, -1), (-1, h, 1), (1, h, 1), (1, h, -1),
           uvs=((0.02, 0.02), (0.02, 0.06), (0.06, 0.06), (0.06, 0.02)))
    return m


# `lit` is white on purpose. A textured face multiplies its texel by its vertex
# colour, and the vertex colour already carries the lambert, so a grey face
# colour under a grey texture darkens twice and the building comes out nearly
# black. White here means the texture is the colour and the lighting is the
# only thing modulating it.
# ---- the rocket section, mission three's salvage ------------------------
#
# An octagonal prism lying on its side in the swell, 15 long and 4.5 across.
# Octagonal rather than round because a flat facet has to end up on TOP: the
# ship lands on this, and a cylinder gives it a curved deck to slide off.
# Eight sides is where a prism stops reading as a box and starts reading as a
# rocket body at 120 pixels; more sides is more triangles for a shape nobody
# can resolve.
#
# The pickup zone is k_pad_half either way, the same square every deck uses, so
# the mesh is sized to roughly match what the sim already treats as landable
# rather than the other way round.

SEGMENT_LENGTH = 15.0
SEGMENT_RADIUS = 4.5
SEGMENT_SIDES = 8


def build_segment():
    m = Mesh()
    half = SEGMENT_LENGTH * 0.5
    r = SEGMENT_RADIUS
    # Rotated half a facet so a flat face lands squarely on top rather than an
    # edge. Without the offset the deck is a ridge and the ship rolls off it.
    ring = []
    for i in range(SEGMENT_SIDES):
        a = (i + 0.5) * 2.0 * math.pi / SEGMENT_SIDES
        ring.append((math.cos(a) * r, math.sin(a) * r))
    for i in range(SEGMENT_SIDES):
        y0, z0 = ring[i]
        y1, z1 = ring[(i + 1) % SEGMENT_SIDES]
        # u runs along the body so the panel lines band around it; v wraps.
        u0 = i / SEGMENT_SIDES
        u1 = (i + 1) / SEGMENT_SIDES
        m.quad("lit",
               (-half, y0, z0), (-half, y1, z1), (half, y1, z1), (half, y0, z0),
               uvs=((0, u0), (0, u1), (1, u1), (1, u0)))
    # End caps as fans. The scorched one faces where the rest of the stage
    # tore away.
    #
    # These carry uvs even though a flat cap barely needs them, and that is not
    # tidiness. obj2cpp emits a uv table only when EVERY face has one, all or
    # nothing, so a single bare face silently makes the whole mesh untextured.
    # The first version left these plain and the entire section rendered as a
    # white box, which looks exactly like a texture that failed to load rather
    # than like one that was never asked for.
    cap_uv = ((0.90, 0.10), (0.98, 0.10), (0.98, 0.90), (0.90, 0.90))
    for end, sign in ((-half, -1), (half, 1)):
        for i in range(1, SEGMENT_SIDES - 1):
            a = ring[0]
            b = ring[i if sign > 0 else SEGMENT_SIDES - i]
            c = ring[i + 1 if sign > 0 else SEGMENT_SIDES - i - 1]
            m.quad("burn", (end, a[0], a[1]), (end, b[0], b[1]),
                   (end, c[0], c[1]), (end, c[0], c[1]), uvs=cap_uv)
    return m


MATERIALS = [
    ("burn",      (54, 44, 42)),
    ("lit",       (255, 255, 255)),
    ("crate",     (194, 112, 58)),
    ("crate_top", (156, 86, 42)),
    ("strap",     (72, 62, 54)),
    ("concrete",  (150, 148, 142)),
    ("window",    (46, 74, 104)),
    ("roof",      (96, 96, 104)),
]


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--out", default="games/tomlander/models",
                    help="directory to write the models into")
    args = ap.parse_args()
    out = pathlib.Path(args.out)
    out.mkdir(parents=True, exist_ok=True)

    builds = [
        ("cargo", build_cargo(),
         "Tom Lander's crate, mission two's load.\n"
         "1.7 units on a side. Authored at true world units, so it draws at\n"
         "scale 1.0 like the pad does.\n"
         "Generated by tools/gen_tomlander_props.py."),
        ("block", build_block(),
         "A plain block building. One unit half width, 1.6 tall, standing on\n"
         "y = 0 and drawn at whatever uniform scale and tint the valley\n"
         "wants, so one mesh is a whole skyline.\n"
         "Generated by tools/gen_tomlander_props.py."),
        ("segment", build_segment(),
         "Tom Lander's salvage, a rocket section floating in the ocean.\n"
         "Octagonal so a flat facet lands on top, because the ship puts down\n"
         "on this. Authored at true world units, drawn at scale 1.0.\n"
         "Generated by tools/gen_tomlander_props.py."),
        ("tower", build_tower(),
         "A banded tower. One unit half width, 5.0 tall. The engine has no\n"
         "texture mapping, so the window\n"
         "bands are real inset faces with their own material: the detail is\n"
         "geometry, because geometry is the only kind of detail there is.\n"
         "Generated by tools/gen_tomlander_props.py."),
    ]

    (out / "props.mtl").write_text(
        "# Shared materials for Tom Lander's crate and buildings.\n"
        "# Generated by tools/gen_tomlander_props.py.\n" + mtl(MATERIALS))

    failed = False
    for name, mesh, header in builds:
        mesh.write(out / f"{name}.obj", name, header, "props.mtl")
        volume = mesh.signed_volume()
        ok = volume < 0
        if not ok:
            failed = True
        print(f"{name}.obj: {len(mesh.verts)} verts, {mesh.triangles()} tris, "
              f"volume {volume:+.1f} {'OK' if ok else 'FACES INWARD'}")

    if failed:
        raise SystemExit("a mesh is wound inside out, see above")


if __name__ == "__main__":
    main()
