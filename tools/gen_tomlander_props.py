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
        self.faces = []          # (material, [index, ...])
        self._index = {}

    def v(self, x, y, z):
        key = (round(x, 5), round(y, 5), round(z, 5))
        if key not in self._index:
            self.verts.append(key)
            self._index[key] = len(self.verts)
        return self._index[key]

    def quad(self, material, a, b, c, d):
        # Reversed on the way in. The corner lists below are written counter
        # clockwise seen from outside, which is the ordinary convention and the
        # readable one; this repo's models are the other way round (tom.obj and
        # pad.obj both come out at a negative signed volume), so the flip
        # happens once here rather than in twenty hand ordered tuples.
        self.faces.append((material, [self.v(*p) for p in (d, c, b, a)]))

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
        for material, idx in self.faces:
            if material != current:
                lines.append(f"usemtl {material}")
                current = material
            lines.append("f " + " ".join(str(i) for i in idx))
        path.write_text("\n".join(lines) + "\n")

    def signed_volume(self):
        """Six times the signed volume, by the divergence theorem over the
        triangle fan of every face. Negative is this repo's outward."""
        total = 0.0
        for _, idx in self.faces:
            p = [self.verts[i - 1] for i in idx]
            for k in range(1, len(p) - 1):
                a, b, c = p[0], p[k], p[k + 1]
                total += (a[0] * (b[1] * c[2] - b[2] * c[1])
                          - a[1] * (b[0] * c[2] - b[2] * c[0])
                          + a[2] * (b[0] * c[1] - b[1] * c[0]))
        return total

    def triangles(self):
        return sum(len(idx) - 2 for _, idx in self.faces)


# ---- the crate -------------------------------------------------------------
#
# 1.7 units on a side. Sized by where it has to FIT rather than by how it
# looks on its own: carried, it tucks under the hull between the legs, and the
# hull's feet reach 1.85 below its origin, so anything taller either hangs
# through the deck on touchdown or has to be drawn at a different size in the
# two places it appears. Banded so it is obviously a container.

def build_cargo():
    m = Mesh()
    h, top = 0.85, 1.7
    m.box("crate", -h, 0.0, -h, h, top, h, skip=("bottom",))
    # A strap around the middle, standing a hair proud so it cannot z fight
    # with the wall it sits on. The depth buffer is one byte, so "a hair" has
    # to be enough to clear a whole depth step at the range this is seen.
    e = h + 0.05
    m.box("strap", -e, 0.62, -e, e, 0.95, e,
          skip=("top", "bottom"))
    m.box("crate_top", -h, top, -h, h, top + 0.10, h, skip=("bottom",))
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


# ---- a banded tower --------------------------------------------------------
#
# The "textured" one. Its facade is cut into horizontal bands and the window
# bands are their own faces with their own colour, so the detail is real
# geometry rather than a picture, which is the only kind of detail an engine
# with no texture mapping can have.
#
# Five bands, four walls, plus a roof and a parapet: 44 triangles. That is a
# quarter of what the ship costs, so a valley can hold a few of them.

def build_tower():
    m = Mesh()
    bands = [(mat, a * TOWER_HEIGHT, b * TOWER_HEIGHT) for mat, a, b in (
        ("concrete", 0.00, 0.10),
        ("window",   0.10, 0.36),
        ("concrete", 0.36, 0.46),
        ("window",   0.46, 0.72),
        ("concrete", 0.72, 1.00),
    )]
    for material, y0, y1 in bands:
        # The window bands are inset, so the concrete ones read as floor slabs
        # standing proud of them and the building gains a silhouette.
        e = 1.0 if material == "concrete" else 0.93
        for name, corners in {
            "back":  ((-e, y0, -1.0), (-e, y1, -1.0), (e, y1, -1.0), (e, y0, -1.0)),
            "front": ((e, y0, 1.0), (e, y1, 1.0), (-e, y1, 1.0), (-e, y0, 1.0)),
            "left":  ((-1.0, y0, e), (-1.0, y1, e), (-1.0, y1, -e), (-1.0, y0, -e)),
            "right": ((1.0, y0, -e), (1.0, y1, -e), (1.0, y1, e), (1.0, y0, e)),
        }.items():
            del name
            m.quad(material, *corners)
    m.quad("roof", (-1.0, TOWER_HEIGHT, -1.0), (-1.0, TOWER_HEIGHT, 1.0),
           (1.0, TOWER_HEIGHT, 1.0), (1.0, TOWER_HEIGHT, -1.0))
    return m


MATERIALS = [
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
