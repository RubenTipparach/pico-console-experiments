#!/usr/bin/env python3
"""Build the Twin Flare track viewer: one self contained WebGL page per run.

The page is `tools/twinflare_viewer.html` with its `/*__DATA__*/` placeholder
replaced by a dump of every track's world geometry. That dump comes from
`twinflare_export`, a host binary that links the GAME'S OWN sim and renderer
and asks `ground_slice()` where each band boundary is.

That indirection is the whole point. A viewer that rebuilt the cross section
from the track tables would be a second opinion about the geometry, and a
second opinion cannot audit the first one: the mitred normal, the fold clamp,
`ground_offset`, the waterline, every one of those has been wrong at some
point, and every one of them would have been wrong identically in a viewer
that reimplemented it. What this tool ships is the geometry the device draws.

Usage:
    gen_twinflare_viewer.py                       build, export, write the page
    gen_twinflare_viewer.py --out /tmp/v.html     somewhere else
    gen_twinflare_viewer.py --json tracks.json    reuse an existing dump
"""

import argparse
import json
import os
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
TEMPLATE = os.path.join(HERE, "twinflare_viewer.html")
PLACEHOLDER = "/*__DATA__*/"


class ViewerError(Exception):
    """Raised when the page cannot be built."""


def build_exporter(build_dir):
    """Configure and build `twinflare_export`, returning the binary's path.

    The host test tree is what carries the exporter, so this is the same
    configure the engine tests use. CMAKE_POLICY_VERSION_MINIMUM is set for the
    same reason the rest of the repo sets it: the vendored SDK asks for a
    policy version newer CMake no longer offers by default.
    """
    if not shutil.which("cmake"):
        raise ViewerError("cmake is not on PATH")

    subprocess.run(
        ["cmake", "-S", ROOT, "-B", build_dir,
         "-DBUILD_ENGINE_TESTS=ON",
         "-DCMAKE_BUILD_TYPE=Release",
         "-DCMAKE_POLICY_VERSION_MINIMUM=3.5"],
        check=True, stdout=subprocess.DEVNULL,
    )
    subprocess.run(
        ["cmake", "--build", build_dir, "--target", "twinflare_export",
         "--parallel", str(os.cpu_count() or 2)],
        check=True, stdout=subprocess.DEVNULL,
    )

    for candidate in (
        os.path.join(build_dir, "games", "twinflare", "tests", "twinflare_export"),
        os.path.join(build_dir, "twinflare_export"),
    ):
        if os.path.exists(candidate):
            return candidate
    raise ViewerError("twinflare_export built but was not found in %s" % build_dir)


def export(binary):
    """Run the exporter and return its dump as a dict."""
    done = subprocess.run([binary], check=True, capture_output=True, text=True)
    return json.loads(done.stdout)


def shrink(data, places=2):
    """Round every coordinate to `places`, so the page is not mostly digits.

    The dump prints three decimals of a unit, which on a 240 pixel screen at
    the far plane is several thousandths of a pixel. Two decimals is already
    finer than anything the viewer can show and it takes about a fifth off the
    file.
    """
    if isinstance(data, float):
        rounded = round(data, places)
        return int(rounded) if rounded == int(rounded) else rounded
    if isinstance(data, list):
        return [shrink(v, places) for v in data]
    if isinstance(data, dict):
        return {k: shrink(v, places) for k, v in data.items()}
    return data


def render(data):
    """Substitute the dump into the template and return the finished page."""
    with open(TEMPLATE, "r", encoding="utf-8") as handle:
        page = handle.read()

    if page.count(PLACEHOLDER) != 1:
        raise ViewerError(
            "%s must contain exactly one %s, found %d"
            % (TEMPLATE, PLACEHOLDER, page.count(PLACEHOLDER)))

    blob = json.dumps(shrink(data), separators=(",", ":"))
    # A `</script>` inside the blob would end the script element early. Nothing
    # in a track dump contains one today, and this is two characters of belt.
    blob = blob.replace("</", "<\\/")
    return page.replace(PLACEHOLDER, blob)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", default=os.path.join(ROOT, "build.viewer",
                                                      "twinflare_viewer.html"))
    parser.add_argument("--json", help="reuse a dump instead of building one")
    parser.add_argument("--build-dir", default=os.path.join(ROOT, "build.test"))
    args = parser.parse_args(argv)

    try:
        if args.json:
            with open(args.json, "r", encoding="utf-8") as handle:
                data = json.load(handle)
        else:
            data = export(build_exporter(args.build_dir))
        page = render(data)
    except (ViewerError, subprocess.CalledProcessError, ValueError) as err:
        sys.stderr.write("gen_twinflare_viewer: %s\n" % err)
        return 1

    os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
    with open(args.out, "w", encoding="utf-8") as handle:
        handle.write(page)

    nodes = sum(len(t["nodes"]) for t in data["tracks"])
    print("%s: %d tracks, %d nodes, %.0f KB"
          % (args.out, len(data["tracks"]), nodes, len(page) / 1024))
    return 0


if __name__ == "__main__":
    sys.exit(main())
