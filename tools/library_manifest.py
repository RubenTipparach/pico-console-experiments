#!/usr/bin/env python3
"""Tracks what belongs in the flasher's local library.

The flasher used to decide what to show by scanning several build output
directories (build.pico, build.slot, build.launcher) and guessing a file's
role from its own bytes: where it linked, whether it carried a metadata
block. That guess is fundamentally ambiguous for anything not built by this
project (see PicoFlasher/GameLibrary.cs), and it also meant that files built
for reasons that have nothing to do with a bundle (a standalone single-game
build, made for local testing) showed up in the library uninvited.

This is the fix: one canonical folder, build.launcher/library/, and one JSON
file that says what is actually in it and why. build_bundle.bat writes an
entry here right after it builds something, when it already knows for a fact
which slot a game was linked for, so the flasher never has to guess again for
anything this project built. A file dropped into the library from outside
(the flasher's own Import/drag-drop) gets its own entry with role "imported",
and the flasher classifies those the old way, by reading the file itself,
because nothing else knows what they are.

Schema:
    {
      "entries": [
        {"file": "launcher.uf2", "role": "launcher"},
        {"file": "chicken.uf2",  "role": "slot", "slot": 1},
        {"file": "celeste.uf2",  "role": "imported"}
      ]
    }

Usage:
    library_manifest.py add --manifest build.launcher/library/manifest.json \\
        --file chicken.uf2 --role slot --slot 1
    library_manifest.py add --manifest ... --file launcher.uf2 --role launcher
"""

import argparse
import json
import os
import sys


def load(path):
    if not os.path.exists(path):
        return {"entries": []}
    with open(path, "r", encoding="utf-8") as handle:
        return json.load(handle)


def save(path, data):
    directory = os.path.dirname(os.path.abspath(path))
    if directory:
        os.makedirs(directory, exist_ok=True)
    with open(path, "w", encoding="utf-8") as handle:
        json.dump(data, handle, indent=2)
        handle.write("\n")


def cmd_add(args):
    if args.role == "slot" and args.slot is None:
        sys.stderr.write("library_manifest: --role slot needs --slot N\n")
        return 1

    data = load(args.manifest)
    entries = [e for e in data.get("entries", []) if e.get("file") != args.file]

    entry = {"file": args.file, "role": args.role}
    if args.slot is not None:
        entry["slot"] = args.slot
    entries.append(entry)

    data["entries"] = entries
    save(args.manifest, data)
    sys.stderr.write("library_manifest: %s -> %s\n" % (args.file, args.role))
    return 0


def parse_args(argv):
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="command", required=True)

    add = sub.add_parser("add", help="add or replace one entry")
    add.add_argument("--manifest", required=True)
    add.add_argument("--file", required=True, help="filename, not a full path")
    add.add_argument("--role", required=True, choices=["launcher", "slot", "imported"])
    add.add_argument("--slot", type=int, default=None)
    add.set_defaults(func=cmd_add)

    return parser.parse_args(argv)


def main(argv):
    args = parse_args(argv)
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
