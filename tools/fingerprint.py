#!/usr/bin/env python3
"""Decide which games actually need rebuilding.

CI minutes are the scarce resource in this repo, so nothing is rebuilt unless
its inputs changed. A game's fingerprint is a hash over its own directory plus
everything it declares a dependency on. The published site carries the
fingerprint of every game it was built from, in `builds.json`. A game is stale
when those two disagree.

This is deliberately content based rather than git-diff based. It stays correct
across force pushes, re-runs, first runs, and reverts: reverting a change
restores the old fingerprint, so nothing rebuilds, which is the right answer.

Commands:
    fingerprint.py list                       print slug and fingerprint, one per line
    fingerprint.py plan --published P.json    emit the GitHub Actions build matrix
    fingerprint.py manifest --published P.json --built A,B  write the new builds.json
"""

import argparse
import hashlib
import json
import os
import sys

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GAMES_DIR = os.path.join(REPO_ROOT, "games")

# Inputs every game depends on unless its game.yml says otherwise.
DEFAULT_DEPENDS_ON = ["engine", "cmake", "tools/obj2cpp.py"]

# Files inside a game directory that must never trigger a rebuild. Thumbnails
# are published artefacts, not build inputs: rebuilding because a screenshot
# landed would make thumbnail capture self-perpetuating.
IGNORED_NAMES = {"thumbnail.png", "screenshot.png", ".DS_Store"}
IGNORED_DIRS = {"build", "__pycache__", ".git"}


class GameError(Exception):
    """Raised when a game directory is malformed."""


def read_yaml(path):
    """Tiny YAML reader for the flat subset used by game.yml.

    Supports `key: value`, `key:` followed by `  - item` lists, quoted strings,
    booleans, and integers. Pulling in PyYAML for this would mean a pip install
    on every CI runner, which is exactly the sort of cost this repo avoids.
    """
    data = {}
    key = None
    with open(path, "r", encoding="utf-8") as handle:
        for line_no, raw in enumerate(handle, start=1):
            line = raw.split("#", 1)[0].rstrip()
            if not line.strip():
                continue
            if line.lstrip().startswith("- "):
                if key is None:
                    raise GameError("%s:%d: list item outside a key"
                                    % (path, line_no))
                data.setdefault(key, [])
                if not isinstance(data[key], list):
                    data[key] = []
                data[key].append(_coerce(line.lstrip()[2:].strip()))
                continue
            if ":" not in line:
                raise GameError("%s:%d: expected 'key: value'" % (path, line_no))
            key, _, value = line.partition(":")
            key = key.strip()
            value = value.strip()
            data[key] = _coerce(value) if value else None
    return data


def _coerce(value):
    if len(value) >= 2 and value[0] == value[-1] and value[0] in "\"'":
        return value[1:-1]
    lowered = value.lower()
    if lowered in ("true", "yes"):
        return True
    if lowered in ("false", "no"):
        return False
    try:
        return int(value)
    except ValueError:
        return value


class Game:
    """One entry under games/. Knows its own identity and its build inputs."""

    def __init__(self, directory):
        self.directory = directory
        self.dirname = os.path.basename(directory)
        manifest_path = os.path.join(directory, "game.yml")
        if not os.path.isfile(manifest_path):
            raise GameError("%s: missing game.yml" % directory)
        self.manifest = read_yaml(manifest_path)

        self.slug = self.manifest.get("slug") or self.dirname
        self.title = self.manifest.get("title") or self.slug
        self.sdk = self.manifest.get("sdk") or "picosystem"
        self.web = bool(self.manifest.get("web", self.sdk == "32blit"))
        self.target = self.manifest.get("target") or self.slug
        self.depends_on = self.manifest.get("depends_on") or DEFAULT_DEPENDS_ON

        if self.sdk not in ("picosystem", "32blit"):
            raise GameError("%s: sdk must be 'picosystem' or '32blit', got %r"
                            % (manifest_path, self.sdk))
        if self.web and self.sdk != "32blit":
            raise GameError(
                "%s: web builds need the 32blit SDK. The picosystem SDK has no "
                "emscripten target." % manifest_path)

    @property
    def has_thumbnail(self):
        return os.path.isfile(os.path.join(self.directory, "thumbnail.png"))

    def fingerprint(self, epoch):
        digest = hashlib.sha256()
        digest.update(b"epoch\0")
        digest.update(epoch.encode("utf-8"))
        digest.update(b"\0")
        for path in self._input_paths():
            rel = os.path.relpath(path, REPO_ROOT).replace(os.sep, "/")
            digest.update(rel.encode("utf-8"))
            digest.update(b"\0")
            digest.update(_hash_file(path))
            digest.update(b"\0")
        return digest.hexdigest()[:16]

    def _input_paths(self):
        roots = [self.directory]
        roots.extend(os.path.join(REPO_ROOT, dep) for dep in self.depends_on)
        found = []
        for root in roots:
            if os.path.isfile(root):
                found.append(root)
            elif os.path.isdir(root):
                found.extend(_walk(root))
            else:
                raise GameError("%s: depends_on path does not exist: %s"
                                % (self.slug, root))
        return sorted(set(found))

    def describe(self):
        return {
            "slug": self.slug,
            "dir": os.path.relpath(self.directory, REPO_ROOT).replace(os.sep, "/"),
            "title": self.title,
            "sdk": self.sdk,
            "web": self.web,
            "target": self.target,
        }


def _walk(root):
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames[:] = [d for d in dirnames
                       if d not in IGNORED_DIRS and not d.startswith("build.")]
        for name in filenames:
            if name in IGNORED_NAMES:
                continue
            yield os.path.join(dirpath, name)


