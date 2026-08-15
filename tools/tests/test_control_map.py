#!/usr/bin/env python3
"""The buttons a game READS and the buttons its page PROMISES are one mapping.

Rule 12 already says the keyboard key must never be written into `game.yml`,
because `A` is `Z` in exactly one place and a second copy is a mapping waiting
to go stale. The same argument applies one level up and had no check at all:
`game.yml` names a console button beside what it does, `src/game.cpp` wires
that button to an action, and nothing compared them.

That gap is not hypothetical. Twin Flare's air brake and repair were swapped
between X and Y on request, and the only thing that made the page agree was
remembering to edit both files. Swapping them back in the code alone left the
whole suite green, the tutorial confidently wrong, and the game unplayable in
the way that is hardest to debug: the page says X is the brake, X repairs, and
the player concludes the game is broken rather than the manual.

What is checked, per game that maps face buttons in a shape this can read:

  - a button the code reads is named somewhere in `controls`;
  - where an action's own word appears in the controls text, the button it
    appears under is the button the code wires it to.

The second is the swap detector, and it is deliberately narrow. A game whose
page words an action without ever using the action's name is skipped rather
than guessed at, and the run prints how many pairings were actually proven so
"passed" cannot quietly mean "checked nothing".

Usage:
    test_control_map.py
"""

import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO_ROOT = os.path.dirname(os.path.dirname(HERE))
GAMES = os.path.join(REPO_ROOT, "games")

FACE = ("A", "B", "X", "Y")

failures = []


def check(ok, what):
    if not ok:
        failures.append(what)
        print("FAIL: %s" % what)


def read(path):
    with open(path, encoding="utf-8") as handle:
        return handle.read()


def code_mapping(src):
    """{action: button} for `in.<action> = buttons[.pressed] & Button::<B>`.

    Only the face buttons. The dpad is skipped: every game means the same
    thing by it and no page spells the four directions out one per line.
    """
    out = {}
    pattern = r"in\.([a-z_]+)\s*=\s*buttons(?:\.pressed)?\s*&\s*Button::([A-Z_]+)"
    for action, button in re.findall(pattern, src):
        if button in FACE:
            out.setdefault(action, button)
    return out


def yml_controls(text):
    """[(keys, description)] from the `controls:` block of a game.yml.

    Hand rolled rather than via a yaml module for the same reason the rest of
    tools/tests is: this has to run on a bare runner with nothing installed.
    """
    lines = []
    in_block = False
    for raw in text.splitlines():
        if raw.startswith("controls:"):
            in_block = True
            continue
        if in_block:
            if raw.startswith("  - "):
                item = raw[4:].strip()
                if item[:1] in ("'", '"'):
                    item = item[1:-1] if item[-1:] == item[:1] else item[1:]
                key, sep, desc = item.partition(":")
                lines.append((key if sep else "", item if not sep else desc))
            elif raw.strip() and not raw.startswith(" "):
                break
    return lines


def names_button(key, button):
    """Is `button` one of the buttons this control line is about?

    Token matched, so `B` does not match the `B` inside `BOOST` and the line
    `B twice` still counts as naming B.
    """
    return re.search(r"(?<![A-Za-z])%s(?![A-Za-z])" % button, key) is not None


def audit(slug, src, yml):
    """(proven, list of failure strings) for one game."""
    mapping = code_mapping(src)
    if not mapping:
        return 0, []
    controls = yml_controls(yml)
    problems = []
    proven = 0
    for action, button in sorted(mapping.items()):
        here = [(k, d) for k, d in controls if names_button(k, button)]
        if not here:
            problems.append(
                "%s: the code reads %s for '%s' and the page never mentions %s"
                % (slug, button, action, button))
            continue
        word = action.replace("_", " ")
        if any(word in d.lower() for _, d in here):
            proven += 1
            continue
        # The action's word is not under this button. If it is under a
        # DIFFERENT one, the two files disagree and that is the swap.
        elsewhere = [k for k, d in controls
                     if word in d.lower() and not names_button(k, button)]
        if elsewhere:
            problems.append(
                "%s: the code wires '%s' to %s, the page puts it on '%s'"
                % (slug, action, button, elsewhere[0]))
    return proven, problems


def test_every_game_agrees_with_its_own_page():
    total_proven = 0
    covered = []
    for slug in sorted(os.listdir(GAMES)):
        game_cpp = os.path.join(GAMES, slug, "src", "game.cpp")
        game_yml = os.path.join(GAMES, slug, "game.yml")
        if not (os.path.isfile(game_cpp) and os.path.isfile(game_yml)):
            continue
        proven, problems = audit(slug, read(game_cpp), read(game_yml))
        for p in problems:
            check(False, p)
        if proven:
            covered.append("%s (%d)" % (slug, proven))
            total_proven += proven
    check(total_proven > 0,
          "at least one game's buttons were actually cross checked")
    print("  %d button pairings proven across %s"
          % (total_proven, ", ".join(covered) if covered else "nothing"))


def test_the_checker_notices_a_swap():
    """The check has to fail on the mistake it exists to catch.

    Without this, a parser that quietly matched nothing would report a clean
    sweep of zero pairings and look exactly like a clean sweep of real ones.
    """
    src = ("twinflare::Input read_input() {\n"
           "    twinflare::Input in{};\n"
           "    in.brake = buttons & Button::X;\n"
           "    in.repair = buttons & Button::Y;\n"
           "    return in;\n"
           "}\n")
    yml = ('controls:\n'
           '  - "X: air brake, and it nearly doubles how hard you can turn"\n'
           '  - "Y: repair both engines, at a third of the throttle"\n')
    proven, problems = audit("fake", src, yml)
    check(proven == 2 and not problems,
          "the checker passes a game whose code and page agree")

    swapped = src.replace("Button::X;", "TMP").replace(
        "Button::Y;", "Button::X;").replace("TMP", "Button::Y;")
    proven, problems = audit("fake", swapped, yml)
    check(problems, "the checker FAILS a game whose buttons were swapped")

    missing = ('controls:\n'
               '  - "B: throttle"\n')
    proven, problems = audit("fake", src, missing)
    check(problems, "and one whose page never mentions the buttons it reads")


def main():
    test_every_game_agrees_with_its_own_page()
    test_the_checker_notices_a_swap()
    if failures:
        print("control map: %d check(s) failed" % len(failures))
        return 1
    print("control map: all checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
