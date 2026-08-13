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
import re
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


def test_rules_get_their_own_panels():
    """A game with a scoring system explains it, and does it between the
    objective and the controls rather than inside the objective."""
    html = panels_for(BASE + 'objective: "Go."\nrules:\n'
                      '  - "Hands: five of a kind beats four."\n'
                      '  - "Lines: all five of them pay."\n')
    check(html.count("data-panel") == 3, "two rules make two more panels")
    check("<h2>Hands</h2>" in html, "a rule's label becomes its heading")
    check("five of a kind beats four." in html, "and the rest is the body")
    check(html.index("Objective") < html.index("Hands") < html.index("Lines"),
          "rules follow the objective, in the order game.yml gives them")

    with_controls = panels_for(BASE + 'objective: "Go."\nrules:\n'
                               '  - "Hands: five beats four."\n'
                               'controls:\n  - "A: pull"\n')
    check(with_controls.index("Hands") < with_controls.index("Controls"),
          "and come before the controls")


def test_a_rule_without_a_colon_still_gets_a_panel():
    html = panels_for(BASE + 'rules:\n  - "Just keep the ball up."\n')
    check("Just keep the ball up." in html, "the prose is kept")
    check("<h2>How to play</h2>" in html, "under a heading of its own")


def test_no_rules_is_the_normal_case():
    """Most games do not need this. A missing rules list has to cost nothing,
    or every game pays for the one game that wanted it."""
    html = panels_for(BASE + 'objective: "Go."\ncontrols:\n  - "A: jump"\n')
    check(html.count("data-panel") == 2, "objective and controls, and no more")


def test_a_rule_can_carry_a_picture():
    """Some rules are shapes and sums, and a paragraph is the worst way to hand
    somebody either. A picture is a file named after the rule's own heading."""
    check(gen_shell.diagram_name("Chips and mult") == "chips-and-mult",
          "a heading names its file")
    check(gen_shell.diagram_name("Paylines") == "paylines",
          "and a one word heading is just the word")

    tmp = tempfile.mkdtemp()
    try:
        game_dir = write_game(os.path.join(tmp, "testgame"),
                              BASE + 'rules:\n  - "Chips and mult: A sum."\n'
                              '  - "Hands: No picture for this one."\n')
        os.makedirs(os.path.join(game_dir, "tutorial"))
        with open(os.path.join(game_dir, "tutorial", "chips-and-mult.svg"),
                  "w", encoding="utf-8") as handle:
            handle.write('<?xml version="1.0"?>\n'
                         '<svg xmlns="http://www.w3.org/2000/svg" '
                         'viewBox="0 0 10 10"><rect id="picture"/></svg>')
        html = gen_shell.render_panels(Game(game_dir))
        check('<rect id="picture"/>' in html, "the picture is inlined")
        check("<?xml" not in html,
              "and its XML declaration is stripped, or it prints as text")
        check(html.count("<svg") == 1,
              "the rule with no picture gets none rather than a blank figure")
        # The picture belongs to its own rule, not to whichever panel is first.
        chips = html[html.index("Chips and mult"):html.index("Hands")]
        check("<svg" in chips, "and it lands in the panel whose heading it is")
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def test_every_picture_is_claimed_by_a_rule():
    """A picture is found by its rule's heading, so renaming a heading orphans
    the file and the panel silently loses its diagram.

    Walked over the real repo, like the objective check below: nothing else
    would notice, because a missing picture renders as a game that never had
    one. An orphan here is a build failure instead.
    """
    for game in discover_games():
        rules = game.manifest.get("rules") or []
        wanted = {gen_shell.diagram_name(str(r).partition(":")[0])
                  for r in rules if str(r).strip()}
        for name in gen_shell.diagrams_of(game.directory):
            check(name in wanted,
                  "%s ships tutorial/%s.svg, which no rule heading claims "
                  "(headings: %s)" % (game.slug, name,
                                      ", ".join(sorted(wanted)) or "none"))


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
    #
    # The face keys are WASD laid over the diamond in the positions the buttons
    # physically sit in, which is what the shell intercepts: X is the top
    # button so it is W, Y is the left one so it is A, B is the bottom one so
    # it is S, A is the right one so it is D. These are `data-kbd`, what a
    # person presses, not `data-key`, what the button dispatches to SDL.
    for button, expected in (("x", "W"), ("y", "A"), ("b", "S"), ("a", "D")):
        check(mapping.get(button) == expected,
              "the pad says %s is %s" % (button.upper(), expected))
    for direction in ("up", "down", "left", "right"):
        check(direction in mapping, "the pad maps %s" % direction)
    check(len(mapping) >= 8, "all four buttons and four directions are mapped")


