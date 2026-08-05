#!/usr/bin/env python3
"""What a game's web page promises about explaining itself.

Every game that ships to the web ships a mini tutorial: what it wants from the
player, and what each button does. The page used to say `Arrows or WASD - Z X
C V`, which are the keys the SDL build reads and told a player nothing about
the game in front of them.

The last test here is the one that keeps that true. It walks every game in the
repo rather than a fixture, so a new game with no objective, or an old one that
loses its controls, fails the build instead of shipping a page that cannot be
understood.

Usage:
    test_gen_shell.py
"""

import os
import shutil
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
TOOLS = os.path.dirname(HERE)
REPO_ROOT = os.path.dirname(TOOLS)
sys.path.insert(0, TOOLS)

import gen_shell  # noqa: E402
from build_plan import Game, GameError, discover_games  # noqa: E402

SHELL = os.path.join(REPO_ROOT, "web", "shell.html")

failures = []


def check(ok, what):
    if not ok:
        failures.append(what)
        print("FAIL %s" % what)


def write_game(directory, body):
    os.makedirs(directory, exist_ok=True)
    with open(os.path.join(directory, "game.yml"), "w",
              encoding="utf-8") as handle:
        handle.write(body)
    return directory


BASE = """slug: testgame
title: Test Game
blurb: A blurb.
target: testgame
sdk: 32blit
web: true
"""


def _game_from(body):
    """A Game built from a throwaway directory. The caller owns nothing: the
    manifest is read at construction, so the directory can go straight away."""
    tmp = tempfile.mkdtemp()
    try:
        write_game(os.path.join(tmp, "testgame"), body)
        return Game(os.path.join(tmp, "testgame"))
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def panels_for(body):
    return gen_shell.render_panels(_game_from(body))


def test_the_objective_leads():
    html = panels_for(BASE + 'objective: "Get to the end."\n')
    check("<h2>Objective</h2>" in html, "the objective gets a panel")
    check("Get to the end." in html, "the objective text is on it")
    check(html.index("Objective") < len(html),
          "the objective is the panel a player meets first")


def test_the_blurb_stands_in_for_a_missing_objective():
    """A game with no objective still says something rather than nothing."""
    html = panels_for(BASE)
    check("A blurb." in html, "the blurb fills in for a missing objective")


def test_a_control_becomes_a_key_and_a_meaning():
    html = panels_for(BASE + 'controls:\n  - "A: throttle"\n')
    check("<kbd>A</kbd>" in html, "the key is set apart")
    check("<span>throttle</span>" in html, "what it does is spelled out")


def test_a_control_without_a_colon_survives():
    """Not everything a game needs to say is one button."""
    html = panels_for(BASE + 'controls:\n  - "tilt to steer"\n')
    check("tilt to steer" in html, "a plain line is kept")
    check("<kbd>" not in html, "and is not forced into a key column")


def test_controls_split_across_panels():
    lines = "".join('  - "%d: does a thing"\n' % i for i in range(9))
    html = panels_for(BASE + "objective: \"Go.\"\ncontrols:\n" + lines)
    check(html.count("data-panel") == 4,
          "nine controls make three panels, after the objective")
    check("Controls 1/3" in html and "Controls 3/3" in html,
          "the panels say where you are in them")
    check("tut-prev" in html and "tut-next" in html,
          "arrows appear when there is somewhere to go")


def test_no_arrows_for_a_single_panel():
    html = panels_for(BASE + 'objective: "Go."\n')
    check("data-panel" in html, "the one panel is there")
    check("tut-next" not in html, "no arrows when there is nowhere to go")


def test_the_keyboard_keys_come_from_the_pad():
    """The mapping is read out of the shell's own gamepad, so it cannot drift
    from what those buttons actually dispatch."""
    with open(SHELL, encoding="utf-8") as handle:
        shell = handle.read()
    mapping = gen_shell.keyboard_map(shell)

    # Every console button has to be in there. If the pad's markup changes
    # shape the regex quietly matches nothing, the hints vanish from every
    # page, and nothing else would notice: that is what this guards.
    for button, expected in (("a", "Z"), ("b", "X"), ("x", "C"), ("y", "V")):
        check(mapping.get(button) == expected,
              "the pad says %s is %s" % (button.upper(), expected))
    for direction in ("up", "down", "left", "right"):
        check(direction in mapping, "the pad maps %s" % direction)
    check(len(mapping) >= 8, "all four buttons and four directions are mapped")


