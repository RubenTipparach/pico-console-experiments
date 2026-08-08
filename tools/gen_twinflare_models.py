#!/usr/bin/env python3
"""Build Twin Flare's meshes.

A podracer is not one solid. It is two engines out in front, a cockpit trailing
behind them on cables, and the parts move relative to each other: the engines
answer the stick at once and the cockpit lags behind and swings. So they are
separate meshes drawn with separate transforms, not one model, and that is what
decides the shape of this file.

Rule 11 says geometry lives in a `.obj` a modeller can open. These are bodies of
revolution, which are tedious and error prone to type by hand and trivial to
generate, so this writes them and what it writes is committed. Edit the `.obj`
for a one off tweak; come back here when the shape itself should change.

SIX SIDES, and the count is the design. A ring of six is 36 triangles an engine
and two engines plus a cockpit is 86, which is what lets six pods share a frame
with 200 triangles of road inside a queue that holds 640. Eight sides doubles
the engine for a roundness that is two pixels wide at 120 by 120.

THREE RINGS, not four. The first draft had a separate waist band in the trim
colour, at 48 triangles an engine, and six pods came to 732 before a metre of
road was drawn. Three rings is the same silhouette at three quarters of the
cost, and the waist comes from the flat shading instead, which is what flat
shading is for.

Usage: tools/gen_twinflare_models.py [--out games/twinflare/models]
"""

import argparse
import math
import pathlib
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))

from objmesh import Mesh, mtl  # noqa: E402

SIDES = 6

# Model space is the engine's: +Z is the nose, +Y is up, +X is right.
# Renderer3D::draw_mesh says "positive pitch lifts the +Z nose".


def ring(radius, z, squash, sides=SIDES):
    out = []
    for i in range(sides):
        a = (i / sides) * math.tau + math.pi / 6
        out.append((math.cos(a) * radius, math.sin(a) * radius * squash, z))
    return out


def build_engine(length, nose_r, body_r, bell_r, intake, bell, squash):
    """Intake cone, body, exhaust bell, throat. 36 triangles."""
    m = Mesh()
    z_nose, z_bell = length * 0.5, -length * 0.5
    z_front = z_nose - length * intake
    z_rear = z_bell + length * bell

    a = ring(nose_r + body_r * 0.45, z_front, squash)
    c = ring(body_r, z_rear, squash)
    d = ring(bell_r, z_bell, squash)
    tip = (0.0, 0.0, z_nose)
    throat = (0.0, 0.0, z_bell - length * 0.05)

    # Wound counter clockwise seen from OUTSIDE, which is what Mesh.quad and
    # Mesh.tri take: they reverse on the way in, because this repo's models
    # come out at a negative signed volume and obj2cpp's face_normal takes
    # v x u to match. The ring runs anticlockwise in XY seen from +Z, so a
    # face on the outside of the hull reads rear ring first.
    for i in range(SIDES):
        j = (i + 1) % SIDES
        m.tri("intake", tip, a[i], a[j])
        m.quad("hull", c[i], c[j], a[j], a[i])
        m.quad("trim", d[i], d[j], c[j], c[i])
        # The throat faces backward, into the exhaust: it is the inside of the
        # bell, not the outside, so it winds the other way from everything else.
        m.tri("glow", throat, d[j], d[i])
    return m