def test_the_pad_dispatches_what_sdl_reads():
    """The two keys on a face button are different things, and swapping them
    breaks something that still looks fine.

    `data-kbd` is what a person presses. `data-key` is what the button
    dispatches, and the SDL build reads only z x c v for A B X Y: dispatch a
    `KeyW` and the game gets a D-pad press, or nothing. So the panels follow
    data-kbd and the wire stays data-key, and this pins both.
    """
    with open(SHELL, encoding="utf-8") as handle:
        shell = handle.read()

    pad = gen_shell.pad_buttons(shell)
    for button, dispatched, pressed in (("x", "KeyC", "KeyW"),
                                        ("y", "KeyV", "KeyA"),
                                        ("a", "KeyZ", "KeyD"),
                                        ("b", "KeyX", "KeyS")):
        tag = pad.get(button)
        check(tag is not None, "the pad still has a %s button" % button.upper())
        if not tag:
            continue
        check(tag["key"] == dispatched,
              "%s dispatches %s, which is what SDL reads"
              % (button.upper(), dispatched))
        check(tag["kbd"] == pressed,
              "%s is pressed with %s" % (button.upper(), pressed))

    # The D-pad is the control case: arrows are already the key you press, so
    # those buttons carry no data-kbd and must keep dispatching the arrows.
    for direction, dispatched in (("up", "ArrowUp"), ("down", "ArrowDown"),
                                  ("left", "ArrowLeft"), ("right", "ArrowRight")):
        tag = pad.get(direction)
        check(tag is not None and tag["key"] == dispatched and not tag["kbd"],
              "%s dispatches %s and is not reassigned" % (direction, dispatched))

    # The interception is the whole reason WASD can mean anything but the
    # D-pad: the SDL build binds those keys itself, so the shell has to take
    # them before it sees them. Without capture, this silently does nothing.
    check("data-kbd" in shell and "stopImmediatePropagation" in shell,
          "the shell still intercepts the reassigned keys")
    check(re.search(r"addEventListener\('keydown'[\s\S]{0,400}?\}, true\)", shell)
          is not None,
          "and does it in the capture phase, before the SDL port's listener")


def test_the_guide_owns_the_keyboard_while_it_is_open():
    """The arrows page the tutorial, and nothing reaches the game behind it.

    Neither was true. The only thing that turned a page was clicking an arrow,
    so a keyboard player could not reach panel two at all, and pressing left or
    right instead sent the press straight through to the game, which moved its
    speed dial behind a modal the player was reading.

    Checked against the shell's source the way the WASD interception is: this
    file has no browser, and the mechanism vanishing silently is the failure
    worth catching. The behaviour itself was driven with real key presses in a
    real browser when it was written.
    """
    with open(SHELL, encoding="utf-8") as handle:
        shell = handle.read()

    guide = shell[shell.index("var GAME_KEYS"):] if "var GAME_KEYS" in shell \
        else ""
    check(bool(guide), "the shell has the guide's key handler")
    if not guide:
        return
    for code in ("ArrowLeft", "ArrowRight", "Escape"):
        check(code in guide, "the guide handles %s" % code)
    check("stopImmediatePropagation" in guide,
          "and stops what it handles, so the game never sees it")
    check(re.search(r"addEventListener\('keydown'[\s\S]{0,600}?\}, true\)",
                    guide) is not None,
          "in the capture phase, ahead of the SDL port's own listener")
    # The swallowed set is read off the pad rather than typed, so a remapped
    # button changes one place and this follows.
    check("querySelectorAll('#pad button[data-key]')" in guide,
          "the keys it swallows come from the pad's own markup")

    # And the other half: the pad bridge stands down while the guide is up, or
    # WASD would still be pressed into the game from under the modal.
    bridge = shell[shell.index("function kbdTarget"):
                   shell.index("window.addEventListener('keydown'")]
    check("guideIsOpen()" in bridge,
          "the pad bridge sends nothing to the game while the guide is open")


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
    test_rules_get_their_own_panels()
    test_a_rule_without_a_colon_still_gets_a_panel()
    test_no_rules_is_the_normal_case()
    test_a_rule_can_carry_a_picture()
    test_every_picture_is_claimed_by_a_rule()
    test_controls_split_across_panels()
    test_no_arrows_for_a_single_panel()
    test_the_keyboard_keys_come_from_the_pad()
    test_the_pad_dispatches_what_sdl_reads()
    test_the_guide_owns_the_keyboard_while_it_is_open()
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
