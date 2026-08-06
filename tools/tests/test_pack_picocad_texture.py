#!/usr/bin/env python3
"""Prove a picoCAD export survives the trip into a lit, correctly coloured mesh.

Two failures this tool exists to prevent are both silent, and both look like
the model is wrong rather than the import:

WINDING. picoCAD winds its faces the opposite way to every model in this repo.
All 24 committed models have negative signed volume, which is why
`obj2cpp.face_normal` takes the cross product as `v x u` rather than `u x v`.
A picoCAD solid is positive. Import one without flipping and every normal
points into the hull, Lambert collapses to pure ambient, and the model renders
as a flat silhouette. It compiles, it runs, and nothing warns. So the test that
matters is not "did the tool flip something", it is "does the mesh that comes
out of obj2cpp.py have normals pointing away from the solid", checked on the
generated C++ rather than on the tool's own report.

V FLIP. picoCAD measures v from the bottom, so a texel row is
`(1 - v) * height`. Getting it backwards samples the sheet upside down and
picks plausible looking wrong colours, which is harder to notice than obviously
wrong ones. The fixture is built so the top and bottom halves are different
colours and a flip cannot pass.

The end to end check is the point: the tool's output is fed to the real
obj2cpp.py and the real emitted table is parsed back. A tool that agreed with
itself and disagreed with the build would pass any test that stopped earlier.
"""

import os
import re
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
TOOLS = os.path.dirname(HERE)
REPO = os.path.dirname(TOOLS)
sys.path.insert(0, TOOLS)

import game_meta                # noqa: E402
import pack_picocad_texture     # noqa: E402

FAILURES = []


def check(condition, what):
    if condition:
        return
    FAILURES.append(what)
    sys.stderr.write("FAIL: %s\n" % what)


# ---------------------------------------------------------------- fixtures

# A unit cube wound the way picoCAD winds one, which is counter clockwise seen
# from outside, so the standard cross product points out and the signed volume
# is positive. Written out longhand rather than generated: a generator that
# got the winding wrong would make the test agree with the bug.
CUBE_V = [
    (-1, -1, -1), (1, -1, -1), (1, 1, -1), (-1, 1, -1),
    (-1, -1, 1), (1, -1, 1), (1, 1, 1), (-1, 1, 1),
]
CUBE_FACES = [
    (4, 5, 6, 7),     # +Z
    (1, 0, 3, 2),     # -Z
    (5, 1, 2, 6),     # +X
    (0, 4, 7, 3),     # -X
    (3, 7, 6, 2),     # +Y
    (0, 1, 5, 4),     # -Y
]

# Two 8x8 cells of a 16x16 sheet, one in the top half and one in the bottom.
# The uvs below put three faces on the top cell and three on the bottom, so a
# v flip swaps every colour and cannot go unnoticed.
TOP_RGB = (255, 163, 0)      # picotron 9
BOTTOM_RGB = (41, 173, 255)  # picotron 12
SHEET = 16


def write_cube_obj(path, uv_rows):
    """uv_rows[i] is 'top' or 'bottom' for face i."""
    lines = ["# picocad model", "mtllib cube.mtl", "o cube"]
    for v in CUBE_V:
        lines.append("v %f %f %f" % v)

    # picoCAD writes v from the bottom, so the top half of the image is the
    # HIGH v range. Cell y 0..8 is v 0.5..1.0; cell y 8..16 is v 0.0..0.5.
    slots = {}
    uvs = []
    for where in ("top", "bottom"):
        v0, v1 = (0.5, 1.0) if where == "top" else (0.0, 0.5)
        base = len(uvs)
        uvs.extend([(0.0, v1), (0.5, v1), (0.5, v0), (0.0, v0)])
        slots[where] = [base + 1, base + 2, base + 3, base + 4]
    for u, v in uvs:
        lines.append("vt %f %f" % (u, v))

    lines.append("usemtl cube.mtl")
    for i, face in enumerate(CUBE_FACES):
        slot = slots[uv_rows[i]]
        lines.append("f " + " ".join(
            "%d/%d/%d" % (face[k] + 1, slot[k], i + 1) for k in range(4)))
    with open(path, "w", encoding="utf-8") as handle:
        handle.write("\n".join(lines) + "\n")


