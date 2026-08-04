#!/usr/bin/env python3
"""Compose the launcher and every game into one flashable .uf2.

A UF2 block carries its own target address, so a bundle is nothing more than
the launcher's blocks plus each game's blocks with the block numbering redone.
No relinking, no patching: the games were already linked at their slots by
cmake/slot.cmake, so their blocks already point where they belong.

What this does add is the checking. A bundle that overlaps itself, or that puts
a game somewhere the launcher will not look, produces a console that boots into
the wrong thing, and that is not a failure anybody can debug from the outside.
So every image is checked against the slot map before it is written.

    make_bundle.py --launcher launcher.uf2 \\
        --game kingfisher=1:kingfisher.uf2 --game dustrider=2:dustrider.uf2 \\
        --out bundle.uf2
    make_bundle.py --list bundle.uf2
"""

import argparse
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import game_meta  # noqa: E402  (same directory)

# Kept in step with cmake/slot.cmake and launcher/src/library.hpp. All three
# have to agree or a game boots into whatever is next door.
FLASH_BASE = 0x10000000
SLOT_SIZE = 512 * 1024
SLOT_COUNT = 23

UF2_MAGIC_START0 = 0x0A324655
UF2_MAGIC_START1 = 0x9E5D5157
UF2_MAGIC_END = 0x0AB16F30
UF2_FLAG_FAMILY_ID = 0x00002000
RP2040_FAMILY_ID = 0xE48BFF56
PAYLOAD = 256


class BundleError(Exception):
    """Raised when a bundle would produce a console that does not boot."""


def slot_address(slot):
    return FLASH_BASE + slot * SLOT_SIZE


