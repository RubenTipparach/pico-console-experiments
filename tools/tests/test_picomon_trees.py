#!/usr/bin/env python3
"""Which of Picomon's trees are sprites and which are geometry.

A tree tile is drawn one of two ways and the choice is not stylistic. The
wall of trees that frames a map is over a hundred tiles: at twenty triangles
each that is more geometry than everything else in the frame put together, on
a chip with no FPU, so it has to be billboards. A tree standing on its own
inside the playable area is one of under twenty, the player walks around it
and sees more than one side of it, and it is worth the triangles.

tools/picomon_data.py tells them apart with two rules: flood inward from the
map edge across tree tiles, and then also promote anything the flood DID
reach that stands with three or more open sides, because that is a spur the
player walks around rather than a wall they walk past.

This tests that flood fill, on maps built to make its failure modes obvious
and then on the real ones. It matters because both ways of getting it wrong
are invisible on a screenshot: flood too far and every tree quietly goes back
to being a sprite, flood not far enough and a hundred and twenty border tiles
each cost twenty triangles, which shows up as a frame rate on hardware and
nowhere else.

The models are checked too. Twenty triangles is the budget the whole split is
justified by, so a tree mesh that grows past it invalidates the arithmetic
above without failing anything.

Usage:
    test_picomon_trees.py
"""
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, "tools"))

import picomon_data


def fail(msg):
    print(f"FAIL: {msg}")
    sys.exit(1)


def load():
    d = picomon_data.Data(os.path.join(ROOT, "games", "picomon", "data"))
    d.load_tileset()
    d.load_moves()
    d.load_species()
    d.load_items()
    d.load_zones()
    d.load_start()
    d.seal_text()
    d.check()
    return d


def tile_ids(d):
    tree = next(i for i, t in enumerate(d.tiles) if t["name"] == "tree")
    core = next(i for i, t in enumerate(d.tiles) if t["name"] == "treecore")
    return tree, core


def split(rows, w, h, tree, core):
    """Run the real split over a throwaway map.

    Tile 0 is open ground and both tree tiles block, which is what the open
    side count reads.
    """
    d = picomon_data.Data.__new__(picomon_data.Data)
    block = picomon_data.TILE_FLAGS["block"]
    d.tiles = [{"name": "", "flags": 0} for _ in range(max(tree, core) + 1)]
    d.tiles[tree] = {"name": "tree", "flags": block}
    d.tiles[core] = {"name": "treecore", "flags": block}
    z = {"w": w, "h": h, "tiles": [list(r) for r in rows]}
    d.zones = [z]
    picomon_data.Data.split_trees(d)
    return z["tiles"], z["mesh_trees"]


def test_a_ring_is_all_border():
    """A hollow box of trees is entirely reachable from outside."""
    T, C = 7, 20
    g = [[0] * 5 for _ in range(5)]
    for i in range(5):
        g[0][i] = g[4][i] = g[i][0] = g[i][4] = T
    out, n = split(g, 5, 5, T, C)
    if n != 0:
        fail(f"a ring of trees produced {n} mesh trees, expected 0: every tile "
             "of it touches the outside")
    print("  a ring of trees is all border")


def test_an_island_is_interior():
    """One tree with open ground all round it is geometry."""
    T, C = 7, 20
    g = [[0] * 5 for _ in range(5)]
    g[2][2] = T
    out, n = split(g, 5, 5, T, C)
    if n != 1 or out[2][2] != C:
        fail(f"a lone tree at 2,2 produced {n} mesh trees and tile "
             f"{out[2][2]}, expected 1 and {C}")
    print("  a lone tree inside the map is geometry")


def test_a_thick_treeline_stays_sprites():
    """A deep band of trees along the edge is all border.

    This is the case a distance rule gets wrong. Hometown's northern
    treeline is six rows deep, and the player only ever sees its near face,
    so every tile of it is a billboard however far in it runs.
    """
    T, C = 7, 20
    g = [[0] * 8 for _ in range(8)]
    for y in range(4):         # a four row band across the top
        for x in range(8):
            g[y][x] = T
    out, n = split(g, 8, 8, T, C)
    if n != 0:
        fail(f"a four row treeline across the top produced {n} mesh trees, "
             "expected 0: nothing in it has three open sides")
    print("  a deep treeline hanging off the edge stays sprites")


