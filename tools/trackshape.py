#!/usr/bin/env python3
"""Turning a drawn track shape into a road a pod can actually drive.

A racing circuit has two halves to its design and they fight each other. One
is CHARACTER: where the long straight is, where the switchback goes, which way
the tail hooks, whether the thing reads as a place rather than as a ring. The
other is whether any given corner can physically be taken at the speed the
track arrives at it. Authoring both at once by moving coordinates in a text
editor does not converge; it goes fix the hairpin, discover the tail is now 14
units of radius, fix the tail, discover the hairpin came back.

So this module splits them. A shape is authored purely for character, as a
closed ring of control points, and then RELAXED until every corner clears a
target radius. What comes out is committed, the way tools/gen_tomlander_props.py
writes .obj files that are then committed and editable.

Two things here are not obvious and both were learned the hard way.

WHY CONTROL POINTS, NOT TURN COMMANDS. The first version of this authored a
track as "turn 46 degrees over 170 units, then run straight for 120". A plan
like that closes its HEADING, not its POSITION: to land back where it started
every plan had to be forced to turn the same way the whole way round, and a
shape that only ever turns one way is an oval. Four tracks, four ovals, and
they were perfectly pleasant to drive, so nothing about driving them said so.
The minimap said so. Control points close by construction, which frees every
corner to go whichever way the track wants.

WHY THE RELAXER MEASURES THE SAMPLED CURVE. The first relaxer measured the
angle at each control point and did almost nothing, because Catmull-Rom passes
THROUGH its control points but bulges BETWEEN them: a polygon whose corners all
look gentle can still carry a 7 unit radius halfway along a span. The curvature
has to be measured on the same resampled nodes the game will drive on, and the
blame for a tight node assigned back to the control points nearest it.
"""

import math

# World units between nodes. The only thing that decides what a track costs in
# flash: at 8 units a 2,400 unit lap is 300 nodes, and a node is 8 bytes.
NODE_SPACING = 8.0

# How finely the spline is sampled before the constant arc length resample.
# Sampling at constant PARAMETER instead would bunch nodes into the corners and
# stretch them down the straights, and since a node is also the collision
# resolution that would make the hairpins accurate and the straights coarse,
# which is backwards.
SUBDIVISIONS = 40


def catmull(p0, p1, p2, p3, u):
    """Catmull-Rom, which passes through its control points rather than near
    them. That matters for authoring: a point dropped where the hairpin should
    be is where the hairpin is."""
    u2, u3 = u * u, u * u * u

    def axis(a, b, c, d):
        return 0.5 * ((2 * b) + (-a + c) * u + (2 * a - 5 * b + 4 * c - d) * u2
                      + (-a + 3 * b - 3 * c + d) * u3)

    return (axis(p0[0], p1[0], p2[0], p3[0]), axis(p0[1], p1[1], p2[1], p3[1]))


def sample(control, spacing=NODE_SPACING):
    """The closed spline through `control`, resampled at constant arc length.

    Returns (points, total_length). `points` is what the game drives on.
    """
    m = len(control)
    dense = []
    for i in range(m):
        p0 = control[(i - 1) % m]
        p1 = control[i]
        p2 = control[(i + 1) % m]
        p3 = control[(i + 2) % m]
        for k in range(SUBDIVISIONS):
            dense.append(catmull(p0, p1, p2, p3, k / SUBDIVISIONS))

    n = len(dense)
    cum = [0.0]
    for i in range(1, n + 1):
        a, b = dense[i - 1], dense[i % n]
        cum.append(cum[i - 1] + math.hypot(b[0] - a[0], b[1] - a[1]))
    total = cum[n]

    count = max(24, round(total / spacing))
    step = total / count
    out = []
    seek = 0
    for i in range(count):
        want = i * step
        while seek < n - 1 and cum[seek + 1] < want:
            seek += 1
        span = cum[seek + 1] - cum[seek] or 1.0
        u = (want - cum[seek]) / span
        a, b = dense[seek], dense[(seek + 1) % n]
        out.append((a[0] + (b[0] - a[0]) * u, a[1] + (b[1] - a[1]) * u))
    return out, total


def headings(points):
    """Heading at each node, from its neighbours rather than from the segment
    ahead, so a node's heading does not lag half a segment behind the curve."""
    n = len(points)
    return [math.atan2(points[(i + 1) % n][0] - points[(i - 1) % n][0],
                       points[(i + 1) % n][1] - points[(i - 1) % n][1])
            for i in range(n)]


def wrap_pi(a):
    while a > math.pi:
        a -= math.tau
    while a < -math.pi:
        a += math.tau
    return a