def read_blocks(path):
    """Return [(address, payload)] for a .uf2, in file order."""
    blocks = []
    with open(path, "rb") as handle:
        data = handle.read()
    if len(data) % 512 != 0 or not data:
        raise BundleError("%s: not a whole number of UF2 blocks" % path)
    for offset in range(0, len(data), 512):
        block = data[offset:offset + 512]
        start0, start1, _flags, address, size = struct.unpack("<IIIII", block[:20])
        if start0 != UF2_MAGIC_START0 or start1 != UF2_MAGIC_START1:
            raise BundleError("%s: block %d is not a UF2 block"
                              % (path, offset // 512))
        if struct.unpack("<I", block[-4:])[0] != UF2_MAGIC_END:
            raise BundleError("%s: block %d has no end magic"
                              % (path, offset // 512))
        if size > PAYLOAD:
            raise BundleError("%s: block %d claims %d payload bytes"
                              % (path, offset // 512, size))
        blocks.append((address, block[32:32 + size]))
    if not blocks:
        raise BundleError("%s: empty" % path)
    return blocks


def write_blocks(path, blocks):
    total = len(blocks)
    with open(path, "wb") as handle:
        for index, (address, payload) in enumerate(blocks):
            padded = payload + b"\0" * (PAYLOAD - len(payload))
            header = struct.pack("<IIIIIIII", UF2_MAGIC_START0,
                                 UF2_MAGIC_START1, UF2_FLAG_FAMILY_ID, address,
                                 PAYLOAD, index, total, RP2040_FAMILY_ID)
            handle.write(header + padded + b"\0" * 220
                         + struct.pack("<I", UF2_MAGIC_END))


def extent(blocks):
    low = min(address for address, _ in blocks)
    high = max(address + len(payload) for address, payload in blocks)
    return low, high


def check_launcher(blocks):
    low, high = extent(blocks)
    if low != FLASH_BASE:
        raise BundleError("the launcher starts at 0x%08X, not the base of "
                          "flash. It has to own the reset vector." % low)
    if high > slot_address(1):
        raise BundleError("the launcher runs to 0x%08X and would overwrite "
                          "slot 1 at 0x%08X" % (high, slot_address(1)))


def check_game(name, slot, blocks):
    if not 1 <= slot <= SLOT_COUNT:
        raise BundleError("%s: slot %d is outside 1..%d"
                          % (name, slot, SLOT_COUNT))
    low, high = extent(blocks)
    want = slot_address(slot)
    if low != want:
        raise BundleError(
            "%s was linked at 0x%08X but is being bundled into slot %d at "
            "0x%08X. Build it with -DPICO_SLOT=%d."
            % (name, low, slot, want, slot))
    if high > want + SLOT_SIZE:
        raise BundleError("%s needs %d bytes and a slot holds %d"
                          % (name, high - low, SLOT_SIZE))


def specs_from_plan(path, slot_dir):
    """Turn the detect job's matrix into --game specs.

    Taking the slots from the same plan the matrix was built from is what
    stops a game being linked for one slot and bundled into another: there is
    only one place the number comes from.
    """
    import json
    with open(path, "r", encoding="utf-8") as handle:
        plan = json.load(handle)
    specs = []
    for entry in plan.get("include", []):
        slug = entry["slug"]
        if "slot" not in entry:
            raise BundleError("%s has no slot in the build plan" % slug)
        image = os.path.join(slot_dir, slug + ".uf2")
        if not os.path.isfile(image):
            raise BundleError("%s has no slot build at %s" % (slug, image))
        specs.append("%s=%d:%s" % (slug, entry["slot"], image))
    return specs


def cmd_bundle(args):
    blocks = read_blocks(args.launcher)
    check_launcher(blocks)
    used = {}

    specs = list(args.game)
    if args.plan:
        specs.extend(specs_from_plan(args.plan, args.slot_dir))

    for spec in specs:
        # name=slot:path
        try:
            name, rest = spec.split("=", 1)
            slot_text, path = rest.split(":", 1)
            slot = int(slot_text)
        except ValueError:
            raise BundleError("bad --game %r, want name=slot:path" % spec)

        if slot in used:
            raise BundleError("slot %d is claimed by both %s and %s"
                              % (slot, used[slot], name))
        used[slot] = name

        game_blocks = read_blocks(path)
        check_game(name, slot, game_blocks)
        blocks.extend(game_blocks)

        # A game with no metadata block would be invisible in the menu: it
        # would occupy a slot the launcher scans and finds nothing in.
        _base, image = game_meta.uf2_image(path)
        try:
            info, _pixels = game_meta.unpack(game_meta.find_block(image))
        except game_meta.MetaError:
            raise BundleError("%s has no metadata block, so the launcher "
                              "would not list it" % name)
        sys.stderr.write("slot %2d  0x%08X  %-24s %s\n"
                         % (slot, slot_address(slot), info["title"],
                            info["version"]))

    write_blocks(args.out, blocks)
    sys.stderr.write("bundle: %d games, %d blocks, %d bytes\n"
                     % (len(used), len(blocks), len(blocks) * 512))
    return 0


def cmd_list(args):
    blocks = read_blocks(args.list)
    by_slot = {}
    for address, payload in blocks:
        slot = (address - FLASH_BASE) // SLOT_SIZE
        by_slot.setdefault(slot, []).append((address, payload))

    for slot in sorted(by_slot):
        low, high = extent(by_slot[slot])
        if slot == 0:
            sys.stdout.write("slot  0  launcher  %d bytes\n" % (high - low))
            continue
        image = bytearray(high - low)
        for address, payload in by_slot[slot]:
            image[address - low:address - low + len(payload)] = payload
        try:
            info, _ = game_meta.unpack(game_meta.find_block(bytes(image)))
            sys.stdout.write("slot %2d  %-24s %-10s %d bytes\n"
                             % (slot, info["title"], info["version"],
                                high - low))
        except game_meta.MetaError:
            sys.stdout.write("slot %2d  (no metadata)  %d bytes\n"
                             % (slot, high - low))
    return 0


def main(argv):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--launcher", help="the launcher's .uf2")
    parser.add_argument("--game", action="append", default=[],
                        metavar="NAME=SLOT:PATH")
    parser.add_argument("--plan", metavar="MATRIX.json",
                        help="a build plan matrix; slots are read from it")
    parser.add_argument("--slot-dir", default="incoming/slot",
                        help="where the plan's slot builds are")
    parser.add_argument("--out", help="bundle to write")
    parser.add_argument("--list", metavar="BUNDLE",
                        help="describe a bundle instead of making one")

    args = parser.parse_args(argv)
    try:
        if args.list:
            return cmd_list(args)
        if not args.launcher or not args.out:
            parser.error("--launcher and --out are required")
        return cmd_bundle(args)
    except (BundleError, game_meta.MetaError) as error:
        sys.stderr.write("make_bundle: %s\n" % error)
        return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
