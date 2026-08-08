#!/usr/bin/env python3
"""Build Twin Flare's four circuits.

The shapes below are AUTHORED: drawn for character, with no attention paid to
whether any given corner can be taken. tools/trackshape.py then relaxes each
one until every corner clears a radius the pod can actually turn, resamples it
at a constant 8 units, and this writes the result out as a const table in
flash.

That split is the point. Authoring shape and drivability at the same time does
not converge, and the previous format could not even express an interesting
shape: a track was a list of "turn 46 degrees over 170 units" commands, and a
plan like that closes its HEADING rather than its POSITION, so every track had
to turn the same way the whole way round to get back to its start line. Four
tracks, four ovals, all perfectly pleasant to drive. Nothing about driving them
said so; the minimap said so.

Usage:
    tools/gen_twinflare_tracks.py                     write the header
    tools/gen_twinflare_tracks.py --report            measure, write nothing
    tools/gen_twinflare_tracks.py --plot              draw them in the terminal
"""

import argparse
import pathlib
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))

from trackshape import NODE_SPACING, measure, plot, relax, report, sample  # noqa: E402

# Feature flags, and they must match games/twinflare/src/tracks.hpp.
F_RAMP, F_GAP, F_BOOST, F_WALL, F_SHORT = 1, 2, 4, 8, 16

# Half widths, in world units. A walled section is narrower because the walls
# are the point; a shortcut is wider because it has to be worth aiming at.
HALF_WIDTH = {"default": 9.5, "wall": 8.5, "short": 11.0}


TRACKS = [
    dict(
        key="dune", name="DUNE SEA", laps=3, min_radius=46,
        # The boot. A big fast lobe on the right, then a long peninsula
        # running west with a hook on the end: the road goes out and comes
        # back beside itself, which is the shape the reference game's tracks
        # have and no oval ever will.
        shape=[
            (-120, -210), (130, -224), (320, -186), (432, -80), (428, 62),
            (320, 168), (168, 200), (40, 150), (-40, 60), (-160, 96),
            (-320, 128), (-470, 120), (-560, 40), (-500, -60), (-360, -76),
            (-200, -104), (-60, -130),
        ],
        elevation=[0, 0, 4, 10, 6, -2, -8, -4, 2, 6, 2, -2, 0],
        features=[
            (0.06, 26, F_BOOST), (0.20, 110, F_RAMP), (0.255, 56, F_GAP),
            (0.36, 210, F_WALL), (0.50, 140, F_SHORT), (0.63, 110, F_RAMP),
            (0.78, 26, F_BOOST),
        ],
    ),
    dict(
        key="tide", name="TIDEBREAK", laps=3, min_radius=46,
        # The inlet. A long causeway around the outside, then the road cuts
        # hard inland, runs down a trench and comes back out.
        shape=[
            (-300, -170), (20, -214), (280, -160), (400, -30), (356, 110),
            (210, 190), (60, 150), (10, 30), (-70, -40), (-190, 20),
            (-230, 150), (-350, 210), (-470, 130), (-450, -20), (-390, -120),
        ],
        elevation=[6, 10, 12, 8, 2, -6, -14, -18, -12, -4, 2, 6],
        features=[
            (0.10, 110, F_RAMP), (0.24, 210, F_WALL), (0.36, 26, F_BOOST),
            (0.46, 140, F_SHORT), (0.565, 56, F_GAP), (0.70, 110, F_RAMP),
            (0.88, 26, F_BOOST),
        ],
    ),
    dict(
        key="ash", name="ASHFALL", laps=2, min_radius=46,
        # The dumbbell. Two lobes joined by two long straights, which is where
        # the boost goes and why the lap is the longest here.
        shape=[
            (-560, -70), (-410, -186), (-190, -214), (40, -206), (300, -186),
            (496, -104), (566, 46), (470, 176), (296, 206), (150, 120),
            (30, 190), (-140, 208), (-300, 128), (-430, 190), (-540, 90),
        ],
        elevation=[0, 8, 18, 10, -4, -14, -8, 2, 10, 14, 8, -4, -6],
        features=[
            (0.02, 26, F_BOOST), (0.17, 110, F_RAMP), (0.225, 56, F_GAP),
            (0.34, 210, F_WALL), (0.47, 140, F_SHORT), (0.60, 110, F_RAMP),
            (0.655, 56, F_GAP), (0.82, 26, F_BOOST),
        ],
    ),
    dict(
        key="frost", name="HOARFROST", laps=3, min_radius=40,
        # The tangle. Nothing here is a straight for long, and on half grip
        # that is the whole character of the track.
        shape=[
            (-330, -160), (-170, -230), (-20, -140), (130, -236), (300, -170),
            (400, -40), (320, 90), (160, 40), (40, 140), (-110, 180),
            (-200, 70), (-330, 150), (-440, 50), (-350, -50), (-450, -120),
        ],
        elevation=[0, 5, 9, 4, -3, -7, -2, 5, 8, 3, -3, -2],
        features=[
            (0.08, 210, F_WALL), (0.20, 26, F_BOOST), (0.27, 210, F_WALL),
            # 32 units, where the other three run 56. HOARFROST has half the
            # grip, so the slowest pod on the roster never really gets going
            # and its measured worst case jump there is 78 units. At 56 that
            # clears by one and a half and at 40 by not quite two, which is a
            # jump you make by not making a mistake rather than a jump you
            # make. The gap is a thing in metres precisely so it can differ
            # per track when the track's physics differ.
            (0.40, 110, F_RAMP), (0.45, 32, F_GAP), (0.545, 140, F_SHORT),
            (0.64, 210, F_WALL), (0.80, 110, F_RAMP), (0.90, 26, F_BOOST),
        ],
    ),
]


