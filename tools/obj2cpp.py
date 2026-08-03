#!/usr/bin/env python3
"""Convert a Wavefront .obj model into a compact const C++ table.

Run at build time by cmake/obj_model.cmake. Output lands in the build tree and
is never committed. Models stay editable in Blender and friends.

The emitted data is `const`, so it lives in RP2040 XIP flash rather than SRAM.
Vertices are 16 bit fixed point, faces are indexed triangles with a flat colour
and a packed normal. A 200 triangle model costs roughly 3 KB of flash.

Usage:
    obj2cpp.py MODEL.obj --out-cpp out.cpp --out-hpp out.hpp --name santa_sleigh
                         [--scale 256] [--fit 1.0] [--no-normals]
"""

import argparse
import hashlib
import os
import re
import sys

SAFE_NAME = re.compile(r"[^A-Za-z0-9_]")


class ObjParseError(Exception):
    """Raised when the .obj file cannot be understood."""


def colour_from_name(name):
    """Derive a stable, reasonably bright colour from a material name.

    Used when a model has `usemtl` but no .mtl file resolves it. Returning a
    single grey for every unnamed material would silently flatten a model that
    was authored with distinct parts, and the failure looks like a modelling
    mistake rather than a missing file.
    """
    digest = hashlib.sha256(name.encode("utf-8")).digest()
    # Bias each channel into 90..245 so nothing comes out black or blown out.
    return tuple(90 + (digest[i] * 155 // 255) for i in range(3))


class Material:
    """A single named material with a diffuse colour in 0..255."""

    def __init__(self, name, rgb=None):
        self.name = name
        self.rgb = rgb if rgb is not None else colour_from_name(name)


class MaterialLibrary:
    """Reads .mtl sidecar files. Missing files are not an error."""

    DEFAULT = Material("__default__", (200, 200, 200))

    def __init__(self):
        self._materials = {}

    def load(self, path):
        if not os.path.isfile(path):
            return
        current = None
        with open(path, "r", encoding="utf-8", errors="replace") as handle:
            for raw in handle:
                parts = raw.split()
                if not parts:
                    continue
                key, values = parts[0], parts[1:]
                if key == "newmtl" and values:
                    current = Material(values[0])
                    self._materials[current.name] = current
                elif key == "Kd" and current is not None and len(values) >= 3:
                    current.rgb = tuple(
                        max(0, min(255, int(round(float(v) * 255.0))))
                        for v in values[:3]
                    )

    def get(self, name):
        """A `usemtl` with no matching .mtl entry still gets its own colour,
        derived from the name. See colour_from_name."""
        material = self._materials.get(name)
        if material is None:
            material = Material(name)
            self._materials[name] = material
        return material


class Mesh:
    """Triangulated, indexed geometry ready for emission."""

    def __init__(self):
        self.vertices = []   # list of (x, y, z) floats
        self.faces = []      # list of (i0, i1, i2, (r, g, b))

    @property
    def triangle_count(self):
        return len(self.faces)


class ObjReader:
    """Parses a .obj into a Mesh. Only geometry and material names are read."""

    def __init__(self, materials):
        self._materials = materials

    def read(self, path):
        mesh = Mesh()
        current = MaterialLibrary.DEFAULT
        mtl_dir = os.path.dirname(os.path.abspath(path))

        with open(path, "r", encoding="utf-8", errors="replace") as handle:
            for line_no, raw in enumerate(handle, start=1):
                line = raw.split("#", 1)[0].strip()
                if not line:
                    continue
                parts = line.split()
                key, values = parts[0], parts[1:]

                if key == "v":
                    if len(values) < 3:
                        raise ObjParseError(
                            "%s:%d: vertex needs 3 coordinates" % (path, line_no)
                        )
                    mesh.vertices.append(tuple(float(v) for v in values[:3]))
                elif key == "mtllib" and values:
                    self._materials.load(os.path.join(mtl_dir, " ".join(values)))
                elif key == "usemtl" and values:
                    current = self._materials.get(values[0])
                elif key == "f":
                    self._add_face(mesh, values, current, path, line_no)

        if not mesh.vertices:
            raise ObjParseError("%s: no vertices found" % path)
        if not mesh.faces:
            raise ObjParseError("%s: no faces found" % path)
        return mesh

    def _add_face(self, mesh, values, material, path, line_no):
        indices = [self._vertex_index(v, len(mesh.vertices), path, line_no)
                   for v in values]
        if len(indices) < 3:
            raise ObjParseError("%s:%d: face needs 3 or more vertices"
                                % (path, line_no))
        # Fan triangulation. Convex n-gons are exact, concave ones are close
        # enough for models this small, and Blender exports triangles anyway.
        for k in range(1, len(indices) - 1):
            mesh.faces.append(
                (indices[0], indices[k], indices[k + 1], material.rgb)
            )

    @staticmethod
    def _vertex_index(token, vertex_count, path, line_no):
        raw = token.split("/")[0]
        try:
            index = int(raw)
        except ValueError:
            raise ObjParseError("%s:%d: bad face index %r" % (path, line_no, token))
        if index < 0:
            index = vertex_count + index      # negative indices are relative
        else:
            index -= 1                        # .obj is 1 based
        if index < 0 or index >= vertex_count:
            raise ObjParseError("%s:%d: face index %d out of range"
                                % (path, line_no, index))
        return index


def face_normal(mesh, face):
    """Unit normal of a triangle, as three floats. Degenerate faces give +Y."""
    a = mesh.vertices[face[0]]
    b = mesh.vertices[face[1]]
    c = mesh.vertices[face[2]]
    u = (b[0] - a[0], b[1] - a[1], b[2] - a[2])
    v = (c[0] - a[0], c[1] - a[1], c[2] - a[2])
    n = (
        u[1] * v[2] - u[2] * v[1],
        u[2] * v[0] - u[0] * v[2],
        u[0] * v[1] - u[1] * v[0],
    )
    length = (n[0] ** 2 + n[1] ** 2 + n[2] ** 2) ** 0.5
    if length < 1e-9:
        return (0.0, 1.0, 0.0)
    return (n[0] / length, n[1] / length, n[2] / length)


def fit_scale(mesh, target):
    """Uniform factor that puts the model's largest half extent at `target`."""
    if target <= 0.0:
        return 1.0
    extent = 0.0
    for v in mesh.vertices:
        extent = max(extent, abs(v[0]), abs(v[1]), abs(v[2]))
    if extent < 1e-9:
        return 1.0
    return target / extent


class CppEmitter:
    """Writes the generated header and source. One job, no side effects."""

    HEADER_NOTE = ("// Generated by tools/obj2cpp.py from %s\n"
                   "// Do not edit. Edit the .obj and rebuild.\n")

    def __init__(self, name, source_name, scale):
        self.name = name
        self.source_name = source_name
        self.scale = scale

    def emit_header(self, mesh):
        out = [self.HEADER_NOTE % self.source_name]
        out.append("#pragma once\n")
        out.append('#include "pse/mesh.hpp"\n\n')
        out.append("namespace models {\n\n")
        out.append("// %d vertices, %d triangles, %d bytes of flash\n"
                   % (len(mesh.vertices), mesh.triangle_count,
                      self.flash_bytes(mesh)))
        out.append("extern const pse::MeshData %s;\n\n" % self.name)
        out.append("}  // namespace models\n")
        return "".join(out)

    def emit_source(self, mesh, want_normals):
        out = [self.HEADER_NOTE % self.source_name]
        out.append('#include "%s.hpp"\n\n' % self.name)
        out.append("namespace models {\nnamespace {\n\n")

        out.append("const pse::MeshVertex k_vertices[] = {\n")
        for x, y, z in mesh.vertices:
            out.append("    {%d, %d, %d},\n" % (
                self._fixed(x), self._fixed(y), self._fixed(z)))
        out.append("};\n\n")

        out.append("const pse::MeshFace k_faces[] = {\n")
        for face in mesh.faces:
            i0, i1, i2, rgb = face
            if want_normals:
                nx, ny, nz = face_normal(mesh, face)
                normal = (self._signed_byte(nx), self._signed_byte(ny),
                          self._signed_byte(nz))
            else:
                normal = (0, 127, 0)
            out.append("    {%d, %d, %d, %d, %d, %d, %d, %d, %d},\n" % (
                i0, i1, i2, rgb[0], rgb[1], rgb[2],
                normal[0], normal[1], normal[2]))
        out.append("};\n\n")
        out.append("}  // namespace\n\n")

        out.append("const pse::MeshData %s = {\n" % self.name)
        out.append("    k_vertices,\n")
        out.append("    static_cast<uint16_t>(%d),\n" % len(mesh.vertices))
        out.append("    k_faces,\n")
        out.append("    static_cast<uint16_t>(%d),\n" % mesh.triangle_count)
        out.append("    static_cast<int16_t>(%d),\n" % self.scale)
        out.append("};\n\n")
        out.append("}  // namespace models\n")
        return "".join(out)

    def flash_bytes(self, mesh):
        return len(mesh.vertices) * 6 + mesh.triangle_count * 12 + 12

    def _fixed(self, value):
        fixed = int(round(value * self.scale))
        if fixed < -32768 or fixed > 32767:
            raise ObjParseError(
                "coordinate %.3f overflows int16 at scale %d. Lower --scale or "
                "use --fit." % (value, self.scale))
        return fixed

    @staticmethod
    def _signed_byte(value):
        return max(-127, min(127, int(round(value * 127.0))))


def write_if_changed(path, text):
    """Rewrite only when the content actually differs.

    When the content matches we still bump the mtime. Skipping that entirely
    looks tidier but makes the Unix Makefiles generator consider the output
    permanently out of date, so it re-runs this converter on every single build
    forever. Ninja is immune because CMake marks custom commands `restat`, which
    is exactly the sort of difference that makes a bug like this show up only on
    someone else's machine.
    """
    os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
    if os.path.isfile(path):
        with open(path, "r", encoding="utf-8") as handle:
            if handle.read() == text:
                os.utime(path, None)
                return False
    with open(path, "w", encoding="utf-8") as handle:
        handle.write(text)
    return True


def parse_args(argv):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("obj", help="input .obj file")
    parser.add_argument("--out-cpp", required=True)
    parser.add_argument("--out-hpp", required=True)
    parser.add_argument("--name", help="C++ identifier, defaults to file stem")
    parser.add_argument("--scale", type=int, default=256,
                        help="fixed point units per world unit (default 256). This is\n"
                             "independent of the engine fixed point scale: the\n"
                             "renderer divides by MeshData::scale. 256 keeps\n"
                             "models under 128 world units in int16.")
    parser.add_argument("--fit", type=float, default=0.0,
                        help="rescale so the largest half extent is this many "
                             "world units. 0 disables (default)")
    parser.add_argument("--no-normals", action="store_true",
                        help="skip per face normals to save 3 bytes per face")
    return parser.parse_args(argv)


def main(argv):
    args = parse_args(argv)

    name = args.name or os.path.splitext(os.path.basename(args.obj))[0]
    name = SAFE_NAME.sub("_", name)
    if name[:1].isdigit():
        name = "model_" + name

    materials = MaterialLibrary()
    try:
        mesh = ObjReader(materials).read(args.obj)
    except (ObjParseError, OSError) as error:
        sys.stderr.write("obj2cpp: %s\n" % error)
        return 1

    if args.fit > 0.0:
        factor = fit_scale(mesh, args.fit)
        mesh.vertices = [(x * factor, y * factor, z * factor)
                         for x, y, z in mesh.vertices]

    emitter = CppEmitter(name, os.path.basename(args.obj), args.scale)
    try:
        header = emitter.emit_header(mesh)
        source = emitter.emit_source(mesh, not args.no_normals)
    except ObjParseError as error:
        sys.stderr.write("obj2cpp: %s\n" % error)
        return 1

    write_if_changed(args.out_hpp, header)
    write_if_changed(args.out_cpp, source)

    sys.stderr.write("obj2cpp: %s -> %s (%d verts, %d tris, %d bytes flash)\n"
                     % (os.path.basename(args.obj), name, len(mesh.vertices),
                        mesh.triangle_count, emitter.flash_bytes(mesh)))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