def _hash_file(path):
    digest = hashlib.sha256()
    with open(path, "rb") as handle:
        for block in iter(lambda: handle.read(65536), b""):
            digest.update(block)
    return digest.digest()


def build_epoch():
    """A manual cache buster. Bump `.build-epoch` to force a full rebuild when
    the toolchain or the workflow changes in a way content hashing cannot see."""
    path = os.path.join(REPO_ROOT, ".build-epoch")
    if not os.path.isfile(path):
        return "0"
    with open(path, "r", encoding="utf-8") as handle:
        return handle.read().strip() or "0"


def discover_games():
    if not os.path.isdir(GAMES_DIR):
        return []
    games = []
    for name in sorted(os.listdir(GAMES_DIR)):
        directory = os.path.join(GAMES_DIR, name)
        if not os.path.isdir(directory):
            continue
        if not os.path.isfile(os.path.join(directory, "game.yml")):
            continue
        games.append(Game(directory))
    slugs = [g.slug for g in games]
    duplicates = {s for s in slugs if slugs.count(s) > 1}
    if duplicates:
        raise GameError("duplicate slugs: %s" % ", ".join(sorted(duplicates)))
    return games


def load_published(path):
    if not path or not os.path.isfile(path):
        return {}
    try:
        with open(path, "r", encoding="utf-8") as handle:
            data = json.load(handle)
    except (ValueError, OSError):
        return {}
    return {entry["slug"]: entry for entry in data.get("games", [])
            if isinstance(entry, dict) and "slug" in entry}


def emit_output(name, value):
    """Write a GitHub Actions step output, or fall back to stdout locally."""
    target = os.environ.get("GITHUB_OUTPUT")
    line = "%s=%s" % (name, value)
    if target:
        with open(target, "a", encoding="utf-8") as handle:
            handle.write(line + "\n")
    else:
        sys.stdout.write(line + "\n")


def cmd_list(args):
    epoch = build_epoch()
    for game in discover_games():
        sys.stdout.write("%s %s\n" % (game.slug, game.fingerprint(epoch)))
    return 0


def cmd_plan(args):
    epoch = build_epoch()
    games = discover_games()
    published = load_published(args.published)
    force = {s.strip() for s in (args.force or "").split(",") if s.strip()}
    build_all = args.all or "all" in force

    include = []
    skipped = []
    for game in games:
        fingerprint = game.fingerprint(epoch)
        previous = published.get(game.slug)
        stale = (
            build_all
            or game.slug in force
            or previous is None
            or previous.get("fingerprint") != fingerprint
        )
        entry = game.describe()
        entry["fingerprint"] = fingerprint
        if stale:
            include.append(entry)
        else:
            skipped.append(entry)

    matrix = json.dumps({"include": include}, separators=(",", ":"))
    emit_output("matrix", matrix)
    emit_output("has_work", "true" if include else "false")
    emit_output("built", ",".join(e["slug"] for e in include))
    emit_output("skipped", ",".join(e["slug"] for e in skipped))

    summary = os.environ.get("GITHUB_STEP_SUMMARY")
    report = _plan_report(include, skipped)
    if summary:
        with open(summary, "a", encoding="utf-8") as handle:
            handle.write(report)
    sys.stderr.write(report)
    return 0


def _plan_report(include, skipped):
    lines = ["## Build plan\n\n"]
    if include:
        lines.append("Rebuilding %d game(s):\n\n" % len(include))
        for entry in include:
            lines.append("- `%s` (%s, %s)\n" % (
                entry["slug"], entry["sdk"],
                "web + device" if entry["web"] else "device only"))
    else:
        lines.append("Nothing to rebuild. Every game matches the published "
                     "fingerprint.\n")
    if skipped:
        lines.append("\nUnchanged, republished as is: %s\n"
                     % ", ".join("`%s`" % e["slug"] for e in skipped))
    return "".join(lines)


def cmd_manifest(args):
    epoch = build_epoch()
    games = discover_games()
    published = load_published(args.published)
    built = {s.strip() for s in (args.built or "").split(",") if s.strip()}

    entries = []
    for game in games:
        entry = game.describe()
        entry["fingerprint"] = game.fingerprint(epoch)
        entry["has_thumbnail"] = game.has_thumbnail
        previous = published.get(game.slug, {})
        if game.slug in built:
            entry["built_at"] = args.timestamp
            entry["commit"] = args.commit
        else:
            entry["built_at"] = previous.get("built_at", args.timestamp)
            entry["commit"] = previous.get("commit", args.commit)
        entries.append(entry)

    payload = {"schema": 1, "generated_at": args.timestamp, "games": entries}
    with open(args.out, "w", encoding="utf-8") as handle:
        json.dump(payload, handle, indent=2, sort_keys=True)
        handle.write("\n")
    return 0


def main(argv):
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)

    sub.add_parser("list").set_defaults(func=cmd_list)

    plan = sub.add_parser("plan")
    plan.add_argument("--published", help="path to the live site's builds.json")
    plan.add_argument("--force", default="", help="comma separated slugs, or 'all'")
    plan.add_argument("--all", action="store_true")
    plan.set_defaults(func=cmd_plan)

    manifest = sub.add_parser("manifest")
    manifest.add_argument("--published")
    manifest.add_argument("--built", default="")
    manifest.add_argument("--out", required=True)
    manifest.add_argument("--timestamp", default="")
    manifest.add_argument("--commit", default="")
    manifest.set_defaults(func=cmd_manifest)

    args = parser.parse_args(argv)
    try:
        return args.func(args)
    except GameError as error:
        sys.stderr.write("fingerprint: %s\n" % error)
        return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