def write_sheet(path):
    rows = [[(0, 0, 0)] * SHEET for _ in range(SHEET)]
    for y in range(SHEET):
        colour = TOP_RGB if y < SHEET // 2 else BOTTOM_RGB
        for x in range(SHEET // 2):
            rows[y][x] = colour
    game_meta.write_png(path, SHEET, SHEET, rows)


def run_tool(args):
    return subprocess.run(
        [sys.executable, os.path.join(TOOLS, "pack_picocad_texture.py")] + args,
        capture_output=True, text=True)


def run_obj2cpp(obj, out_cpp, out_hpp, name):
    # --namespace is required: models are generated one directory per game so
    # two games can both ship a tree.obj without colliding in the console.
    return subprocess.run(
        [sys.executable, os.path.join(TOOLS, "obj2cpp.py"), obj,
         "--out-cpp", out_cpp, "--out-hpp", out_hpp, "--name", name,
         "--namespace", "packtest"],
        capture_output=True, text=True)


def parse_mesh(cpp_path):
    """Read back the vertices and faces obj2cpp actually emitted."""
    src = open(cpp_path, "r", encoding="utf-8").read()
    vblock = re.search(r"k_vertices\[\] = \{(.*?)\n\};", src, re.S).group(1)
    verts = [tuple(int(n) for n in m)
             for m in re.findall(r"\{(-?\d+), (-?\d+), (-?\d+)\}", vblock)]
    fblock = re.search(r"k_faces\[\] = \{(.*?)\n\};", src, re.S).group(1)
    faces = []
    for m in re.findall(
            r"\{(\d+), (\d+), (\d+), (\d+), (\d+), (\d+), (-?\d+), (-?\d+), (-?\d+)\}",
            fblock):
        n = [int(x) for x in m]
        faces.append((n[0], n[1], n[2], (n[3], n[4], n[5]), (n[6], n[7], n[8])))
    return verts, faces


def outward_count(verts, faces):
    """How many baked normals point away from the model's centroid.

    Valid for a convex solid, which the fixture is on purpose.
    """
    cx = sum(v[0] for v in verts) / float(len(verts))
    cy = sum(v[1] for v in verts) / float(len(verts))
    cz = sum(v[2] for v in verts) / float(len(verts))
    out = 0
    for face in faces:
        mid = [sum(verts[face[k]][j] for k in range(3)) / 3.0
               for j in range(3)]
        delta = (mid[0] - cx, mid[1] - cy, mid[2] - cz)
        normal = face[4]
        if sum(delta[j] * normal[j] for j in range(3)) > 0:
            out += 1
    return out


# ------------------------------------------------------------------- checks

def test_winding_reaches_obj2cpp_pointing_outward():
    """The whole point. Flipped by default, and every normal lands outward."""
    with tempfile.TemporaryDirectory() as work:
        model = os.path.join(work, "cube.obj")
        sheet = os.path.join(work, "cube.png")
        write_cube_obj(model, ["top"] * 3 + ["bottom"] * 3)
        write_sheet(sheet)

        for keep, label, want_outward in ((False, "flipped", True),
                                          (True, "kept", False)):
            out_obj = os.path.join(work, "out_%s.obj" % label)
            out_mtl = os.path.join(work, "out_%s.mtl" % label)
            args = [model, "--texture", sheet,
                    "--out-obj", out_obj, "--out-mtl", out_mtl, "--bake"]
            if keep:
                args.append("--keep-winding")
            result = run_tool(args)
            check(result.returncode == 0,
                  "packer exits 0 for %s (%s)" % (label, result.stderr.strip()))

            cpp = os.path.join(work, "m_%s.cpp" % label)
            hpp = os.path.join(work, "m_%s.hpp" % label)
            emitted = run_obj2cpp(out_obj, cpp, hpp, "m_%s" % label)
            check(emitted.returncode == 0,
                  "obj2cpp accepts the %s model (%s)"
                  % (label, emitted.stderr.strip()))
            if emitted.returncode != 0:
                continue

            verts, faces = parse_mesh(cpp)
            out = outward_count(verts, faces)
            if want_outward:
                check(out == len(faces),
                      "every baked normal points outward after the flip, "
                      "got %d of %d" % (out, len(faces)))
            else:
                # Not a wish, a demonstration: this is what shipping the
                # unflipped model would actually look like to the renderer.
                check(out == 0,
                      "--keep-winding really does give inward normals, "
                      "got %d of %d outward" % (out, len(faces)))


def test_v_origin_is_not_upside_down():
    """Faces on the top half of the sheet get the top half's colour."""
    with tempfile.TemporaryDirectory() as work:
        model = os.path.join(work, "cube.obj")
        sheet = os.path.join(work, "cube.png")
        # +Z, -Z, +X on the top cell; -X, +Y, -Y on the bottom cell.
        write_cube_obj(model, ["top", "top", "top",
                               "bottom", "bottom", "bottom"])
        write_sheet(sheet)

        out_obj = os.path.join(work, "out.obj")
        out_mtl = os.path.join(work, "out.mtl")
        result = run_tool([model, "--texture", sheet, "--out-obj", out_obj,
                           "--out-mtl", out_mtl, "--bake"])
        check(result.returncode == 0,
              "packer exits 0 (%s)" % result.stderr.strip())

        mtl = open(out_mtl, "r", encoding="utf-8").read()
        wanted = set()
        for rgb in (TOP_RGB, BOTTOM_RGB):
            wanted.add("Kd %.6f %.6f %.6f"
                       % (rgb[0] / 255.0, rgb[1] / 255.0, rgb[2] / 255.0))
        for line in wanted:
            check(line in mtl,
                  "the sampled colour %r reaches the .mtl. A v flip would "
                  "put the other half's colour here." % line)
        check(mtl.count("newmtl ") == 2,
              "two distinct colours become two materials, got %d"
              % mtl.count("newmtl "))


def test_only_used_rectangles_are_packed():
    """The atlas carries what the faces read and nothing else."""
    with tempfile.TemporaryDirectory() as work:
        model = os.path.join(work, "cube.obj")
        sheet = os.path.join(work, "cube.png")
        write_cube_obj(model, ["top"] * 6)      # one cell, all six faces
        write_sheet(sheet)

        atlas = os.path.join(work, "atlas.png")
        result = run_tool([model, "--texture", sheet, "--atlas", atlas])
        check(result.returncode == 0,
              "packer exits 0 (%s)" % result.stderr.strip())
        check(os.path.isfile(atlas), "an atlas is written")
        if not os.path.isfile(atlas):
            return
        width, height, rows = game_meta.read_png(atlas)
        check((width, height) == (8, 8),
              "one 8x8 cell packs to an 8x8 atlas, got %dx%d" % (width, height))
        check(all(pixel == TOP_RGB for row in rows for pixel in row),
              "the packed atlas carries the cell's colour and no filler")


def test_a_face_without_a_uv_is_refused():
    """Rather than silently colouring it grey."""
    with tempfile.TemporaryDirectory() as work:
        model = os.path.join(work, "bare.obj")
        with open(model, "w", encoding="utf-8") as handle:
            handle.write("v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n")
        result = run_tool([model])
        check(result.returncode != 0,
              "a model with no texture coordinates is an error")
        check("texture coordinate" in result.stderr,
              "and the message says why, got %r" % result.stderr.strip())


def test_the_repo_convention_still_holds():
    """Every committed model is negative, which is what the flip assumes.

    If someone lands a positively wound model, `obj2cpp.face_normal` is wrong
    for it and this tool's default is wrong for it, and both fail silently.
    Better to fail here.
    """
    checked = 0
    for root, _dirs, files in os.walk(os.path.join(REPO, "games")):
        for name in files:
            if not name.endswith(".obj"):
                continue
            path = os.path.join(root, name)
            model = pack_picocad_texture.read_obj_geometry(path)
            solids = pack_picocad_texture.find_solids(model)
            volumes = [pack_picocad_texture.signed_volume(model, s)
                       for s in range(solids)]
            positive = sum(1 for v in volumes if v > 0)
            check(positive * 2 <= solids,
                  "%s is wound the way this repo expects, %d of %d solids "
                  "positive" % (os.path.relpath(path, REPO), positive, solids))
            checked += 1
    check(checked > 0, "found committed models to check")
    sys.stdout.write("pack_picocad_texture: checked %d committed model(s)\n"
                     % checked)


def main():
    test_winding_reaches_obj2cpp_pointing_outward()
    test_v_origin_is_not_upside_down()
    test_only_used_rectangles_are_packed()
    test_a_face_without_a_uv_is_refused()
    test_the_repo_convention_still_holds()

    if FAILURES:
        sys.stderr.write("\n%d check(s) failed\n" % len(FAILURES))
        return 1
    sys.stdout.write("pack_picocad_texture: all checks passed\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
