#!/usr/bin/env python3
"""How a game asks for a screen, on a repo that now has two shaped devices.

Games call `pse::set_screen_mode`, never the SDK's bare `set_screen_mode`. The
difference is one argument: the engine's version passes the design bounds from
`pse/board.hpp` explicitly, and the SDK's version takes whatever the fitted
panel happens to be.

On a PicoSystem those are the same 240x240 and the two calls are identical,
which is exactly what makes this worth a test. A game that reaches for the SDK
call still compiles, still links, and still runs correctly on every board this
repo could build for until the Tufty 2350 arrived. On the Tufty it gets a
320x240 surface instead of a centred 240x240 one, and then draws a 120 wide
picture into a 160 wide screen: everything measured from the right hand edge
lands 40 pixels out, everything centred is off by 20, and nothing anywhere
reports an error.

That failure is invisible to every other check here. The host tests do not
compile a game's SDK facing file at all, both boards compile this happily, and
this repo does not build pull requests, so the first place it could show up is
a photograph of a device.

Reading the source rather than compiling it is the same bargain
test_game_entry.py makes, and for the same reason: there is no 32blit SDK on a
machine running the host tests.

Usage:
    test_screen_mode.py
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


def strip_comments(text):
    """Good enough to stop a comment counting as code."""
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    return re.sub(r"//[^\n]*", "", text)


# Any call to set_screen_mode that is NOT reached through pse::. A preceding
# `pse::` or `.`/`->` (a member call on something of our own) is fine; a bare
# name, or an explicit `blit::`, is the SDK's and is what this is looking for.
SDK_CALL = re.compile(r"(?<![\w:.>])(?:blit::)?set_screen_mode\s*\(")
PSE_CALL = re.compile(r"pse::set_screen_mode\s*\(")


def sources_under(directory):
    """Every .cpp under a directory, with its text."""
    out = []
    for root, _dirs, files in os.walk(directory):
        for name in sorted(files):
            if not name.endswith((".cpp", ".cc")):
                continue
            path = os.path.join(root, name)
            with open(path, "r", encoding="utf-8") as f:
                out.append((os.path.relpath(path, REPO_ROOT), f.read()))
    return out


def game_sources():
    for game in discover_games():
        for path, text in sources_under(os.path.join(game.directory, "src")):
            yield game.slug, path, text


def console_sources():
    for path, text in sources_under(os.path.join(REPO_ROOT, "console", "src")):
        yield "console", path, text


def test_nothing_calls_the_sdk_screen_mode():
    for slug, path, text in list(game_sources()) + list(console_sources()):
        body = strip_comments(text)
        # Take the pse:: calls out first so the bare pattern cannot match the
        # tail of a correct one.
        body = PSE_CALL.sub("", body)
        if SDK_CALL.search(body):
            check(False,
                  "%s calls the SDK's set_screen_mode directly, which asks for "
                  "the whole panel: use pse::set_screen_mode so the game gets "
                  "the size it was drawn for on every board (%s)"
                  % (slug, path))


def test_the_engine_is_the_only_place_that_may():
    """The one file allowed to make the SDK call is the one that wraps it."""
    engine = os.path.join(REPO_ROOT, "engine", "src")
    allowed = os.path.join("engine", "src", "blit_target.cpp")
    for path, text in sources_under(engine):
        body = PSE_CALL.sub("", strip_comments(text))
        if SDK_CALL.search(body):
            check(path == allowed,
                  "only %s may call the SDK's set_screen_mode, %s does too"
                  % (allowed, path))


def test_this_test_can_see_the_thing_it_looks_for():
    """A walker that silently finds nothing passes forever."""
    found = list(game_sources())
    check(len(found) > 0, "there are game sources to read")
    check(len(list(console_sources())) > 0, "there are console sources to read")

    check(bool(SDK_CALL.search("set_screen_mode(ScreenMode::lores);")),
          "the bare SDK call matches")
    check(bool(SDK_CALL.search("blit::set_screen_mode(ScreenMode::hires);")),
          "an explicitly namespaced SDK call matches")
    check(not SDK_CALL.search("pse::set_screen_mode(pse::ScreenMode::lores);"),
          "the engine call does not match")
    check(bool(PSE_CALL.search("pse::set_screen_mode(pse::ScreenMode::lores);")),
          "the engine pattern matches the engine call")
    # The pattern has no line anchor, so comments are the caller's job to
    # remove. Check the pair the way it is actually used, not the regex alone.
    check(not SDK_CALL.search(
              strip_comments("// set_screen_mode(ScreenMode::lores);")),
          "comments do not count")

    # The substitution order matters: a correct call contains the bare name.
    body = PSE_CALL.sub("", "pse::set_screen_mode(pse::ScreenMode::lores);")
    check(not SDK_CALL.search(body),
          "removing the engine calls does not leave a false positive")


def main():
    test_this_test_can_see_the_thing_it_looks_for()
    test_nothing_calls_the_sdk_screen_mode()
    test_the_engine_is_the_only_place_that_may()
    if failures:
        print("screen_mode: %d check(s) failed" % len(failures))
        return 1
    print("screen_mode: all checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
