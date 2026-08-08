#!/usr/bin/env python3
"""Build Pico Space Program's rocket and its launch pad.

Rule 11 says geometry lives in a `.obj` a modeller can open, not in a vertex
table in C++. These three are rings of a regular polygon repeated up an axis,
which is exactly the kind of shape that is tedious and error prone to type and
trivial to describe, so this writes them, and what it writes is committed. Edit
the `.obj` directly for a one off tweak; come back here when the shape itself
should change.

AUTHORED IN METRES, NOSE UP. The sim measures everything in metres and the
renderer draws the orbital plane from the side, so a model here is at true
world size and draws at scale 1.0. Up the page is +y, which is where the nose
points at a roll of zero: the renderer turns the ship with draw_mesh's roll,
about the model's own z, and the ship's attitude is measured the same way the
sim measures it, a quarter turn meaning straight up.

Eight sided rather than round. At 120 pixels a rocket is about eight pixels
across, so a sixteen sided body spends double the triangles on a silhouette
nobody can tell from an octagon, and a scanline rasterizer charges the full per
triangle bill for every sliver it is handed.

Usage: tools/gen_picospace_models.py [--out games/picospace/models]
"""

import argparse
import math
import pathlib

from objmesh import Mesh, mtl

SIDES = 8


def ring(radius, offset=0.5):
    """One ring of the body, as (x, z) pairs.

    Offset half a facet so a FLAT face points along +x rather than an edge.
    The camera looks down z at the orbital plane, so +x and -x are the two
    silhouette edges of the rocket, and an edge there gives a body that is a
    pixel wider on one side than the other for no reason anyone can see.
    """
    return [(math.cos((i + offset) * 2 * math.pi / SIDES) * radius,
             math.sin((i + offset) * 2 * math.pi / SIDES) * radius)
            for i in range(SIDES)]


def tube(mesh, material, y0, r0, y1, r1):
    """A ring to a ring: a cylinder when the radii match, a cone when not."""
    a, b = ring(r0), ring(r1)
    for i in range(SIDES):
        j = (i + 1) % SIDES
        mesh.quad(material,
                  (a[i][0], y0, a[i][1]), (b[i][0], y1, b[i][1]),
                  (b[j][0], y1, b[j][1]), (a[j][0], y0, a[j][1]))


def cap(mesh, material, y, radius, up):
    """Close a ring off with a fan. `up` is which way the face looks."""
    r = ring(radius)
    for i in range(1, SIDES - 1):
        p = [r[0], r[i], r[i + 1]] if up else [r[0], r[i + 1], r[i]]
        mesh.quad(material, (p[0][0], y, p[0][1]), (p[1][0], y, p[1][1]),
                  (p[2][0], y, p[2][1]), (p[2][0], y, p[2][1]))


def apex(mesh, material, y0, radius, tip_y):
    """A cone from a ring up to a point: the nose."""
    r = ring(radius)
    for i in range(SIDES):
        j = (i + 1) % SIDES
        mesh.quad(material, (r[i][0], y0, r[i][1]), (0.0, tip_y, 0.0),
                  (0.0, tip_y, 0.0), (r[j][0], y0, r[j][1]))


# ---- the booster ----------------------------------------------------------
#
# Nine and a half metres of tank under a metre and a half of engine, with four
# fins. It is the bottom of the stack, so it stands BELOW the origin: the
# origin of the whole ship is the joint between the two stages, which is what
# lets the renderer draw the two as one rocket before staging and the upper
# alone afterwards without moving anything.

BOOSTER_BOTTOM = -9.4
BOOSTER_RADIUS = 1.0
FIN_SPAN = 2.3


