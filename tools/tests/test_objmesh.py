#!/usr/bin/env python3
"""Every face of a generated model faces outward.

This exists because one did not, and shipped. Twin Flare's cockpit had its roof
and its floor wound inside out. The engine's rasterizer treats the signed
screen area as the backface test, so a face wound the wrong way is not drawn
dark or drawn wrong: it is not drawn at all. The chase camera sits behind and
above a podracer, which makes the roof the largest face it ever looks at, so
the player looked straight through the cockpit into an empty shell and reported
the pod as "missing the top".

Nothing in the repo could have caught it:

  - signed_volume() is ONE number for a whole mesh, and the roof and the floor
    are near mirror images, so their two errors very nearly cancelled inside
    it. The generator printed no warning.
  - the host render tests measure where the cables attach and how far the
    camera-relative coordinates reach, which a missing face does not change.
  - at 120x120 a hole in a model looks like a shading choice.

So the check has to be per face, and it has to run in CI rather than in a
comment. `Mesh.inward_faces()` compares each baked normal against the direction
from the middle of the mesh out to that face, using obj2cpp's own cross product
order rather than a re-derivation, because a check derived independently can be
self consistent and still disagree with what reaches the device.
"""

import pathlib
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))

import gen_twinflare_models  # noqa: E402
from objmesh import Mesh  # noqa: E402

failures = 0


def check(ok, what):
    global failures
    if not ok:
        print(f"FAIL: {what}")
        failures += 1


def test_a_correct_box_has_no_inward_faces():
    m = Mesh()
    m.box("side", -1, -1, -1, 1, 1, 1)
    bad = m.inward_faces()
    check(not bad, f"a box wound by Mesh.box faces outward, got {bad}")
    check(m.signed_volume() < 0,
          "a box wound by Mesh.box has this repo's negative signed volume")


def test_a_flipped_face_is_caught():
    """The check has to FAIL on the bug, or passing means nothing.

    Two faces flipped, top and bottom, which is exactly the cockpit's shape of
    mistake, and the point is that signed_volume stays negative through it.
    """
    m = Mesh()
    m.box("side", -1, -1, -1, 1, 1, 1, skip=("top", "bottom"))
    # Reversed: the corners a caller would write for a face pointing the other
    # way. Mesh.quad reverses on the way in, so this is what a transposed pair
    # of corners produces.
    m.quad("side", (-1, 1, -1), (1, 1, -1), (1, 1, 1), (-1, 1, 1))
    m.quad("side", (-1, -1, 1), (1, -1, 1), (1, -1, -1), (-1, -1, -1))
    bad = m.inward_faces()
    check(len(bad) == 2, f"both flipped faces are reported, got {len(bad)}")
    check(m.signed_volume() < 0,
          "and the signed volume is still negative, which is why one number "
          "for a whole mesh cannot be the check")


def test_every_twinflare_model_faces_outward():
    """The models the game actually ships, rebuilt from their generator."""
    for name, (build, _materials) in gen_twinflare_models.MODELS.items():
        mesh = build()
        bad = mesh.inward_faces()
        check(not bad, f"{name} has inward faces: {bad}")
        check(mesh.signed_volume() < 0, f"{name} has a negative signed volume")
        print(f"  {name:<14} {mesh.triangles():>3} triangles, every face outward")


def test_the_committed_obj_matches_its_generator():
    """A model is committed AND generated, so the two can drift.

    They did not here, but a hand edit that fixes a rendered frame and is then
    silently undone by the next generator run is a whole afternoon, and this is
    four lines.
    """
    import tempfile
    from objmesh import mtl
    with tempfile.TemporaryDirectory() as tmp:
        out = pathlib.Path(tmp)
        for name, (build, materials) in gen_twinflare_models.MODELS.items():
            mesh = build()
            (out / f"{name}.mtl").write_text(mtl(materials))
            mesh.write(out / f"{name}.obj", name, gen_twinflare_models.HEADER,
                       f"{name}.mtl")
            committed = ROOT / "games" / "twinflare" / "models" / f"{name}.obj"
            check(committed.read_text() == (out / f"{name}.obj").read_text(),
                  f"games/twinflare/models/{name}.obj matches its generator")


if __name__ == "__main__":
    test_a_correct_box_has_no_inward_faces()
    test_a_flipped_face_is_caught()
    test_every_twinflare_model_faces_outward()
    test_the_committed_obj_matches_its_generator()
    if failures:
        print(f"{failures} failure(s)")
        raise SystemExit(1)
    print("objmesh tests passed")
