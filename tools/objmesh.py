#!/usr/bin/env python3
"""Building a Wavefront .obj from Python, for models that are too regular to
hand author and too much geometry to write as a vertex table in C++.

Rule 11 says a model lives in a `.obj` a modeller can open, not in source. That
still leaves the question of how a twenty quad lattice tower or a banded facade
gets INTO the .obj, and typing it is not the answer: the mistakes are silent
(one corner in the wrong order is a face that vanishes from one side) and
tedious to find. So the regular ones are generated, and what is generated is
committed and editable from then on.

This module is the shared half of that: the mesh builder, the winding
convention, the .mtl writer, and the check that a mesh really does face
outward. It carries no geometry of its own. tools/gen_tomlander_props.py and
tools/gen_picospace_models.py both build on it, and they were two copies of
this class before they were one.

WINDING, which is the part that matters and the part that is easy to get
wrong: this repo's models wind so the signed volume comes out NEGATIVE, and
obj2cpp's face_normal takes v x u to match. Mesh.quad takes its corners the
ORDINARY way round, counter clockwise seen from outside, and reverses them on
the way in, so a caller writes what it means and the flip happens once here
rather than in every tuple. signed_volume() proves it for a finished mesh
rather than trusting that it was typed correctly.
"""

import math


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
