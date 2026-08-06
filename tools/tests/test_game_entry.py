#!/usr/bin/env python3
"""How a game hands itself to whatever is running it.

A game exports one symbol, a `pse::Game` of three function pointers, written
with the `PSE_GAME` macro. It does not define the SDK's own `init`, `update`
and `render`: `cmake/standalone_main.cpp.in` is generated per game and writes
those, forwarding to the exported symbol. That indirection is the seam the
console is built on, because several games are linked into one binary and only
one of them can own a global named `init`.

This walks every game in the repo, because the failure it catches took main
red and nothing else here could see it. Picomon defined `init`, `update` and
`render` directly. It compiled, every host test passed, and the collision
appeared only at the device link, in a job that runs after the tests and only
on main, because this repo does not build pull requests. The host tests never
compile a game's SDK facing file at all, so the one signal available before
the merge was blind to it by construction.

Reading the source rather than compiling it is deliberate: there is no 32blit
SDK on a machine running the host tests, so compiling is not on the table, and
what went wrong is visible in the text.

Usage:
    test_game_entry.py
"""

import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
TOOLS = os.path.dirname(HERE)
REPO_ROOT = os.path.dirname(TOOLS)
sys.path.insert(0, TOOLS)

from build_plan import discover_games  # noqa: E402

failures = []


def check(ok, what):
    if not ok:
        failures.append(what)
        print("FAIL %s" % what)


def sources_of(game):
    """Every .cpp under a game's src/, with its text."""
    src = os.path.join(game.directory, "src")
    out = []
    for root, _dirs, files in os.walk(src):
        for name in sorted(files):
            if not name.endswith((".cpp", ".cc")):
                continue
            path = os.path.join(root, name)
            with open(path, "r", encoding="utf-8") as f:
                out.append((os.path.relpath(path, REPO_ROOT), f.read()))
    return out


def strip_comments(text):
    """Good enough to stop a comment counting as code. It only has to be
    right about lines that look like function definitions."""
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    return re.sub(r"//[^\n]*", "", text)


# `void init() {`, `void update(uint32_t time) {`, at the start of a line, so
# a member function or a namespaced one does not match.
ENTRY = re.compile(
    r"^\s*(?:void)\s+(init|update|render)\s*\([^)]*\)\s*\{", re.M)

PSE_GAME = re.compile(r"^\s*PSE_GAME\s*\(\s*([A-Za-z0-9_]+)", re.M)


def test_every_game_exports_one_pse_game():
    for game in discover_games():
        found = []
        for path, text in sources_of(game):
            found += [(path, m) for m in PSE_GAME.findall(strip_comments(text))]
        check(len(found) == 1,
              "%s exports exactly one PSE_GAME (found %d)"
              % (game.slug, len(found)))
        if len(found) != 1:
            continue
        # The macro pastes its first argument into the symbol name, and the
        # generated shim derives the same name from the slug with dashes
        # turned into underscores. A mismatch is an undefined reference at
        # link time and nothing at all before it.
        want = game.slug.replace("-", "_")
        check(found[0][1] == want,
              "%s names its PSE_GAME %r, the build expects %r (%s)"
              % (game.slug, found[0][1], want, found[0][0]))


def test_no_game_defines_the_sdk_entry_points():
    for game in discover_games():
        for path, text in sources_of(game):
            for name in ENTRY.findall(strip_comments(text)):
                check(False,
                      "%s defines a global %s(), which collides with the "
                      "generated standalone_main.cpp at link time: name it "
                      "game_%s and export it with PSE_GAME (%s)"
                      % (game.slug, name, name, path))


def test_this_test_can_see_the_thing_it_looks_for():
    """A walker that silently finds nothing passes forever."""
    games = list(discover_games())
    check(len(games) > 0, "there are games to check")
    check(any(sources_of(g) for g in games), "games have sources to read")
    check(bool(ENTRY.search("void init() {\n}\n")), "the entry pattern matches")
    check(not ENTRY.search("void game_init() {\n}\n"),
          "and does not match the correct form")
    check(not ENTRY.search("// void init() {"), "comments do not count")
    check(bool(PSE_GAME.search("PSE_GAME(dustrider, a, b, c);")),
          "the export pattern matches")


def main():
    test_this_test_can_see_the_thing_it_looks_for()
    test_every_game_exports_one_pse_game()
    test_no_game_defines_the_sdk_entry_points()
    if failures:
        print("game_entry: %d check(s) failed" % len(failures))
        return 1
    print("game_entry: all checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
