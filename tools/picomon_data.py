#!/usr/bin/env python3
"""Compile the whole Picomon data directory into one pair of const C++ files.

The device never parses text. Everything under games/picomon/data is read here,
cross checked against everything else, and emitted as const tables that live in
flash and are read straight through the XIP cache.

The cross checking is the point. A zone that warps to a misspelled zone, an
encounter that rolls a species nobody defined, an NPC that waits on a flag
nothing ever sets: all of those are a failed build, not a level that quietly
does not work. A silently ignored key is how a map ends up with an event that
never fires and nobody can see why.

Usage:
    picomon_data.py <data-dir> --out-hpp <path> --out-cpp <path>
"""
import argparse
import os
import re
import sys

# --- limits, all of them the width of something on a 120 pixel screen -------
MAX_ZONE_NAME = 16          # the zone banner
MAX_SPECIES_NAME = 10       # the HP plate
MAX_MOVE_NAME = 10          # the move list, two columns
MAX_ITEM_NAME = 12          # the bag list
MAX_ITEM_DESC = 54          # two lines of 27 under the bag
DIALOGUE_COLS = 28          # a dialogue line at a 4 pixel advance
DIALOGUE_LINES = 3          # what the panel holds
MAX_FLAGS = 64              # the save block's flag bits
MAX_PARTY = 6
MAX_MONEY = 65535           # the wallet is a uint16_t
MAX_STOCK = 8               # what the shop list shows without scrolling

TYPES = ["ember", "tide", "leaf", "spark", "stone", "mind"]
MOVE_EFFECTS = ["none", "lower_def", "lower_spd", "raise_def", "raise_spd", "drain"]
POCKETS = ["balls", "medicine", "key"]
ITEM_EFFECTS = ["ball", "heal", "cure", "revive", "key"]
FACINGS = ["north", "east", "south", "west"]
NPC_KINDS = ["villager", "trainer", "healer", "shop"]
EVENT_KINDS = ["sign", "item", "trigger"]
# Which tree grows in a zone. The order matches art/build_art.py's
# TREE_KINDS, and render.cpp static_asserts that the two agree.
TREE_KINDS = ["pine", "broadleaf"]
TILE_FLAGS = {
    "walk": 0x01, "block": 0x02, "water": 0x04, "encounter": 0x08,
    "door": 0x10, "ledge_north": 0x20, "ledge_east": 0x40,
    "ledge_south": 0x60, "ledge_west": 0x80,
}
# The four ledge directions share two bits with a base of 0x20, so the flag
# byte stays one byte. 0x00 in that field means "not a ledge".
LEDGE_MASK = 0xE0


class DataError(Exception):
    pass


def fail(where, msg):
    raise DataError(f"{where}: {msg}")


def wraps_within(text, cols, lines):
    """Would this text fit the dialogue panel? The same greedy wrap the game
    uses, run at build time so a line that would print through the edge of its
    own panel fails here instead."""
    out, cur = [], ""
    for word in text.split():
        if len(word) > cols:
            return False
        if not cur:
            cur = word
        elif len(cur) + 1 + len(word) <= cols:
            cur += " " + word
        else:
            out.append(cur)
            cur = word
    if cur:
        out.append(cur)
    return len(out) <= lines


def read_records(path):
    """Yield (lineno, indent, tokens) for every meaningful line."""
    with open(path, encoding="utf-8") as f:
        for n, raw in enumerate(f, 1):
            line = raw.split("#", 1)[0].rstrip()
            if not line.strip():
                continue
            yield n, raw.rstrip("\n"), line.strip()


def ident(name):
    return re.sub(r"[^a-z0-9]+", "_", name.lower()).strip("_")


