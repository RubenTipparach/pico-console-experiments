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

# Model space is the engine's: +Z is the nose, +Y is up, +X is right.
# Renderer3D::draw_mesh says "positive pitch lifts the +Z nose".


def ring(radius, z, squash, sides):
    out = []
    for i in range(sides):
        a = (i / sides) * math.tau + math.pi / sides
        out.append((math.cos(a) * radius, math.sin(a) * radius * squash, z))
    return out


def build_engine(length, mouth_r, body_r, nozzle_r, body_at, scoop, throat,
                 squash, sides=6):
    """A turbine: blunt intake at the front, tapering to a nozzle at the back.

    IT USED TO BE A ROCKET, pointed at the front and flaring to a wide bell at
    the back, and that is backwards for the thing it is. A podracer engine is a
    turbine: the front is an enormous intake and the back is where the exhaust
    leaves, narrower than the mouth that fed it.

    It also read badly, and for a reason worth writing down: the chase camera
    only ever sees the BACK of these. With the flare aft, the widest part of
    the engine was the only part on screen, so a pod was two fat hexagons with
    no length to them and no direction. Tapering aft puts the narrow end in
    shot with a lit throat down the middle of it, and the eye gets the
    engine's axis for free.

    Both ends are RECESSES rather than points: a cone whose apex sits inside
    the hull, so the intake is a scoop you look into and the nozzle is a hole
    that glows. Six triangles a side, the same as the rocket cost.
    """
    m = Mesh()
    z_mouth, z_tail = length * 0.5, -length * 0.5
    z_body = z_mouth - length * body_at

    a = ring(mouth_r, z_mouth, squash, sides)          # the intake lip
    b = ring(body_r, z_body, squash, sides)            # the waist
    c = ring(nozzle_r, z_tail, squash, sides)          # the nozzle
    intake = (0.0, 0.0, z_mouth - length * scoop)      # inside the mouth
    burn = (0.0, 0.0, z_tail + length * throat)        # inside the nozzle

    # Wound counter clockwise seen from OUTSIDE, which is what Mesh.quad and
    # Mesh.tri take: they reverse on the way in, because this repo's models
    # come out at a negative signed volume and obj2cpp's face_normal takes
    # v x u to match. The ring runs anticlockwise in XY seen from +Z, so a
    # face on the outside of the hull reads rear ring first.
    #
    # The two cones read the other way round from the hull, because they face
    # into the engine rather than out of it. Both recesses are shallow next to
    # their own radius, which is what keeps their normals pointing away from
    # the middle of the mesh and keeps inward_faces() quiet: a deep scoop is a
    # concavity that check cannot reason about.
    for i in range(sides):
        j = (i + 1) % sides
        m.tri("intake", intake, a[i], a[j])
        m.quad("hull", b[i], b[j], a[j], a[i])
        m.quad("trim", c[i], c[j], b[j], b[i])
        m.tri("glow", burn, c[j], c[i])
    return m


def build_cockpit(w=1.15, h=0.80, length=2.10, nose_out=0.60, nose_drop=0.08,
                  taper=0.42, rake=0.50):
    """A wedge with a canopy. 14 triangles, and it is the part the camera sits
    closest to and the part the player is looking past.

    Every proportion is an argument because there are six of these now, one per
    racer, and the difference between them has to be visible at 120 pixels from
    behind. `rake` is how far back the canopy's leading edge sits, `taper` how
    much the tail pinches in.
    """
    m = Mesh()
    nose = (0.0, -h * nose_drop, length * nose_out)
    f_tl = (-w * rake, h * .5, length * .08)
    f_tr = (w * rake, h * .5, length * .08)
    f_bl = (-w * (rake - .08), -h * .5, length * .08)
    f_br = (w * (rake - .08), -h * .5, length * .08)
    b_tl = (-w * taper, h * .42, -length * .5)
    b_tr = (w * taper, h * .42, -length * .5)
    b_bl = (-w * (taper - .08), -h * .42, -length * .5)
    b_br = (w * (taper - .08), -h * .42, -length * .5)

    m.tri("hull", nose, f_bl, f_br)
    m.tri("canopy", nose, f_tr, f_tl)
    m.tri("hull", nose, f_br, f_tr)
    m.tri("hull", nose, f_tl, f_bl)
    # The roof and the floor, and both were wound the wrong way round for the
    # whole of the game's first release. The chase camera sits behind and
    # above, so the roof is the largest face it ever looks at, and it was
    # back facing: the player was looking THROUGH the cockpit into an empty
    # shell, which is why the pod read as a small dark box hanging under the
    # engines rather than as a cockpit.
    #
    # Nothing caught it. signed_volume() is one number for the whole mesh and
    # the two flipped faces very nearly cancelled inside it; the host tests
    # measure where the cables attach, not which way a face points; and at
    # 120x120 a missing roof looks like a shading choice. inward_faces() is
    # the check that finds this, and main() below treats it as an error.
    m.quad("canopy", f_tl, f_tr, b_tr, b_tl)
    m.quad("hull", b_bl, b_br, f_br, f_bl)
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