def build_booster():
    m = Mesh()
    r = BOOSTER_RADIUS
    tube(m, "hull", BOOSTER_BOTTOM, r, -0.7, r)
    # The interstage: a band in the launch colour so the seam the ship splits
    # at is visible from the moment it leaves the pad, and a player watching
    # the fuel bar empty can see where the rocket is about to come apart.
    tube(m, "stripe", -0.7, r, 0.0, r)
    cap(m, "stripe", 0.0, r, up=True)
    # The skirt and the bell.
    tube(m, "engine", BOOSTER_BOTTOM, r, -10.4, 0.72)
    tube(m, "engine", -10.4, 0.72, -11.0, 0.95)
    cap(m, "burn", -11.0, 0.95, up=False)

    # Four fins, on the two axes. The pair on x is the pair the side on camera
    # sees; the pair on z is what stops the rocket reading as a flat cut out
    # when the view swings.
    for ax, az in ((1, 0), (-1, 0), (0, 1), (0, -1)):
        t = 0.13                          # half thickness, across the fin
        root_y, tip_y = BOOSTER_BOTTOM, BOOSTER_BOTTOM + 3.0
        inner, outer = r * 0.8, FIN_SPAN

        def p(along, out, side):
            """A fin corner: `out` along the fin's axis, `side` across it."""
            return (ax * out + az * side * t, along, az * out + ax * side * t)

        for side in (1, -1):
            m.quad("fin", p(root_y, inner, side), p(root_y, outer, side),
                   p(tip_y, outer * 0.62, side), p(tip_y, inner, side))
        # The outboard edge, so the fin is a wedge and not a zero width sheet.
        m.quad("fin", p(root_y, outer, -1), p(root_y, outer, 1),
               p(tip_y, outer * 0.62, 1), p(tip_y, outer * 0.62, -1))
        m.quad("fin", p(root_y, inner, 1), p(root_y, outer, 1),
               p(root_y, outer, -1), p(root_y, inner, -1))
    return m


# ---- the upper stage ------------------------------------------------------
#
# The half that reaches the moon, so it is also the lander: a tank, a nose, an
# engine and four legs. It stands ABOVE the origin, on the joint the booster
# hangs below.

LANDER_TOP = 4.2
LANDER_RADIUS = 0.8
LANDER_NOSE = 5.9


def build_lander():
    m = Mesh()
    r = LANDER_RADIUS
    tube(m, "hull", 0.0, r, 2.4, r)
    tube(m, "band", 2.4, r, 2.9, r)        # the stripe that says which way up
    tube(m, "hull", 2.9, r, LANDER_TOP, r)
    apex(m, "nose", LANDER_TOP, r, LANDER_NOSE)
    # Engine: a short bell hanging under the tank.
    tube(m, "engine", 0.0, 0.5, -0.95, 0.78)
    cap(m, "burn", -0.95, 0.78, up=False)
    return m


# ---- the landing legs -----------------------------------------------------
#
# Their own model, and not because it is tidier. They reach nearly six metres
# below the joint, which is straight through the booster: drawn as part of the
# lander they straddled the first stage all the way up the ascent. So they are
# drawn only once the booster is gone, which is when a real vehicle deploys
# them anyway, and the ascent is 32 triangles lighter for it.
#
# They are what the ship rests on, and k_gear_m in tuning.hpp is the height the
# sim parks the origin at, so a foot has to sit at about -k_gear_m for the
# picture and the physics to agree about where the ground is.
# tests/preview.cpp measures exactly that.

LEG_FOOT = -5.9
LEG_SPAN = 2.1


def build_legs():
    m = Mesh()
    r = LANDER_RADIUS
    for ax, az in ((1, 0), (-1, 0), (0, 1), (0, -1)):
        t = 0.16

        def p(along, out, side):
            return (ax * out + az * side * t, along, az * out + ax * side * t)

        for side in (1, -1):
            m.quad("leg", p(0.9, r * 0.9, side), p(LEG_FOOT + 0.5, LEG_SPAN, side),
                   p(LEG_FOOT, LEG_SPAN, side), p(0.3, r * 0.9, side))
        m.quad("leg", p(0.9, r * 0.9, -1), p(0.9, r * 0.9, 1),
               p(LEG_FOOT + 0.5, LEG_SPAN, 1), p(LEG_FOOT + 0.5, LEG_SPAN, -1))
        m.quad("leg", p(0.3, r * 0.9, 1), p(0.3, r * 0.9, -1),
               p(LEG_FOOT, LEG_SPAN, -1), p(LEG_FOOT, LEG_SPAN, 1))
        # The foot: a small pad, flat side down.
        m.quad("foot", p(LEG_FOOT, LEG_SPAN - 0.35, -1.9),
               p(LEG_FOOT, LEG_SPAN + 0.45, -1.9),
               p(LEG_FOOT, LEG_SPAN + 0.45, 1.9),
               p(LEG_FOOT, LEG_SPAN - 0.35, 1.9))
    return m