# --------------------------------------------------------------------------
class Data:
    def __init__(self, root):
        self.root = root
        self.tiles = []          # list of dicts, index is the tile id
        self.tile_by_char = {}
        self.moves, self.move_ix = [], {}
        self.species, self.species_ix = [], {}
        self.items, self.item_ix = [], {}
        self.zones, self.zone_ix = [], {}
        self.sheets, self.sheet_ix = [], {}
        self.flag_set, self.flag_read = {}, {}
        self.text = []           # the page pool
        self.text_sealed = False
        self.start = None

    # ---- shared helpers
    def sheet(self, name, where):
        if name not in self.sheet_ix:
            self.sheet_ix[name] = len(self.sheets)
            self.sheets.append(name)
        return self.sheet_ix[name]

    def add_text(self, pages, where):
        # Once the pool has been written out, an index handed back from here
        # points past the end of the array the game will actually carry. That
        # is not a theoretical hazard: it shipped once, silently, and only
        # showed up under a sanitiser.
        if self.text_sealed:
            fail(where, "text added after the pool was written out, so this "
                        "index would point past the end of k_text")
        for p in pages:
            if not wraps_within(p, DIALOGUE_COLS, DIALOGUE_LINES):
                fail(where, f"text does not fit {DIALOGUE_LINES} lines of "
                            f"{DIALOGUE_COLS}: {p!r}")
        first = len(self.text)
        self.text.extend(pages)
        return first, len(pages)

    def seal_text(self):
        self.text_sealed = True

    def flag(self, name, where, writing):
        book = self.flag_set if writing else self.flag_read
        book.setdefault(name, where)
        return name

    # ---- tileset
    def load_tileset(self):
        path = os.path.join(self.root, "tileset.txt")
        for n, _raw, line in read_records(path):
            where = f"{path}:{n}"
            parts = line.split()
            if len(parts) < 6:
                fail(where, "expected: <char> <name> <flags...> <r> <g> <b>")
            ch, name = parts[0], parts[1]
            if len(ch) != 1:
                fail(where, f"tile character must be one character, got {ch!r}")
            if ch in self.tile_by_char:
                fail(where, f"tile character {ch!r} defined twice")
            rgb = parts[-3:]
            flags = parts[2:-3]
            if not flags:
                fail(where, "a tile needs at least one flag")
            bits = 0
            for f in flags:
                if f not in TILE_FLAGS:
                    fail(where, f"unknown tile flag {f!r}, expected one of "
                                f"{', '.join(sorted(TILE_FLAGS))}")
                bits |= TILE_FLAGS[f]
            try:
                r, g, b = (int(v, 16) for v in rgb)
            except ValueError:
                fail(where, f"colour must be three hex bytes, got {rgb}")
            for v, chan in zip((r, g, b), "rgb"):
                if v % 0x11:
                    fail(where, f"{chan}={v:#04x} is not a multiple of 0x11, so "
                                "it would shift on a four bit panel")
            self.tile_by_char[ch] = len(self.tiles)
            self.tiles.append(dict(ch=ch, name=name, flags=bits, r=r, g=g, b=b))
        if not self.tiles:
            fail(path, "no tiles defined")

    # ---- a generic block reader
    def blocks(self, path, opener):
        cur, header, start_line = None, None, 0
        for n, _raw, line in read_records(path):
            where = f"{path}:{n}"
            if cur is None:
                parts = line.split()
                if parts[0] != opener:
                    fail(where, f"expected a {opener!r} block, got {parts[0]!r}")
                header, cur, start_line = parts[1:], [], n
                continue
            if line == "end":
                yield f"{path}:{start_line}", header, cur
                cur = None
                continue
            cur.append((where, line))
        if cur is not None:
            fail(f"{path}:{start_line}", "block is never closed with 'end'")

    # ---- moves
    def load_moves(self):
        path = os.path.join(self.root, "moves.txt")
        for where, header, body in self.blocks(path, "move"):
            if len(header) != 1:
                fail(where, "expected: move <id>")
            mid = header[0]
            if mid in self.move_ix:
                fail(where, f"move {mid!r} defined twice")
            m = dict(id=mid, name=None, type=None, power=None, pp=None,
                     effect="none")
            for w, line in body:
                k, _, v = line.partition(" ")
                v = v.strip()
                if k == "name":
                    if len(v) > MAX_MOVE_NAME:
                        fail(w, f"move name {v!r} is {len(v)} characters, the "
                                f"move list holds {MAX_MOVE_NAME}")
                    m["name"] = v
                elif k == "type":
                    if v not in TYPES:
                        fail(w, f"unknown type {v!r}")
                    m["type"] = v
                elif k == "power":
                    m["power"] = int(v)
                elif k == "pp":
                    m["pp"] = int(v)
                elif k == "effect":
                    if v not in MOVE_EFFECTS:
                        fail(w, f"unknown effect {v!r}")
                    m["effect"] = v
                else:
                    fail(w, f"unknown key {k!r} in a move block")
            for k in ("name", "type", "power", "pp"):
                if m[k] is None:
                    fail(where, f"move {mid!r} has no {k}")
            if m["power"] == 0 and m["effect"] == "none":
                fail(where, f"move {mid!r} has no power and no effect, so it "
                            "does nothing at all")
            self.move_ix[mid] = len(self.moves)
            self.moves.append(m)

    # ---- species
    def load_species(self):
        path = os.path.join(self.root, "species.txt")
        pending_evo = []
        for where, header, body in self.blocks(path, "species"):
            if len(header) != 1:
                fail(where, "expected: species <id>")
            sid = header[0]
            if sid in self.species_ix:
                fail(where, f"species {sid!r} defined twice")
            s = dict(id=sid, name=None, type=None, stats=None, catch=None,
                     xp=None, mesh=None, tint=(0xFF, 0xFF, 0xFF), scale=100,
                     learn=[], evolve=None)
            for w, line in body:
                k, _, v = line.partition(" ")
                v = v.strip()
                if k == "name":
                    if len(v) > MAX_SPECIES_NAME:
                        fail(w, f"species name {v!r} is {len(v)} characters, "
                                f"the HP plate holds {MAX_SPECIES_NAME}")
                    s["name"] = v
                elif k == "type":
                    if v not in TYPES:
                        fail(w, f"unknown type {v!r}")
                    s["type"] = v
                elif k == "stats":
                    nums = [int(x) for x in v.split()]
                    if len(nums) != 4:
                        fail(w, "stats takes four numbers: hp atk def spd")
                    s["stats"] = nums
                elif k == "catch":
                    s["catch"] = int(v)
                    if not 1 <= s["catch"] <= 255:
                        fail(w, "catch rate must be 1 to 255")
                elif k == "xp":
                    s["xp"] = int(v)
                elif k == "mesh":
                    s["mesh"] = v
                elif k == "tint":
                    vals = [int(x, 16) for x in v.split()]
                    if len(vals) != 3:
                        fail(w, "tint takes three hex bytes")
                    s["tint"] = tuple(vals)
                elif k == "scale":
                    s["scale"] = int(v)
                elif k == "learn":
                    lvl, _, mv = v.partition(" ")
                    s["learn"].append((int(lvl), mv.strip(), w))
                elif k == "evolve":
                    lvl, _, into = v.partition(" ")
                    s["evolve"] = (int(lvl), into.strip(), w)
                else:
                    fail(w, f"unknown key {k!r} in a species block")
            for k in ("name", "type", "stats", "catch", "xp", "mesh"):
                if s[k] is None:
                    fail(where, f"species {sid!r} has no {k}")
            if not s["learn"]:
                fail(where, f"species {sid!r} knows no moves")
            s["learn"].sort(key=lambda e: e[0])
            self.species_ix[sid] = len(self.species)
            self.species.append(s)
            if s["evolve"]:
                pending_evo.append((sid, s["evolve"]))

        for mv in [e[1] for s in self.species for e in s["learn"]]:
            pass
        for s in self.species:
            for lvl, mv, w in s["learn"]:
                if mv not in self.move_ix:
                    fail(w, f"species {s['id']!r} learns unknown move {mv!r}")
        for sid, (lvl, into, w) in pending_evo:
            if into not in self.species_ix:
                fail(w, f"species {sid!r} evolves into unknown species {into!r}")

    # ---- items
    def load_items(self):
        path = os.path.join(self.root, "items.txt")
        for where, header, body in self.blocks(path, "item"):
            if len(header) != 1:
                fail(where, "expected: item <id>")
            iid = header[0]
            if iid in self.item_ix:
                fail(where, f"item {iid!r} defined twice")
            it = dict(id=iid, name=None, pocket=None, effect=None, param=0,
                      desc=None, price=0)
            for w, line in body:
                k, _, v = line.partition(" ")
                v = v.strip()
                if k == "name":
                    if len(v) > MAX_ITEM_NAME:
                        fail(w, f"item name {v!r} is {len(v)} characters, the "
                                f"bag list holds {MAX_ITEM_NAME}")
                    it["name"] = v
                elif k == "pocket":
                    if v not in POCKETS:
                        fail(w, f"unknown pocket {v!r}")
                    it["pocket"] = v
                elif k == "effect":
                    kind, _, param = v.partition(" ")
                    if kind not in ITEM_EFFECTS:
                        fail(w, f"unknown item effect {kind!r}")
                    it["effect"] = kind
                    it["param"] = int(param) if param.strip() else 0
                elif k == "desc":
                    if len(v) > MAX_ITEM_DESC:
                        fail(w, f"item description is {len(v)} characters, the "
                                f"panel holds {MAX_ITEM_DESC}")
                    it["desc"] = v
                elif k == "price":
                    price = int(v)
                    # The wallet is a uint16_t and the shop row prints at most
                    # five digits, so both ends are the same number.
                    if not 0 <= price <= MAX_MONEY:
                        fail(w, f"price {price} is outside 0..{MAX_MONEY}")
                    it["price"] = price
                else:
                    fail(w, f"unknown key {k!r} in an item block")
            for k in ("name", "pocket", "effect", "desc"):
                if it[k] is None:
                    fail(where, f"item {iid!r} has no {k}")
            self.item_ix[iid] = len(self.items)
            self.items.append(it)

    # ---- start
    def load_start(self):
        path = os.path.join(self.root, "start.txt")
        got = list(self.blocks(path, "start"))
        if len(got) != 1:
            fail(path, "expected exactly one start block")
        where, header, body = got[0]
        st = dict(zone=None, x=0, y=0, facing="south", party=[], bag=[],
                  money=0, whiteout=[])
        for w, line in body:
            k, _, v = line.partition(" ")
            v = v.strip()
            if k == "zone":
                st["zone"] = v
            elif k == "at":
                parts = v.split()
                if len(parts) != 3 or parts[2] not in FACINGS:
                    fail(w, "expected: at <x> <y> <facing>")
                st["x"], st["y"], st["facing"] = int(parts[0]), int(parts[1]), parts[2]
            elif k == "party":
                sp, _, lvl = v.partition(" ")
                if sp not in self.species_ix:
                    fail(w, f"unknown species {sp!r}")
                st["party"].append((sp, int(lvl)))
            elif k == "bag":
                it, _, cnt = v.partition(" ")
                if it not in self.item_ix:
                    fail(w, f"unknown item {it!r}")
                st["bag"].append((it, int(cnt)))
            elif k == "money":
                st["money"] = int(v)
            elif k == "whiteout":
                st["whiteout"].append(v)
            else:
                fail(w, f"unknown key {k!r} in the start block")
        if not st["zone"]:
            fail(where, "the start block has no zone")
        if not st["party"]:
            fail(where, "the player starts with nothing to fight with")
        if not st["whiteout"]:
            fail(where, "the start block has no whiteout line, so losing every "
                        "creature would teleport the player with no explanation")
        self.start = (where, st)

    # ---- zones
    def load_zones(self):
        zdir = os.path.join(self.root, "zones")
        for fname in sorted(os.listdir(zdir)):
            if not fname.endswith(".zone"):
                continue
            self.load_zone(os.path.join(zdir, fname))

    def load_zone(self, path):
        expect = os.path.splitext(os.path.basename(path))[0]
        z = dict(id=None, name=None, w=0, h=0, tiles=[], enc=[], npcs=[],
                 warps=[], events=[], indoor=False, trees="pine", path=path)
        lines = list(read_records(path))
        i = 0
        while i < len(lines):
            n, _raw, line = lines[i]
            where = f"{path}:{n}"
            key, _, rest = line.partition(" ")
            rest = rest.strip()

            if key == "zone":
                if rest != expect:
                    fail(where, f"zone id {rest!r} does not match the filename "
                                f"{expect!r}")
                z["id"] = rest
                i += 1
            elif key == "name":
                if len(rest) > MAX_ZONE_NAME:
                    fail(where, f"zone name is {len(rest)} characters, the "
                                f"banner holds {MAX_ZONE_NAME}")
                z["name"] = rest
                i += 1
            elif key == "size":
                parts = rest.split()
                if len(parts) != 2:
                    fail(where, "expected: size <w> <h>")
                z["w"], z["h"] = int(parts[0]), int(parts[1])
                if not (1 <= z["w"] <= 255 and 1 <= z["h"] <= 255):
                    fail(where, "a zone is at most 255 by 255 tiles")
                i += 1
            elif key == "indoor":
                if rest:
                    fail(where, "indoor takes no argument")
                z["indoor"] = True
                i += 1
            elif key == "trees":
                if rest not in TREE_KINDS:
                    fail(where, f"unknown tree {rest!r}, expected one of "
                                f"{', '.join(TREE_KINDS)}")
                z["trees"] = rest
                i += 1
            elif key == "tiles":
                i += 1
                rows = []
                while i < len(lines) and lines[i][2] != "end":
                    rows.append((lines[i][0], lines[i][2]))
                    i += 1
                if i >= len(lines):
                    fail(where, "the tiles block is never closed with 'end'")
                i += 1
                if len(rows) != z["h"]:
                    fail(where, f"tiles has {len(rows)} rows, size says {z['h']}")
                for rn, row in rows:
                    if len(row) != z["w"]:
                        fail(f"{path}:{rn}", f"row is {len(row)} characters, "
                                             f"size says {z['w']}")
                    for ch in row:
                        if ch not in self.tile_by_char:
                            fail(f"{path}:{rn}",
                                 f"tile character {ch!r} is not in tileset.txt")
                    z["tiles"].append([self.tile_by_char[c] for c in row])
            elif key == "warp":
                parts = rest.split()
                if len(parts) not in (5, 6):
                    fail(where, "expected: warp <x> <y> <zone> <dx> <dy> [facing]")
                facing = parts[5] if len(parts) == 6 else "south"
                if facing not in FACINGS:
                    fail(where, f"unknown facing {facing!r}")
                z["warps"].append(dict(
                    x=int(parts[0]), y=int(parts[1]), dest=parts[2],
                    dx=int(parts[3]), dy=int(parts[4]), facing=facing,
                    where=where))
                i += 1
            elif key in ("encounter", "npc", "event"):
                header = rest.split()
                body, i = [], i + 1
                while i < len(lines) and lines[i][2] != "end":
                    body.append((f"{path}:{lines[i][0]}", lines[i][2]))
                    i += 1
                if i >= len(lines):
                    fail(where, f"the {key} block is never closed with 'end'")
                i += 1
                getattr(self, "_zone_" + key)(z, where, header, body)
            else:
                fail(where, f"unknown key {key!r} at the top level of a zone")

        for k in ("id", "name"):
            if not z[k]:
                fail(path, f"zone has no {k}")
        if len(z["tiles"]) != z["h"]:
            fail(path, "zone has no tiles block")
        self.zone_ix[z["id"]] = len(self.zones)
        self.zones.append(z)

    def _zone_encounter(self, z, where, header, body):
        if len(header) != 1 or len(header[0]) != 1:
            fail(where, "expected: encounter <tile character>")
        ch = header[0]
        if ch not in self.tile_by_char:
            fail(where, f"tile character {ch!r} is not in tileset.txt")
        tile = self.tiles[self.tile_by_char[ch]]
        if not tile["flags"] & TILE_FLAGS["encounter"]:
            fail(where, f"tile {ch!r} ({tile['name']}) has no encounter flag, "
                        "so this table would never be rolled")
        rate, slots = None, []
        for w, line in body:
            parts = line.split()
            if parts[0] == "rate":
                rate = int(parts[1])
                if not 1 <= rate <= 100:
                    fail(w, "rate is a percentage, 1 to 100")
                continue
            if len(parts) != 4:
                fail(w, "expected: <species> <min level> <max level> <weight>")
            sp, lo, hi, weight = parts[0], int(parts[1]), int(parts[2]), int(parts[3])
            if sp not in self.species_ix:
                fail(w, f"unknown species {sp!r}")
            if lo < 1 or hi < lo or hi > 100:
                fail(w, f"level range {lo}-{hi} makes no sense")
            if weight <= 0:
                fail(w, "weight must be positive")
            slots.append(dict(species=sp, lo=lo, hi=hi, weight=weight))
        if rate is None:
            fail(where, "an encounter table needs a rate")
        if not slots:
            fail(where, "an encounter table with no species never spawns")
        z["enc"].append(dict(tile=ch, rate=rate, slots=slots, where=where))

    def _zone_npc(self, z, where, header, body):
        if len(header) != 5:
            fail(where, "expected: npc <id> <x> <y> <facing> <sheet>")
        nid, x, y, facing, sheet = header
        if facing not in FACINGS:
            fail(where, f"unknown facing {facing!r}")
        npc = dict(id=nid, x=int(x), y=int(y), facing=facing,
                   sheet=self.sheet(sheet, where), kind="villager", sight=0,
                   party=[], say=[], win=[], lose=[], reward=0, flag=None,
                   cond=None, cond_hide=False, stock=[], where=where)
        for w, line in body:
            k, _, v = line.partition(" ")
            v = v.strip()
            if k == "say":
                npc["say"].append(v)
            elif k == "win":
                npc["win"].append(v)
            elif k == "lose":
                npc["lose"].append(v)
            elif k == "kind":
                if v not in NPC_KINDS:
                    fail(w, f"unknown npc kind {v!r}")
                npc["kind"] = v
            elif k == "sight":
                npc["sight"] = int(v)
                npc["kind"] = "trainer"
            elif k == "party":
                for entry in v.split(","):
                    sp, _, lvl = entry.strip().partition(" ")
                    if sp not in self.species_ix:
                        fail(w, f"unknown species {sp!r}")
                    npc["party"].append((sp, int(lvl)))
                if len(npc["party"]) > MAX_PARTY:
                    fail(w, f"a party holds {MAX_PARTY}")
            elif k == "stock":
                for entry in v.split(","):
                    iid = entry.strip()
                    if iid not in self.item_ix:
                        fail(w, f"unknown item {iid!r}")
                    npc["stock"].append(iid)
                npc["kind"] = "shop"
            elif k == "reward":
                npc["reward"] = int(v)
            elif k == "flag":
                npc["flag"] = self.flag(v, w, writing=True)
            elif k == "onlyif":
                npc["cond"], npc["cond_hide"] = self.flag(v, w, False), False
            elif k == "hideif":
                npc["cond"], npc["cond_hide"] = self.flag(v, w, False), True
            else:
                fail(w, f"unknown key {k!r} in an npc block")
        if npc["kind"] == "trainer" and not npc["party"]:
            fail(where, f"trainer {nid!r} has nothing to send out")
        if npc["kind"] == "trainer" and not npc["flag"]:
            fail(where, f"trainer {nid!r} has no flag, so it would challenge "
                        "the player again every time they walked past")
        if npc["kind"] != "trainer" and npc["party"]:
            fail(where, f"{nid!r} has a party but is not a trainer")
        if npc["kind"] == "shop" and not npc["stock"]:
            fail(where, f"shop {nid!r} sells nothing, so its counter would "
                        "open on an empty list")
        if npc["kind"] != "shop" and npc["stock"]:
            fail(where, f"{nid!r} has stock but is not a shop")
        if len(npc["stock"]) > MAX_STOCK:
            fail(where, f"shop {nid!r} stocks {len(npc['stock'])} items, the "
                        f"list shows {MAX_STOCK} without scrolling")
        if len(set(npc["stock"])) != len(npc["stock"]):
            fail(where, f"shop {nid!r} stocks the same item twice")
        for iid in npc["stock"]:
            # A price of zero is how an item says it is not for sale, so a
            # shop stocking one would offer it free and take nothing.
            if self.items[self.item_ix[iid]]["price"] == 0:
                fail(where, f"shop {nid!r} stocks {iid!r}, which has no price")
        if not npc["say"]:
            fail(where, f"npc {nid!r} has nothing to say")
        z["npcs"].append(npc)

    def _zone_event(self, z, where, header, body):
        if len(header) < 3:
            fail(where, "expected: event <kind> <x> <y> [args]")
        kind, x, y = header[0], int(header[1]), int(header[2])
        if kind not in EVENT_KINDS:
            fail(where, f"unknown event kind {kind!r}")
        ev = dict(kind=kind, x=x, y=y, a0=0, a1=0, say=[], flag=None,
                  where=where)
        if kind == "item":
            if len(header) != 5:
                fail(where, "expected: event item <x> <y> <item> <count>")
            if header[3] not in self.item_ix:
                fail(where, f"unknown item {header[3]!r}")
            ev["a0"], ev["a1"] = self.item_ix[header[3]], int(header[4])
        elif len(header) != 3:
            fail(where, f"event {kind} takes no extra arguments")
        for w, line in body:
            k, _, v = line.partition(" ")
            v = v.strip()
            if k == "say":
                ev["say"].append(v)
            elif k == "flag":
                ev["flag"] = self.flag(v, w, writing=True)
            else:
                fail(w, f"unknown key {k!r} in an event block")
        if kind == "sign" and not ev["say"]:
            fail(where, "a sign with nothing on it is a blank sign")
        if kind == "item" and not ev["flag"]:
            fail(where, "an item event needs a flag, or the player can pick it "
                        "up again every time they walk back")
        z["events"].append(ev)

    # ---- whole dataset checks
    def check(self):
        walkable = lambda t: bool(self.tiles[t]["flags"] & TILE_FLAGS["walk"])
        for z in self.zones:
            seen_warp = set()
            for wp in z["warps"]:
                if wp["dest"] not in self.zone_ix:
                    fail(wp["where"], f"warps to unknown zone {wp['dest']!r}")
                if not (0 <= wp["x"] < z["w"] and 0 <= wp["y"] < z["h"]):
                    fail(wp["where"], "warp is outside its own zone")
                if not walkable(z["tiles"][wp["y"]][wp["x"]]):
                    fail(wp["where"], "the warp tile is not walkable, so the "
                                      "player can never step on it")
                dest = self.zones[self.zone_ix[wp["dest"]]]
                if not (0 <= wp["dx"] < dest["w"] and 0 <= wp["dy"] < dest["h"]):
                    fail(wp["where"], "warp lands outside the destination zone")
                if not walkable(dest["tiles"][wp["dy"]][wp["dx"]]):
                    fail(wp["where"], "warp lands on a tile that is not "
                                      "walkable, which strands the player")
                seen_warp.add((wp["x"], wp["y"]))
            for y in range(z["h"]):
                for x in range(z["w"]):
                    t = self.tiles[z["tiles"][y][x]]
                    if t["flags"] & TILE_FLAGS["door"] and (x, y) not in seen_warp:
                        fail(f"{z['path']}", f"door tile at {x},{y} has no warp "
                                             "on it, so it opens onto nothing")
            for thing, name in ((z["npcs"], "npc"), (z["events"], "event")):
                for e in thing:
                    if not (0 <= e["x"] < z["w"] and 0 <= e["y"] < z["h"]):
                        fail(e["where"], f"{name} is outside the zone")

            # An NPC standing in a wall is drawn, blocks nothing the wall was
            # not already blocking, and cannot be talked to. It reads as an
            # NPC that does not work, which is a long way from the one
            # character in the map that actually moved.
            occupied = {}
            for n in z["npcs"]:
                at = (n["x"], n["y"])
                if not walkable(z["tiles"][n["y"]][n["x"]]):
                    fail(n["where"], f"npc {n['id']!r} stands on a tile that "
                                     "is not walkable, so nothing can reach it")
                if at in seen_warp:
                    fail(n["where"], f"npc {n['id']!r} stands on a warp tile, "
                                     "and an npc blocks the tile it is on")
                if at in occupied:
                    fail(n["where"], f"npc {n['id']!r} stands on top of "
                                     f"{occupied[at]!r}")
                occupied[at] = n["id"]

            # A trainer whose sight line starts in a wall never sees anything,
            # which looks exactly like a trainer who is simply hard to trip.
            for n in z["npcs"]:
                if n["kind"] != "trainer" or n["sight"] == 0:
                    continue
                dx, dy = [(0, -1), (1, 0), (0, 1), (-1, 0)][FACINGS.index(n["facing"])]
                fx, fy = n["x"] + dx, n["y"] + dy
                if not (0 <= fx < z["w"] and 0 <= fy < z["h"]) or \
                        not walkable(z["tiles"][fy][fx]):
                    fail(n["where"], f"trainer {n['id']!r} looks straight into "
                                     "a wall, so its sight line never fires")

            # A sign nobody can stand next to is a sign nobody can read.
            for e in z["events"]:
                if e["kind"] in ("item", "trigger"):
                    if not walkable(z["tiles"][e["y"]][e["x"]]):
                        fail(e["where"], f"an {e['kind']} sits on a tile that "
                                         "is not walkable")
                    continue
                near = [(e["x"] + dx, e["y"] + dy)
                        for dx, dy in ((0, -1), (1, 0), (0, 1), (-1, 0))]
                if not any(0 <= x < z["w"] and 0 <= y < z["h"] and
                           walkable(z["tiles"][y][x]) and (x, y) not in occupied
                           for x, y in near):
                    fail(e["where"], "nothing can stand next to this sign, so "
                                     "it can never be read")
            # A room the camera can see out of is a room with a hole in it.
            # The window the renderer draws runs ten tiles past the map on
            # every side and reads the edge tile out there, so an indoor zone
            # whose border is walkable shows its floor colour marching off
            # into a black void.
            if z["indoor"]:
                edge = [(x, 0) for x in range(z["w"])] + \
                       [(x, z["h"] - 1) for x in range(z["w"])] + \
                       [(0, y) for y in range(z["h"])] + \
                       [(z["w"] - 1, y) for y in range(z["h"])]
                for x, y in edge:
                    t = z["tiles"][y][x]
                    if walkable(t) and \
                            not (self.tiles[t]["flags"] & TILE_FLAGS["door"]):
                        fail(z["path"], f"indoor zone has a walkable edge tile "
                                        f"at {x},{y}, so the room is open to "
                                        "the void outside it")
            if z["indoor"] and z["enc"]:
                fail(z["path"], "an indoor zone has an encounter table, and "
                                "nothing wanders into a room")

            for e in z["enc"]:
                ch = e["tile"]
                if not any(self.tiles[t]["ch"] == ch
                           for row in z["tiles"] for t in row):
                    fail(e["where"], f"no {ch!r} tile in this zone, so this "
                                     "table can never be rolled")

        unset = {n: w for n, w in self.flag_read.items() if n not in self.flag_set}
        for n, w in sorted(unset.items()):
            fail(w, f"flag {n!r} is read but nothing ever sets it, so whatever "
                    "waits on it is switched off forever")
        if len(self.flag_set) > MAX_FLAGS:
            fail("flags", f"{len(self.flag_set)} flags, the save block holds "
                          f"{MAX_FLAGS}")

        _, st = self.start
        if st["zone"] not in self.zone_ix:
            fail(self.start[0], f"start zone {st['zone']!r} does not exist")
        z = self.zones[self.zone_ix[st["zone"]]]
        if not (0 <= st["x"] < z["w"] and 0 <= st["y"] < z["h"]) or \
                not walkable(z["tiles"][st["y"]][st["x"]]):
            fail(self.start[0], "the player starts on a tile they cannot stand on")