def build_cockpit():
    """A wedge with a canopy. 14 triangles, and it is the part the camera sits
    closest to and the part the player is looking past."""
    w, h, length = 1.15, 0.80, 2.10
    m = Mesh()
    nose = (0.0, -h * 0.08, length * 0.60)
    f_tl = (-w * .5, h * .5, length * .08)
    f_tr = (w * .5, h * .5, length * .08)
    f_bl = (-w * .42, -h * .5, length * .08)
    f_br = (w * .42, -h * .5, length * .08)
    b_tl = (-w * .44, h * .42, -length * .5)
    b_tr = (w * .44, h * .42, -length * .5)
    b_bl = (-w * .36, -h * .42, -length * .5)
    b_br = (w * .36, -h * .42, -length * .5)

    m.tri("hull", nose, f_bl, f_br)
    m.tri("canopy", nose, f_tr, f_tl)
    m.tri("hull", nose, f_br, f_tr)
    m.tri("hull", nose, f_tl, f_bl)
    m.quad("canopy", b_tl, b_tr, f_tr, f_tl)
    m.quad("hull", f_bl, f_br, b_br, b_bl)
    m.quad("trim", f_br, b_br, b_tr, f_tr)
    m.quad("trim", b_bl, f_bl, f_tl, b_tl)
    # The tail is 'trim' and not 'glow'. Glow is a dark grey meant as an
    # accent, and draw_mesh's tint MULTIPLIES, so times a pod's livery it came
    # out nearly black: the cockpit read as a hole from directly behind, which
    # is the one angle the chase camera is always at.
    m.quad("trim", b_br, b_bl, b_tl, b_tr)
    return m


def build_rock():
    """A three sided spike. Six triangles, and at 120 pixels it reads as a
    rock, a pillar or an ice shard depending only on the colour the game tints
    it, which is why there is one of these and not four."""
    m = Mesh()
    base = [(math.cos(i / 3 * math.tau) * 1.0, -1.0, math.sin(i / 3 * math.tau) * 1.0)
            for i in range(3)]
    tip = (0.0, 2.6, 0.0)
    for i in range(3):
        j = (i + 1) % 3
        m.tri("stone", base[j], base[i], tip)
    m.tri("stone", base[0], base[1], base[2])
    return m


MODELS = {
    # Slim: the shape four of the six pods run. Long, narrow, a wide bell.
    "engine_slim": (lambda: build_engine(3.5, 0.30, 0.62, 0.90, 0.30, 0.26, 0.86),
                    [("intake", (120, 120, 130)), ("hull", (200, 200, 200)),
                     ("trim", (150, 150, 158)), ("glow", (255, 170, 90))]),
    # Heavy: shorter and fatter, for the two pods built like a fist.
    "engine_heavy": (lambda: build_engine(3.1, 0.42, 0.86, 1.16, 0.22, 0.30, 0.94),
                     [("intake", (120, 120, 130)), ("hull", (200, 200, 200)),
                      ("trim", (150, 150, 158)), ("glow", (255, 170, 90))]),
    "cockpit": (build_cockpit,
                [("hull", (200, 200, 200)), ("trim", (168, 168, 176)),
                 ("canopy", (70, 96, 120)), ("glow", (90, 90, 100))]),
    "rock": (build_rock, [("stone", (170, 170, 170))]),
}

HEADER = """Generated by tools/gen_twinflare_models.py. Committed and editable:
edit this file for a one off tweak, edit the generator when the shape changes.

Face colours here are a neutral grey on purpose. Every pod is drawn with a per
racer tint, and draw_mesh's tint MULTIPLIES the mesh's own colour, so it can
darken and recolour but never brighten: a mesh authored in one pod's livery
could not be repainted into another's."""


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default="games/twinflare/models")
    args = ap.parse_args()
    out = pathlib.Path(args.out)
    out.mkdir(parents=True, exist_ok=True)

    for name, (build, materials) in MODELS.items():
        mesh = build()
        volume = mesh.signed_volume()
        # This repo's models wind so the signed volume comes out NEGATIVE, and
        # obj2cpp's face_normal takes v x u to match. Checked rather than
        # trusted, because one corner in the wrong order is a face that
        # vanishes from one side and nothing else.
        if volume >= 0:
            print(f"  WARNING {name}: signed volume {volume:.2f}, expected negative")
        (out / f"{name}.mtl").write_text(mtl(materials))
        mesh.write(out / f"{name}.obj", name, HEADER, f"{name}.mtl")
        print(f"{name:<14} {len(mesh.verts):>3} verts  "
              f"{sum(len(f[1]) - 2 for f in mesh.faces):>3} triangles  "
              f"volume {volume:>8.2f}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
