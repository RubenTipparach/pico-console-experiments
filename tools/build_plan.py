#!/usr/bin/env python3
"""Work out which games a run builds, from `build.yaml`.

Every enabled game is built on every run. There is no change detection: what
gets built is a decision written down in `build.yaml`, not something inferred
from content hashes, so a build is reproducible from the config alone and no
game is ever skipped for a reason nobody can see.

Held games are not built and are not touched. Their last published build stays
on the site, carried forward from the gh-pages state branch, and their manifest
entry keeps the commit and timestamp it was published with.

Commands:
    build_plan.py list                       print slug and state, one per line
    build_plan.py plan                       emit the GitHub Actions build matrix
    build_plan.py manifest --published P.json --built A,B  write the new builds.json
"""

import argparse
import json
import os
import sys

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GAMES_DIR = os.path.join(REPO_ROOT, "games")
CONFIG_PATH = os.path.join(REPO_ROOT, "build.yaml")


class GameError(Exception):
    """Raised when a game directory or the build config is malformed."""


def read_yaml(path):
    """Tiny YAML reader for the flat subset used by game.yml and build.yaml.

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
    """One entry under games/. Knows its own identity and how it builds."""

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

    def describe(self):
        return {
            "slug": self.slug,
            "dir": os.path.relpath(self.directory, REPO_ROOT).replace(os.sep, "/"),
            "title": self.title,
            "sdk": self.sdk,
            "web": self.web,
            "target": self.target,
        }


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


def load_config(known_slugs):
    """Read build.yaml into (build set, hold set, unlisted list).

    A slug in neither list counts as buildable, so adding a game does not mean
    editing this file first. A slug in either list that no longer exists is an
    error: a typo under `hold` would otherwise read as "unlisted", which means
    the exact opposite of what it says.
    """
    if not os.path.isfile(CONFIG_PATH):
        raise GameError("missing build.yaml at the repo root")
    data = read_yaml(CONFIG_PATH)
    build = _slug_list(data.get("build"), "build")
    hold = _slug_list(data.get("hold"), "hold")

    unknown = sorted((build | hold) - set(known_slugs))
    if unknown:
        raise GameError("build.yaml names games that do not exist: %s"
                        % ", ".join(unknown))
    both = sorted(build & hold)
    if both:
        raise GameError("build.yaml lists these under build and hold: %s"
                        % ", ".join(both))

    unlisted = [slug for slug in known_slugs if slug not in build | hold]
    return build, hold, unlisted


def _slug_list(value, key):
    if value is None:
        return set()
    if not isinstance(value, list):
        raise GameError("build.yaml: `%s` must be a list of slugs" % key)
    return {str(item) for item in value}


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
    games = discover_games()
    _, hold, _ = load_config([game.slug for game in games])
    for game in games:
        sys.stdout.write("%s %s\n"
                         % (game.slug, "hold" if game.slug in hold else "build"))
    return 0


def cmd_plan(args):
    games = discover_games()
    by_slug = {game.slug: game for game in games}
    _, hold, unlisted = load_config(list(by_slug))

    # An explicit list from a manual run wins over the config, so one held
    # game can be rebuilt without editing build.yaml first.
    named = [s.strip() for s in (args.games or "").split(",") if s.strip()]
    unknown = sorted(set(named) - set(by_slug))
    if unknown:
        raise GameError("no such game: %s" % ", ".join(unknown))

    if named:
        selected = [by_slug[slug] for slug in named]
    else:
        selected = [game for game in games if game.slug not in hold]

    chosen = {game.slug for game in selected}
    include = [game.describe() for game in selected]
    held = [game.slug for game in games if game.slug not in chosen]

    matrix = json.dumps({"include": include}, separators=(",", ":"))
    emit_output("matrix", matrix)
    # An empty matrix vector fails the workflow rather than skipping the job,
    # so the build job needs a boolean to guard on.
    emit_output("has_work", "true" if include else "false")
    emit_output("built", ",".join(sorted(chosen)))
    emit_output("held", ",".join(held))

    report = _plan_report(include, held, unlisted, bool(named))
    summary = os.environ.get("GITHUB_STEP_SUMMARY")
    if summary:
        with open(summary, "a", encoding="utf-8") as handle:
            handle.write(report)
    sys.stderr.write(report)
    return 0


def _plan_report(include, held, unlisted, explicit):
    lines = ["## Build plan\n\n"]
    if explicit:
        lines.append("This run names its games explicitly, so `build.yaml` "
                     "does not apply.\n\n")
    if include:
        lines.append("Building %d game(s):\n\n" % len(include))
        for entry in include:
            lines.append("- `%s` (%s, %s)\n" % (
                entry["slug"], entry["sdk"],
                "web + device" if entry["web"] else "device only"))
    else:
        lines.append("Nothing to build. Every game is held in `build.yaml`.\n")
    if held:
        lines.append("\nHeld, published build kept as is: %s\n"
                     % ", ".join("`%s`" % slug for slug in held))
    if unlisted and not explicit:
        lines.append("\nNot mentioned in `build.yaml`, so built by default: "
                     "%s\n" % ", ".join("`%s`" % slug for slug in unlisted))
    return "".join(lines)


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


def cmd_manifest(args):
    games = discover_games()
    published = load_published(args.published)
    built = {s.strip() for s in (args.built or "").split(",") if s.strip()}

    entries = []
    for game in games:
        previous = published.get(game.slug)
        if game.slug not in built and previous is None:
            # Held and never published: no files stand behind it, so listing
            # it would advertise a page that 404s.
            continue
        entry = game.describe()
        entry["has_thumbnail"] = game.has_thumbnail
        if game.slug in built:
            entry["built_at"] = args.timestamp
            entry["commit"] = args.commit
        else:
            entry["built_at"] = previous.get("built_at", args.timestamp)
            entry["commit"] = previous.get("commit", args.commit)
        entries.append(entry)

    payload = {"schema": 2, "generated_at": args.timestamp, "games": entries}
    with open(args.out, "w", encoding="utf-8") as handle:
        json.dump(payload, handle, indent=2, sort_keys=True)
        handle.write("\n")
    return 0


def main(argv):
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)

    sub.add_parser("list").set_defaults(func=cmd_list)

    plan = sub.add_parser("plan")
    plan.add_argument("--games", default="",
                      help="comma separated slugs; overrides build.yaml")
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
        sys.stderr.write("build_plan: %s\n" % error)
        return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