# --------------------------------------------------------------------------
def emit(d, hpp_path, cpp_path):
    flags = sorted(d.flag_set)
    flag_ix = {n: i for i, n in enumerate(flags)}
    meshes = sorted({s["mesh"] for s in d.species})
    mesh_ix = {m: i for i, m in enumerate(meshes)}

    h = []
    w = h.append
    w("// Generated by tools/picomon_data.py. Do not edit, and do not commit.")
    w("#pragma once")
    w("")
    w("#include <cstdint>")
    w("")
    w("namespace pm {")
    w("")
    w("enum class Type : uint8_t { " + ", ".join(t.capitalize() for t in TYPES)
      + ", Count };")
    w("enum class MoveEffect : uint8_t { "
      + ", ".join("".join(p.capitalize() for p in e.split("_")) for e in MOVE_EFFECTS)
      + " };")
    w("enum class Pocket : uint8_t { " + ", ".join(p.capitalize() for p in POCKETS)
      + ", Count };")
    w("enum class ItemEffect : uint8_t { "
      + ", ".join(e.capitalize() for e in ITEM_EFFECTS) + " };")
    w("enum class TreeKind : uint8_t { "
      + ", ".join(k.capitalize() for k in TREE_KINDS) + ", Count };")
    w("enum class NpcKind : uint8_t { "
      + ", ".join(k.capitalize() for k in NPC_KINDS) + " };")
    w("enum class EventKind : uint8_t { "
      + ", ".join(k.capitalize() for k in EVENT_KINDS) + " };")
    w("")
    w("// Tile flags. The four ledge directions share the top three bits, so a")
    w("// tile stays one byte: 0 in that field means the tile is not a ledge.")
    for name, bit in sorted(TILE_FLAGS.items(), key=lambda kv: kv[1]):
        w(f"constexpr uint8_t k_tile_{name} = 0x{bit:02X};")
    w(f"constexpr uint8_t k_tile_ledge_mask = 0x{LEDGE_MASK:02X};")
    w("")
    w("constexpr uint8_t k_no_flag = 0xFF;")
    w("constexpr uint8_t k_cond_hide = 0x80;   // set in NpcDef::cond")
    w("")
    for struct in [
        ("TileDef", ["uint8_t flags", "uint8_t r", "uint8_t g", "uint8_t b"]),
        ("Move", ["const char* name", "uint8_t type", "uint8_t power",
                  "uint8_t pp", "uint8_t effect"]),
        ("LearnEntry", ["uint8_t level", "uint8_t move"]),
        ("Species", ["const char* name", "uint8_t type", "uint8_t hp",
                     "uint8_t atk", "uint8_t def", "uint8_t spd",
                     "uint8_t catch_rate", "uint8_t xp_yield", "uint8_t mesh",
                     "uint8_t tint_r", "uint8_t tint_g", "uint8_t tint_b",
                     "uint8_t scale", "uint8_t evolve_level",
                     "uint8_t evolve_into", "uint8_t learn_first",
                     "uint8_t learn_count"]),
        ("Item", ["const char* name", "const char* desc", "uint8_t pocket",
                  "uint8_t effect", "uint8_t param", "uint16_t price"]),
        ("EncSlot", ["uint8_t species", "uint8_t min_level",
                     "uint8_t max_level", "uint8_t cumulative"]),
        ("EncTable", ["uint8_t tile", "uint8_t rate", "uint8_t first",
                      "uint8_t count"]),
        ("PartyEntry", ["uint8_t species", "uint8_t level"]),
        ("NpcDef", ["uint8_t x", "uint8_t y", "uint8_t facing", "uint8_t sheet",
                    "uint8_t kind", "uint8_t sight", "uint8_t flag",
                    "uint8_t cond", "uint16_t say_first", "uint8_t say_count",
                    "uint16_t win_first", "uint8_t win_count",
                    "uint16_t lose_first", "uint8_t lose_count",
                    "uint16_t party_first", "uint8_t party_count",
                    "uint16_t reward", "uint8_t stock_first",
                    "uint8_t stock_count"]),
        ("WarpDef", ["uint8_t x", "uint8_t y", "uint8_t dest", "uint8_t dx",
                     "uint8_t dy", "uint8_t facing"]),
        ("EventDef", ["uint8_t x", "uint8_t y", "uint8_t kind", "uint8_t arg0",
                      "uint8_t arg1", "uint8_t flag", "uint16_t say_first",
                      "uint8_t say_count"]),
    ]:
        name, fields = struct
        w(f"struct {name} {{")
        for f in fields:
            w(f"    {f};")
        w("};")
        w("")
    w("struct Zone {")
    w("    const char* name;")
    w("    const uint8_t* tiles;      // w * h tile indices into k_tiles")
    w("    uint8_t w, h;")
    w("    uint8_t indoor;            // no sky, and the border is wall")
    w("    uint8_t trees;             // which TreeKind grows here")
    w("    const EncTable* enc;   uint8_t enc_count;")
    w("    const EncSlot* enc_slots;")
    w("    const NpcDef* npcs;    uint8_t npc_count;")
    w("    const WarpDef* warps;  uint8_t warp_count;")
    w("    const EventDef* events; uint8_t event_count;")
    w("};")
    w("")
    w("enum Flag : uint8_t {")
    for n in flags:
        w(f"    flag_{ident(n)},")
    w("};")
    w(f"constexpr int k_flag_count = {len(flags)};")
    w("")
    w("enum Mesh : uint8_t {")
    for m in meshes:
        w(f"    mesh_{ident(m)},")
    w("};")
    w(f"constexpr int k_mesh_count = {len(meshes)};")
    w("")
    w("enum Sheet : uint8_t {")
    for s in d.sheets:
        w(f"    sheet_{ident(s)},")
    w("};")
    w(f"constexpr int k_sheet_count = {len(d.sheets)};")
    w("")
    for zid in [z["id"] for z in d.zones]:
        w(f"constexpr uint8_t zone_{ident(zid)} = {d.zone_ix[zid]};")
    w("")
    # Named tile constants, so the renderer asks for "the tree tile" rather
    # than for index 7. Reordering tileset.txt then moves the number and
    # nothing breaks, which is the whole reason not to write the number down.
    for t in d.tiles:
        w(f"constexpr uint8_t tile_{ident(t['name'])} = {d.tile_by_char[t['ch']]};")
    w("")
    for iid in [i["id"] for i in d.items]:
        w(f"constexpr uint8_t item_{ident(iid)} = {d.item_ix[iid]};")
    w("")
    counts = [("tile", d.tiles), ("move", d.moves), ("species", d.species),
              ("item", d.items), ("zone", d.zones), ("text", d.text)]
    for name, seq in counts:
        w(f"constexpr int k_{name}_count = {len(seq)};")
    w("")
    w("extern const TileDef k_tiles[];")
    w("extern const Move k_moves[];")
    w("extern const LearnEntry k_learn[];")
    w("extern const Species k_species[];")
    w("extern const Item k_items[];")
    w("extern const Zone k_zones[];")
    w("extern const PartyEntry k_parties[];")
    w("extern const uint8_t k_stock[];   // every shop's stock, by item index")
    w("extern const char* const k_text[];")
    w("")
    w("// Where a new game begins, from data/start.txt.")
    w("struct StartState {")
    w("    uint8_t zone, x, y, facing;")
    w("    uint16_t money;")
    w("    uint8_t party_first, party_count;")
    w("    uint8_t bag_first, bag_count;")
    w("    uint16_t whiteout_first;   // what is said after a whiteout")
    w("    uint8_t whiteout_count;")
    w("};")
    w("struct BagEntry { uint8_t item, count; };")
    w("extern const StartState k_start;")
    w("extern const BagEntry k_start_bag[];")
    w("")
    w("}  // namespace pm")

    c = []
    w = c.append
    w("// Generated by tools/picomon_data.py. Do not edit, and do not commit.")
    w('#include "picomon_data.hpp"')
    w("")
    w("namespace pm {")
    w("")
    w("const TileDef k_tiles[] = {")
    for t in d.tiles:
        w(f"    {{0x{t['flags']:02X}, 0x{t['r']:02X}, 0x{t['g']:02X}, "
          f"0x{t['b']:02X}}},   // {t['ch']} {t['name']}")
    w("};")
    w("")
    w("const Move k_moves[] = {")
    for m in d.moves:
        w(f'    {{"{m["name"]}", {TYPES.index(m["type"])}, {m["power"]}, '
          f'{m["pp"]}, {MOVE_EFFECTS.index(m["effect"])}}},   // {m["id"]}')
    w("};")
    w("")
    learn_flat, learn_span = [], {}
    for s in d.species:
        learn_span[s["id"]] = (len(learn_flat), len(s["learn"]))
        for lvl, mv, _ in s["learn"]:
            learn_flat.append((lvl, d.move_ix[mv]))
    w("const LearnEntry k_learn[] = {")
    for lvl, mv in learn_flat:
        w(f"    {{{lvl}, {mv}}},")
    w("};")
    w("")
    w("const Species k_species[] = {")
    for s in d.species:
        first, count = learn_span[s["id"]]
        evo_lv, evo_to = (s["evolve"][0], d.species_ix[s["evolve"][1]]) \
            if s["evolve"] else (0, 0)
        hp, atk, dfn, spd = s["stats"]
        w(f'    {{"{s["name"]}", {TYPES.index(s["type"])}, {hp}, {atk}, {dfn}, '
          f'{spd}, {s["catch"]}, {s["xp"]}, mesh_{ident(s["mesh"])}, '
          f'0x{s["tint"][0]:02X}, 0x{s["tint"][1]:02X}, 0x{s["tint"][2]:02X}, '
          f'{s["scale"]}, {evo_lv}, {evo_to}, {first}, {count}}},   // {s["id"]}')
    w("};")
    w("")
    w("const Item k_items[] = {")
    for it in d.items:
        w(f'    {{"{it["name"]}", "{it["desc"]}", {POCKETS.index(it["pocket"])}, '
          f'{ITEM_EFFECTS.index(it["effect"])}, {it["param"]}, {it["price"]}}},'
          f'   // {it["id"]}')
    w("};")
    w("")
    w("const char* const k_text[] = {")
    for t in d.text:
        w(f'    "{t}",')
    w("};")
    w("")
    # Every shop's stock in one array, the same way every trainer's party is:
    # a counter is one byte on an NpcDef and the list itself is shared.
    stock, stock_span = [], {}
    for z in d.zones:
        for npc in z["npcs"]:
            stock_span[id(npc)] = (len(stock), len(npc["stock"]))
            stock.extend(d.item_ix[iid] for iid in npc["stock"])
    w("const uint8_t k_stock[] = {")
    w("    " + ", ".join(str(i) for i in stock) + ("," if stock else ""))
    w("};")
    w("")

    parties, party_span = [], {}
    for z in d.zones:
        for npc in z["npcs"]:
            party_span[id(npc)] = (len(parties), len(npc["party"]))
            for sp, lvl in npc["party"]:
                parties.append((d.species_ix[sp], lvl))
    _, st = d.start
    start_party_first = len(parties)
    for sp, lvl in st["party"]:
        parties.append((d.species_ix[sp], lvl))
    w("const PartyEntry k_parties[] = {")
    for sp, lvl in parties:
        w(f"    {{{sp}, {lvl}}},")
    w("};")
    w("")
    for z in d.zones:
        zid = ident(z["id"])
        w(f"static const uint8_t k_tiles_{zid}[] = {{")
        for row in z["tiles"]:
            w("    " + " ".join(f"{t}," for t in row))
        w("};")
        if z["enc"]:
            slots = []
            tables = []
            for e in z["enc"]:
                total = sum(s["weight"] for s in e["slots"])
                acc = 0
                first = len(slots)
                for s in e["slots"]:
                    acc += s["weight"]
                    slots.append((d.species_ix[s["species"]], s["lo"], s["hi"],
                                  min(255, acc * 255 // total)))
                slots[-1] = slots[-1][:3] + (255,)
                tables.append((d.tile_by_char[e["tile"]], e["rate"], first,
                               len(e["slots"])))
            w(f"static const EncSlot k_encslots_{zid}[] = {{")
            for s in slots:
                w(f"    {{{s[0]}, {s[1]}, {s[2]}, {s[3]}}},")
            w("};")
            w(f"static const EncTable k_enc_{zid}[] = {{")
            for t in tables:
                w(f"    {{{t[0]}, {t[1]}, {t[2]}, {t[3]}}},")
            w("};")
        if z["npcs"]:
            w(f"static const NpcDef k_npcs_{zid}[] = {{")
            for n in z["npcs"]:
                say = d.add_text(n["say"], n["where"])
                win = d.add_text(n["win"], n["where"]) if n["win"] else (0, 0)
                lose = d.add_text(n["lose"], n["where"]) if n["lose"] else (0, 0)
                pf, pc = party_span[id(n)]
                flag = f"flag_{ident(n['flag'])}" if n["flag"] else "k_no_flag"
                if n["cond"]:
                    cond = f"flag_{ident(n['cond'])}"
                    if n["cond_hide"]:
                        cond += " | k_cond_hide"
                else:
                    cond = "k_no_flag"
                sf, sc = stock_span[id(n)]
                w(f"    {{{n['x']}, {n['y']}, {FACINGS.index(n['facing'])}, "
                  f"sheet_{ident(d.sheets[n['sheet']])}, "
                  f"(uint8_t)NpcKind::{n['kind'].capitalize()}, {n['sight']}, "
                  f"{flag}, (uint8_t)({cond}), "
                  f"{say[0]}, {say[1]}, {win[0]}, {win[1]}, "
                  f"{lose[0]}, {lose[1]}, {pf}, {pc}, {n['reward']}, "
                  f"{sf}, {sc}}},"
                  f"   // {n['id']}")
            w("};")
        if z["warps"]:
            w(f"static const WarpDef k_warps_{zid}[] = {{")
            for wp in z["warps"]:
                w(f"    {{{wp['x']}, {wp['y']}, zone_{ident(wp['dest'])}, "
                  f"{wp['dx']}, {wp['dy']}, {FACINGS.index(wp['facing'])}}},")
            w("};")
        if z["events"]:
            w(f"static const EventDef k_events_{zid}[] = {{")
            for e in z["events"]:
                say = d.add_text(e["say"], e["where"]) if e["say"] else (0, 0)
                flag = f"flag_{ident(e['flag'])}" if e["flag"] else "k_no_flag"
                w(f"    {{{e['x']}, {e['y']}, "
                  f"(uint8_t)EventKind::{e['kind'].capitalize()}, {e['a0']}, "
                  f"{e['a1']}, {flag}, {say[0]}, {say[1]}}},")
            w("};")
        w("")

    w("const Zone k_zones[] = {")
    for z in d.zones:
        zid = ident(z["id"])
        enc = (f"k_enc_{zid}, {len(z['enc'])}, k_encslots_{zid}"
               if z["enc"] else "nullptr, 0, nullptr")
        npcs = f"k_npcs_{zid}, {len(z['npcs'])}" if z["npcs"] else "nullptr, 0"
        warps = f"k_warps_{zid}, {len(z['warps'])}" if z["warps"] else "nullptr, 0"
        evs = f"k_events_{zid}, {len(z['events'])}" if z["events"] else "nullptr, 0"
        w(f'    {{"{z["name"]}", k_tiles_{zid}, {z["w"]}, {z["h"]}, '
          f"{1 if z['indoor'] else 0}, {TREE_KINDS.index(z['trees'])}, "
          f"{enc}, {npcs}, {warps}, {evs}}},"
          f"   // {z['id']}")
    w("};")
    w("")
    w("const BagEntry k_start_bag[] = {")
    for it, cnt in st["bag"]:
        w(f"    {{item_{ident(it)}, {cnt}}},")
    w("};")
    w("")
    whiteout = d.add_text(st["whiteout"], d.start[0])
    w(f'const StartState k_start = {{zone_{ident(st["zone"])}, {st["x"]}, '
      f'{st["y"]}, {FACINGS.index(st["facing"])}, {st["money"]}, '
      f'{start_party_first}, {len(st["party"])}, 0, {len(st["bag"])}, '
      f'{whiteout[0]}, {whiteout[1]}}};')
    w("")
    w("}  // namespace pm")

    # The text pool goes in last, once nothing else can add to it.
    #
    # It is emitted as an empty placeholder early, because C++ does not care
    # about the order of definitions at file scope and one pass is simpler
    # than two. This splice used to happen before k_start was written, and
    # k_start's whiteout line is added to the pool at that point: the index
    # went in as 53 and the array shipped with 53 entries, so the game read
    # one past the end of k_text the first time a player lost every creature.
    # It reached a device build green. Nothing but ASan saw it.
    #
    # So the seal is here, after the last thing that can call add_text, and
    # add_text refuses to run once it has happened.
    d.seal_text()
    ci = c.index("const char* const k_text[] = {")
    pool = ["const char* const k_text[] = {"]
    for t in d.text:
        pool.append(f'    "{t}",')
    pool.append("};")
    end = c.index("};", ci)
    c[ci:end + 1] = pool
    if len(pool) - 2 != len(d.text):
        raise DataError("the text pool and its count disagree")

    # k_text is emitted with the final pool, but the extern declaration in the
    # header carries the count, so it has to be patched after the walk too.
    hdr = "\n".join(h).replace("constexpr int k_text_count = 0;",
                               f"constexpr int k_text_count = {len(d.text)};")
    hdr = re.sub(r"constexpr int k_text_count = \d+;",
                 f"constexpr int k_text_count = {len(d.text)};", hdr)

    os.makedirs(os.path.dirname(hpp_path), exist_ok=True)
    with open(hpp_path, "w") as f:
        f.write(hdr + "\n")
    with open(cpp_path, "w") as f:
        f.write("\n".join(c) + "\n")

    tiles_bytes = sum(z["w"] * z["h"] for z in d.zones)
    trainers = sum(1 for z in d.zones for n in z["npcs"] if n["kind"] == "trainer")
    shops = sum(1 for z in d.zones for n in z["npcs"] if n["kind"] == "shop")
    print(f"picomon_data: {len(d.zones)} zones ({tiles_bytes} B of tiles), "
          f"{len(d.species)} species, {len(d.moves)} moves, "
          f"{len(d.items)} items, {trainers} trainers, {shops} shops, "
          f"{len(d.text)} text pages, {len(flags)}/{MAX_FLAGS} flags")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("data_dir")
    ap.add_argument("--out-hpp", required=True)
    ap.add_argument("--out-cpp", required=True)
    args = ap.parse_args()

    d = Data(args.data_dir)
    try:
        d.load_tileset()
        d.load_moves()
        d.load_species()
        d.load_items()
        d.load_start()
        d.load_zones()
        d.check()
        emit(d, args.out_hpp, args.out_cpp)
    except DataError as e:
        sys.stderr.write(f"picomon_data: {e}\n")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