def test_a_thin_spur_becomes_geometry():
    """One tile wide is not a treeline, it is a tree the player walks round.

    Reaching the edge is not enough on its own. A single file of trees
    poking into the field is seen from three sides as the player passes, and
    it looked exactly like what it was on Route 1: a flat billboard standing
    in open grass. Counting open sides is what tells it from a thick band.
    """
    T, C = 7, 20
    g = [[0] * 7 for _ in range(7)]
    for x in range(7):
        g[0][x] = T            # the border ring's top row
    for y in range(1, 5):
        g[y][3] = T            # a one wide spur hanging off it
    out, n = split(g, 7, 7, T, C)
    if n != 4:
        fail(f"a one wide spur off the top edge produced {n} mesh trees, "
             "expected 4: every tile of it has open ground on three sides")
    for y in range(1, 5):
        if out[y][3] != C:
            fail(f"the spur tile at 3,{y} stayed a sprite")
    if any(out[0][x] != T for x in range(7)):
        fail("the border row itself became geometry, and it has at most one "
             "open side anywhere along it")
    print("  a one tile wide spur off the edge is geometry")


def test_a_pocket_inside_a_thicket_is_interior():
    """Trees walled off from the edge by other trees are still interior.

    Only the flood fill can tell: by any distance measure these are as close
    to the edge as the ring around them.
    """
    T, C = 7, 20
    g = [[0] * 7 for _ in range(7)]
    for i in range(7):
        g[0][i] = g[6][i] = g[i][0] = g[i][6] = T
    g[3][3] = T                # an island in the middle of the hole
    out, n = split(g, 7, 7, T, C)
    if n != 1 or out[3][3] != C:
        fail(f"an island inside a ring produced {n} mesh trees, expected 1")
    print("  an island inside a ring of trees is geometry")


def test_the_real_maps():
    d = load()
    tree, core = tile_ids(d)
    total_border = total_inside = 0
    for z in d.zones:
        w, h, rows = z["w"], z["h"], z["tiles"]
        border = sum(r.count(tree) for r in rows)
        inside = sum(r.count(core) for r in rows)
        total_border += border
        total_inside += inside
        for y in range(h):
            for x in range(w):
                if rows[y][x] != core:
                    continue
                if x in (0, w - 1) or y in (0, h - 1):
                    fail(f"{z['id']}: the tree at {x},{y} is on the outermost "
                         "ring and became geometry, which the flood fill "
                         "starts from and can never do")
        if inside:
            print(f"  {z['id']:10s} {border:3d} border sprites, "
                  f"{inside:2d} interior meshes")
    if total_inside == 0:
        fail("no zone has a single interior tree, so nothing is geometry and "
             "the whole split does nothing")
    if total_border < 100:
        fail(f"only {total_border} border trees left, so the flood fill is "
             "turning the treeline itself into geometry")
    if total_inside * 4 >= total_border:
        fail(f"{total_inside} interior against {total_border} border: the "
             "interior is meant to be the small half, because it is the half "
             "that costs twenty triangles a tile")


def test_the_meshes_stay_inside_their_budget():
    """Twenty triangles is the number the split is justified by."""
    models = os.path.join(ROOT, "games", "picomon", "models")
    for name, limit in (("treepine.obj", 20), ("treeleaf.obj", 20)):
        path = os.path.join(models, name)
        tris = 0
        base = None
        top = None
        for line in open(path):
            if line.startswith("f "):
                tris += len(line.split()) - 3
            elif line.startswith("v "):
                y = float(line.split()[2])
                base = y if base is None else min(base, y)
                top = y if top is None else max(top, y)
        if tris > limit:
            fail(f"{name} is {tris} triangles, over its budget of {limit}. "
                 "Route 1 can hold nineteen of these on screen at once")
        if abs(base) > 1e-6:
            fail(f"{name}'s lowest vertex is at y={base}, not 0, so the game "
                 "cannot place it by its feet")
        print(f"  {name:14s} {tris} triangles, {top:.2f} units tall")


def main():
    test_a_ring_is_all_border()
    test_an_island_is_interior()
    test_a_thick_treeline_stays_sprites()
    test_a_thin_spur_becomes_geometry()
    test_a_pocket_inside_a_thicket_is_interior()
    test_the_real_maps()
    test_the_meshes_stay_inside_their_budget()
    print("picomon tree split tests passed")


if __name__ == "__main__":
    main()