def elevation_at(keys, frac):
    """Elevation around the lap from keyframes, as a closed Catmull-Rom on one
    axis, so the hill at the end of the lap meets the dip at the start without
    a step."""
    m = len(keys)
    f = (frac % 1.0) * m
    i = int(f)
    u = f - i
    p0, p1 = keys[(i - 1) % m], keys[i % m]
    p2, p3 = keys[(i + 1) % m], keys[(i + 2) % m]
    u2, u3 = u * u, u * u * u
    return 0.5 * ((2 * p1) + (-p0 + p2) * u + (2 * p0 - 5 * p1 + 4 * p2 - p3) * u2
                  + (-p0 + 3 * p1 - 3 * p2 + p3) * u3)


def build(track):
    """Relax, resample, and hang the elevation and the features on the nodes."""
    control = relax(track["shape"], track["min_radius"])
    points, total = sample(control)
    count = len(points)

    flags = [0] * count
    for at, length, flag in track["features"]:
        # WHERE a feature sits is a fraction of the lap. HOW LONG it is, is in
        # world units, and the two are different on purpose: a gap written as
        # 3 percent of the lap was 56 units on the short track and 83 on the
        # long one, so making a track bigger silently made its jumps harder.
        # The length of a hole in the road is a thing in metres.
        span = max(1, round(length / NODE_SPACING))
        start = round(at * count)
        for k in range(span):
            flags[(start + k) % count] |= flag

    nodes = []
    for i, (x, z) in enumerate(points):
        f = flags[i]
        hw = (HALF_WIDTH["wall"] if f & F_WALL else
              HALF_WIDTH["short"] if f & F_SHORT else HALF_WIDTH["default"])
        nodes.append(dict(x=x, z=z, y=elevation_at(track["elevation"], i / count),
                          hw=hw, flags=f))
    return control, nodes, total


HEADER = """// GENERATED by tools/gen_twinflare_tracks.py. Do not edit.
//
// Four circuits, each authored as a ring of control points, relaxed until
// every corner clears a radius the pod can turn, then resampled at a constant
// {spacing:.0f} world units. Edit the shapes in the generator and run it again.
//
// Node coordinates are fp4 (16 = one world unit). A track reaches about 600
// units from its centre, which is 9,600 in fp4 and inside an int16, so a node
// is 8 bytes and a whole circuit is under 2.5 KB of flash. The resolution is
// a sixteenth of a unit, about six centimetres of road, which is finer than
// anything the collision or the renderer can see.

#pragma once

#include <cstdint>

#include "tracks.hpp"

namespace twinflare {{
namespace generated {{

"""


def emit(out_path):
    parts = [HEADER.format(spacing=NODE_SPACING)]
    summary = []
    for track in TRACKS:
        control, nodes, total = build(track)
        key = track["key"]
        parts.append(f"// {track['name']}: {len(nodes)} nodes, "
                     f"{total:.0f} units, {track['laps']} laps\n")
        parts.append(f"inline constexpr TrackNode {key}_nodes[] = {{\n")
        for nd in nodes:
            parts.append("    {{{:>6}, {:>6}, {:>5}, {:>4}, {:>3}}},\n".format(
                round(nd["x"] * 16), round(nd["z"] * 16), round(nd["y"] * 16),
                round(nd["hw"] * 16), nd["flags"]))
        parts.append("};\n\n")
        m = measure(control)
        summary.append((track["name"], len(nodes), total, m["tightest"],
                        m["direction_changes"]))
    parts.append("}  // namespace generated\n}  // namespace twinflare\n")
    out_path.write_text("".join(parts))
    return summary


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default="games/twinflare/src/track_data.hpp")
    ap.add_argument("--report", action="store_true")
    ap.add_argument("--plot", action="store_true")
    args = ap.parse_args()

    if args.report or args.plot:
        for track in TRACKS:
            control = relax(track["shape"], track["min_radius"])
            report(track["key"], control)
            if args.plot:
                plot(track["key"], control)
        return 0

    out = pathlib.Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    for name, count, total, tightest, flips in emit(out):
        print(f"{name:<10} {count:>4} nodes  {total:>6.0f} u  "
              f"tightest {tightest:>4.0f} u  direction changes {flips}")
    print(f"wrote {out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