# One engine and one cockpit PER RACER, and the shapes are the roster.
#
# There used to be two engine meshes and one cockpit between the six, recoloured
# and nothing else, which made the pod select screen a colour swatch: the stat
# bars said the pods were different and the only thing you could see was paint.
# A silhouette is what a player recognises at 120 pixels, so the silhouettes
# differ, and they differ ALONG THE STATS: the fast one is a spike, the tough
# one is a drum, the nimble one is a flat blade.
#
# The side count varies too, and it is not free geometry: five sides is thirty
# triangles an engine and seven is forty two, so the slim pods pay less than
# the heavy ones and the average across the roster is the thirty six a single
# shared mesh used to cost. Six pods on screen is the budget, and it has not
# moved.
ENGINE_MATERIALS = [("intake", (120, 120, 130)), ("hull", (200, 200, 200)),
                    ("trim", (150, 150, 158)), ("glow", (255, 170, 90))]
COCKPIT_MATERIALS = [("hull", (200, 200, 200)), ("trim", (168, 168, 176)),
                     ("canopy", (70, 96, 120)), ("glow", (90, 90, 100))]

# name: (engine args, cockpit args)
RACERS = {
    # SCARAB, the balanced one. The reference shape everything else reads off.
    "scarab": (dict(length=3.3, mouth_r=1.00, body_r=0.80, nozzle_r=0.54,
                    body_at=0.30, scoop=0.13, throat=0.11, squash=0.88, sides=6),
               dict(w=1.15, h=0.80, length=2.10)),
    # WISP, five for acceleration and grip and two for top speed. A blade:
    # long, flat and narrow, the least metal on the grid.
    "wisp": (dict(length=4.0, mouth_r=0.76, body_r=0.56, nozzle_r=0.36,
                  body_at=0.26, scoop=0.15, throat=0.10, squash=0.62, sides=5),
             dict(w=0.95, h=0.60, length=2.35, nose_out=0.68, taper=0.34)),
    # ANVIL, five for hull and cooling and two for acceleration and grip. A
    # drum: short, wide and round, the biggest intakes on the roster.
    "anvil": (dict(length=2.8, mouth_r=1.30, body_r=1.06, nozzle_r=0.76,
                   body_at=0.34, scoop=0.11, throat=0.13, squash=1.00, sides=7),
              dict(w=1.45, h=0.95, length=1.90, nose_out=0.46, taper=0.48,
                   rake=0.46)),
    # NEEDLE, five for top speed and one for cooling and hull. A spike: the
    # longest engines and the tightest nozzles, which is where the heat goes.
    "needle": (dict(length=4.7, mouth_r=0.66, body_r=0.50, nozzle_r=0.30,
                    body_at=0.22, scoop=0.17, throat=0.09, squash=0.84, sides=5),
               dict(w=0.85, h=0.66, length=2.55, nose_out=0.74, taper=0.30,
                    rake=0.44)),
    # NIGHTJAR, five for repair and four for hull. Built to be fixed: short
    # body, an intake you could climb into, everything reachable.
    "nightjar": (dict(length=3.1, mouth_r=1.38, body_r=0.96, nozzle_r=0.62,
                      body_at=0.38, scoop=0.10, throat=0.14, squash=0.92,
                      sides=7),
                 dict(w=1.30, h=0.72, length=2.00, nose_out=0.52, taper=0.46,
                      rake=0.54)),
    # FANG, five for grip. Tall and narrow rather than wide and flat, which is
    # the one profile on the roster that stands UP.
    "fang": (dict(length=3.6, mouth_r=0.82, body_r=0.68, nozzle_r=0.44,
                  body_at=0.28, scoop=0.13, throat=0.11, squash=1.32, sides=6),
             dict(w=1.00, h=1.00, length=2.05, nose_out=0.58, taper=0.40,
                  rake=0.52)),
}


def _models():
    out = {}
    for name, (engine, cockpit) in RACERS.items():
        out[f"engine_{name}"] = (lambda e=engine: build_engine(**e),
                                 ENGINE_MATERIALS)
        out[f"cockpit_{name}"] = (lambda c=cockpit: build_cockpit(**c),
                                  COCKPIT_MATERIALS)
    out["rock"] = (build_rock, [("stone", (170, 170, 170))])
    return out


MODELS = _models()

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

    failures = 0
    for name, (build, materials) in MODELS.items():
        mesh = build()
        volume = mesh.signed_volume()
        # This repo's models wind so the signed volume comes out NEGATIVE, and
        # obj2cpp's face_normal takes v x u to match. Checked rather than
        # trusted, because one corner in the wrong order is a face that
        # vanishes from one side and nothing else.
        if volume >= 0:
            print(f"  WARNING {name}: signed volume {volume:.2f}, expected negative")
        # And checked FACE BY FACE, which is the check that would have caught
        # the cockpit's missing roof. An error and not a warning: a warning is
        # something a build scrolls past.
        for index, material, centre in mesh.inward_faces():
            print(f"  ERROR {name}: face {index} ({material}) at {centre} "
                  f"points inward")
            failures += 1
        (out / f"{name}.mtl").write_text(mtl(materials))
        mesh.write(out / f"{name}.obj", name, HEADER, f"{name}.mtl")
        print(f"{name:<14} {len(mesh.verts):>3} verts  "
              f"{sum(len(f[1]) - 2 for f in mesh.faces):>3} triangles  "
              f"volume {volume:>8.2f}")
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