# ---- the launch pad -------------------------------------------------------
#
# A deck and a service tower. Drawn once, at the launch site, and only while
# the ship is low enough to see it, so it can afford to be the one piece of
# built scenery in the game.

PAD_HALF = 5.0
PAD_TOP = 1.4
TOWER_TOP = 16.0


def build_pad():
    m = Mesh()
    m.box("deck", -PAD_HALF, 0.0, -PAD_HALF, PAD_HALF, PAD_TOP, PAD_HALF,
          skip=("bottom",))
    # The mast, off to one side of where the rocket stands.
    m.box("steel", 6.0, 0.0, -0.8, 7.6, TOWER_TOP, 0.8, skip=("bottom",))
    # Work platforms, and the arm that reaches across to the rocket.
    for h in (4.0, 8.0, 12.0):
        m.box("steel", 4.4, h, -1.3, 7.6, h + 0.45, 1.3)
    m.box("arm", 2.4, 11.6, -0.45, 6.0, 12.05, 0.45)
    return m


MATERIALS = [
    # Bright, because the engine multiplies a face colour by its lambert and
    # anything authored mid grey comes out of the shading nearly black.
    ("hull",   (232, 236, 244)),
    ("band",   (58, 110, 210)),
    ("nose",   (226, 74, 90)),
    ("stripe", (238, 138, 46)),
    ("engine", (128, 132, 146)),
    ("burn",   (62, 58, 62)),
    ("fin",    (92, 98, 114)),
    ("leg",    (160, 166, 178)),
    ("foot",   (96, 102, 116)),
    ("deck",   (176, 176, 172)),
    ("steel",  (150, 154, 164)),
    ("arm",    (214, 168, 62)),
]


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--out", default="games/picospace/models",
                    help="directory to write the models into")
    args = ap.parse_args()
    out = pathlib.Path(args.out)
    out.mkdir(parents=True, exist_ok=True)

    builds = [
        ("booster", build_booster(),
         "Pico Space Program's first stage: tank, engine bell and four fins.\n"
         "Metres, nose up, hanging BELOW the origin, which is the joint the\n"
         "two stages separate at. Draws at scale 1.0.\n"
         "Generated by tools/gen_picospace_models.py."),
        ("lander", build_lander(),
         "Pico Space Program's upper stage, which is also the lander: tank,\n"
         "nose cone and engine. Metres, nose up, standing ABOVE the origin,\n"
         "which is the joint the two stages separate at. Draws at scale 1.0.\n"
         "Generated by tools/gen_picospace_models.py."),
        ("legs", build_legs(),
         "Pico Space Program's landing legs, deployed once the booster is\n"
         "gone. Their own model because they reach through where the first\n"
         "stage is. The feet sit at k_gear_m below the origin, which is the\n"
         "height the sim rests the ship at. Draws at scale 1.0.\n"
         "Generated by tools/gen_picospace_models.py."),
        ("pad", build_pad(),
         "Pico Space Program's launch pad: a deck and a service tower.\n"
         "Metres, standing on y = 0, drawn at scale 1.0.\n"
         "Generated by tools/gen_picospace_models.py."),
    ]

    (out / "props.mtl").write_text(
        "# Shared materials for Pico Space Program's rocket and pad.\n"
        "# Generated by tools/gen_picospace_models.py.\n" + mtl(MATERIALS))

    failed = False
    for name, mesh, header in builds:
        mesh.write(out / f"{name}.obj", name, header, "props.mtl")
        volume = mesh.signed_volume()
        # The fins and the legs are open shells, so their signed volume says
        # nothing about winding on its own: only the closed meshes are checked
        # against it. What proves the open ones is the picture, and the preview
        # harness is where that gets looked at.
        ok = volume < 0 or name == "legs"
        if not ok:
            failed = True
        print(f"{name}.obj: {len(mesh.verts)} verts, {mesh.triangles()} tris, "
              f"volume {volume:+.1f} {'OK' if ok else 'FACES INWARD'}")

    if failed:
        raise SystemExit("a mesh is wound inside out, see above")


if __name__ == "__main__":
    main()