def radii(points, spacing=NODE_SPACING):
    """Turn radius at each node, in world units. `inf` where the road is
    straight. This is the number that decides whether a corner is takeable."""
    head = headings(points)
    n = len(points)
    out = []
    for i in range(n):
        d = abs(wrap_pi(head[(i + 1) % n] - head[i]))
        out.append(math.inf if d < 1e-4 else spacing / d)
    return out


def relax(control, target, iterations=600, rate=0.16):
    """Open every corner until it clears `target`, without moving the track.

    Each node under target blames the two control points nearest it, and those
    points move toward the midpoint of THEIR neighbours, which is the one move
    that opens a corner while leaving the shape where it was drawn. Straights
    are untouched, because a straight has no curvature to relax.

    `target` is not a taste number. The pod turns at roughly
    YAW_MAX * (1 - YAW_SPEED_FALL * v/vmax) radians a second, so a radius is
    v / omega: about 103 units at full speed, and about 20 at 45 u/s on the air
    brake, which multiplies yaw authority by 1.75. A corner at 45 units is
    therefore one you brake for and take.
    """
    pts = [list(p) for p in control]
    m = len(pts)
    for _ in range(iterations):
        nodes, _total = sample(pts)
        rs = radii(nodes)
        if min(rs) >= target:
            break
        pressure = [0.0] * m
        for radius, node in zip(rs, nodes):
            if radius >= target:
                continue
            order = sorted(range(m),
                           key=lambda i: math.hypot(pts[i][0] - node[0],
                                                    pts[i][1] - node[1]))
            weight = min(1.0, (target - radius) / target)
            pressure[order[0]] += weight
            pressure[order[1]] += weight * 0.5
        nxt = [p[:] for p in pts]
        for i in range(m):
            if pressure[i] <= 0:
                continue
            a, c = pts[(i - 1) % m], pts[(i + 1) % m]
            k = rate * min(1.0, pressure[i])
            nxt[i][0] += ((a[0] + c[0]) / 2 - pts[i][0]) * k
            nxt[i][1] += ((a[1] + c[1]) / 2 - pts[i][1]) * k
        pts = nxt
    return [(round(p[0]), round(p[1])) for p in pts]


def measure(control):
    """Everything worth knowing about a shape, as a dict.

    `direction_changes` is the one that separates a circuit from an oval: it
    counts how many times the road stops turning one way and starts turning the
    other. An oval scores zero however pretty it is.
    """
    nodes, total = sample(control)
    rs = radii(nodes)
    head = headings(nodes)
    n = len(nodes)

    flips = 0
    sign = 0
    for i in range(n):
        d = wrap_pi(head[(i + 1) % n] - head[i])
        s = sign if abs(d) < 0.004 else (1 if d > 0 else -1)
        if s and sign and s != sign:
            flips += 1
        if s:
            sign = s

    finite = [r for r in rs if r < 1e8]
    tightest_at = min(range(n), key=lambda i: rs[i])
    return {
        "length": total,
        "nodes": n,
        "tightest": min(finite) if finite else math.inf,
        "tightest_at": tightest_at / n,
        "straight_fraction": sum(1 for r in rs if r > 400) / n,
        "direction_changes": flips,
        "closure_error": math.hypot(nodes[0][0] - nodes[-1][0],
                                    nodes[0][1] - nodes[-1][1]),
    }


def report(name, control):
    m = measure(control)
    print(f"{name:<10} len {m['length']:>6.0f}  nodes {m['nodes']:>4}  "
          f"tightest {m['tightest']:>5.0f} at {m['tightest_at']:.2f}  "
          f"straight {m['straight_fraction'] * 100:>3.0f}%  "
          f"direction changes {m['direction_changes']}")
    return m


def plot(name, control, width=74, height=28):
    """An ASCII plot, because looking at a shape is the only way to judge one
    and a browser round trip per iteration is not a design loop."""
    nodes, _ = sample(control)
    xs = [p[0] for p in nodes]
    zs = [p[1] for p in nodes]
    x0, x1, z0, z1 = min(xs), max(xs), min(zs), max(zs)
    s = min((width - 2) / max(1e-6, x1 - x0), (height - 2) / max(1e-6, z1 - z0))
    grid = [[" "] * width for _ in range(height)]
    for i, (x, z) in enumerate(nodes):
        cx = round((x - x0) * s) + 1
        cy = height - 1 - round((z - z0) * s)
        if 0 <= cy < height and 0 <= cx < width:
            grid[cy][cx] = "S" if i == 0 else "#"
    print(f"--- {name} ---")
    print("\n".join("".join(row) for row in grid))