def test_a_control_carries_its_keyboard_key():
    mapping = {"a": "Z", "b": "X", "left": "←", "right": "→"}
    check(gen_shell.keyboard_hint("hold A", mapping) == "Z",
          "a button named with a word beside it still resolves")
    check(gen_shell.keyboard_hint("left/right", mapping) == "← →",
          "a pair of directions gives both keys in order")
    check(gen_shell.keyboard_hint("A", mapping) == "Z", "a bare button works")
    check(gen_shell.keyboard_hint("tilt", mapping) == "",
          "something that is not a button gets no key rather than an empty one")

    html = panels_for(BASE + 'controls:\n  - "A: throttle"\n')
    check('<kbd class="kb">Z</kbd>' not in html,
          "render_panels without a mapping adds no hints")
    html = gen_shell.render_panels(
        _game_from(BASE + 'controls:\n  - "A: throttle"\n'), mapping)
    check('<kbd class="kb">Z</kbd>' in html,
          "with a mapping, the key is shown beside the button")
    check("<kbd>A</kbd>" in html, "and the console button is still named")


def test_the_shell_is_filled_in():
    tmp = tempfile.mkdtemp()
    try:
        game = write_game(os.path.join(tmp, "testgame"),
                          BASE + 'objective: "Go."\ncontrols:\n  - "A: jump"\n')
        out = os.path.join(tmp, "out", "shell.html")
        gen_shell.build(game, SHELL, out)
        with open(out, encoding="utf-8") as handle:
            page = handle.read()
        check(gen_shell.PLACEHOLDER not in page, "the placeholder is consumed")
        check("<title>Test Game</title>" in page, "the page is titled by game")
        check("A jump" not in page and "<kbd>A</kbd>" in page,
              "the controls are rendered into the page")
        check("{{{ SCRIPT }}}" in page,
              "emscripten's own token is left alone")
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def test_a_shell_without_the_placeholder_is_an_error():
    """Silently shipping a page with no tutorial is the failure this whole
    file exists to prevent, so it has to be loud."""
    tmp = tempfile.mkdtemp()
    try:
        game = write_game(os.path.join(tmp, "testgame"), BASE)
        bare = os.path.join(tmp, "bare.html")
        with open(bare, "w", encoding="utf-8") as handle:
            handle.write("<html><body>nothing here</body></html>")
        try:
            gen_shell.build(game, bare, os.path.join(tmp, "out.html"))
            check(False, "a shell with no placeholder is rejected")
        except GameError:
            check(True, "a shell with no placeholder is rejected")
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def test_every_web_game_explains_itself():
    """The requirement, enforced against the real repo: a game that ships to
    the web ships an objective and its controls."""
    for game in discover_games():
        if not game.web:
            continue
        objective = game.manifest.get("objective")
        controls = game.manifest.get("controls") or []
        check(bool(objective),
              "%s has an objective for its tutorial" % game.slug)
        check(isinstance(controls, list) and len(controls) > 0,
              "%s lists its controls" % game.slug)
        html = gen_shell.render_panels(game)
        check("data-panel" in html, "%s renders tutorial panels" % game.slug)


def main():
    test_the_objective_leads()
    test_the_blurb_stands_in_for_a_missing_objective()
    test_a_control_becomes_a_key_and_a_meaning()
    test_a_control_without_a_colon_survives()
    test_controls_split_across_panels()
    test_no_arrows_for_a_single_panel()
    test_the_keyboard_keys_come_from_the_pad()
    test_a_control_carries_its_keyboard_key()
    test_the_shell_is_filled_in()
    test_a_shell_without_the_placeholder_is_an_error()
    test_every_web_game_explains_itself()
    if failures:
        print("gen_shell: %d check(s) failed" % len(failures))
        return 1
    print("gen_shell: all checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
