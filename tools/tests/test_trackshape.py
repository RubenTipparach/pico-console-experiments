#!/usr/bin/env python3
"""The track shaper, checked.

tools/trackshape.py is the only thing standing between an authored shape and a
corner no pod on the roster can turn, and its one subtlety is that it measures
the SAMPLED curve rather than the control polygon. The first version measured
the polygon and did essentially nothing, because Catmull-Rom passes through its
control points but bulges between them: a polygon whose corners all look gentle
can still carry a 7 unit radius halfway along a span. Nothing about that is
visible in the output shape, so it needs a test.
"""

import math
import pathlib
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))

import trackshape  # noqa: E402
import gen_twinflare_tracks as gen  # noqa: E402


FAILURES = []


def check(ok, what):
    if not ok:
        FAILURES.append(what)
        print(f"FAIL: {what}")


def test_resampling_is_even():
    """A node every NODE_SPACING, all the way round including the wrap.

    Sampling the spline at constant PARAMETER instead of constant arc length
    would bunch nodes into the corners and stretch them down the straights,
    which matters beyond looks: a node is also the collision resolution, so
    that would make the hairpins accurate and the straights coarse.
    """
    for track in gen.TRACKS:
        points, _total = trackshape.sample(track["shape"])
        n = len(points)
        for i in range(n):
            a, b = points[i], points[(i + 1) % n]
            d = math.hypot(b[0] - a[0], b[1] - a[1])
            check(abs(d - trackshape.NODE_SPACING) < trackshape.NODE_SPACING * 0.35,
                  f"{track['key']}: node {i} is {d:.2f} from its neighbour")
            if FAILURES:
                return


def test_a_ring_closes():
    """A closed ring, by construction rather than by a fudge at the end.

    The format this replaced was a list of turn-and-length commands, which
    closes its HEADING rather than its POSITION: every track had to be forced
    to turn the same way all the way round to land back at its start, and a
    shape that only ever turns one way is an oval.
    """
    for track in gen.TRACKS:
        m = trackshape.measure(track["shape"])
        check(m["closure_error"] < trackshape.NODE_SPACING * 1.5,
              f"{track['key']}: ring does not close ({m['closure_error']:.1f})")


def test_relax_opens_every_corner():
    """The whole job. Relaxing has to reach the target from a shape that
    starts well under it."""
    for track in gen.TRACKS:
        before = trackshape.measure(track["shape"])["tightest"]
        relaxed = trackshape.relax(track["shape"], track["min_radius"])
        after = trackshape.measure(relaxed)["tightest"]
        check(after >= track["min_radius"] - 1,
              f"{track['key']}: relaxed to {after:.0f}, wanted {track['min_radius']}")
        check(after >= before,
              f"{track['key']}: relaxing made the tightest corner worse")
        print(f"  {track['key']:<7} tightest {before:>4.0f} -> {after:>4.0f} u "
              f"(target {track['min_radius']})")


def test_relax_keeps_the_shape():
    """Opening a corner must not redraw the track. The lap length and the
    number of times the road changes which way it turns are what carry a
    circuit's character, and a relaxer that quietly rounded everything into an
    oval would pass the radius check and lose the game."""
    for track in gen.TRACKS:
        before = trackshape.measure(track["shape"])
        relaxed = trackshape.relax(track["shape"], track["min_radius"])
        after = trackshape.measure(relaxed)
        ratio = after["length"] / before["length"]
        check(0.75 < ratio < 1.25,
              f"{track['key']}: length moved by {(ratio - 1) * 100:.0f}%")
        check(after["direction_changes"] >= 4,
              f"{track['key']}: only {after['direction_changes']} direction "
              f"changes, which is an oval with extra steps")


def test_a_circle_is_left_alone():
    """A shape that already clears the target must come out unchanged, or the
    relaxer is shrinking every track it touches rather than only the corners
    that need it."""
    circle = [(round(200 * math.cos(a * math.tau / 12)),
               round(200 * math.sin(a * math.tau / 12))) for a in range(12)]
    before = trackshape.measure(circle)
    relaxed = trackshape.relax(circle, 40)
    after = trackshape.measure(relaxed)
    check(abs(after["length"] - before["length"]) < before["length"] * 0.02,
          "a circle well inside the target was reshaped anyway")


def test_the_generated_tracks_match_the_generator():
    """The committed header is what the generator writes today.

    The relaxed coordinates are generated and committed, the way
    gen_tomlander_props.py writes .obj files that are then committed, so this
    is the check that nobody has hand edited the output or changed a shape
    without rerunning the tool.
    """
    header = ROOT.parent / "games/twinflare/src/track_data.hpp"
    if not header.exists():
        check(False, "games/twinflare/src/track_data.hpp is missing")
        return
    committed = header.read_text()
    for track in gen.TRACKS:
        _control, nodes, _total = gen.build(track)
        first = nodes[0]
        row = "{{{:>6}, {:>6}, {:>5}, {:>4}, {:>3}}},".format(
            round(first["x"] * 16), round(first["z"] * 16),
            round(first["y"] * 16), round(first["hw"] * 16), first["flags"])
        check(row in committed,
              f"{track['key']}: the committed table does not match the "
              f"generator, rerun tools/gen_twinflare_tracks.py")
        check(f"{track['key']}_nodes" in committed,
              f"{track['key']}: no node table in the committed header")


def main():
    test_resampling_is_even()
    test_a_ring_closes()
    test_relax_opens_every_corner()
    test_relax_keeps_the_shape()
    test_a_circle_is_left_alone()
    test_the_generated_tracks_match_the_generator()
    if FAILURES:
        print(f"{len(FAILURES)} failure(s)")
        return 1
    print("trackshape tests passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
