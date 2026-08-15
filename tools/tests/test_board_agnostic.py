#!/usr/bin/env python3
"""No game names one board's hardware, and nothing lights an LED.

A game draws through `pse::RenderTarget` and reads `blit::buttons`. It does not
touch GPIO, and it does not include a board header, because there are two
boards now and a pin number means different things on each. It does not drive
the LED either, by any route: the light on the case is not part of any game
here, and every use of it so far has been somebody debugging.

This exists because of a specific failure, and the failure is worth writing
down. Dust Rider carried a temporary debugging header that lit the PicoSystem's
RGB LED at checkpoints, straight at the GPIO, and it included
`boards/pimoroni_picosystem.h` to get the pin numbers. That header is in the
Pico SDK whichever board is being built, so it compiled perfectly for the
Tufty 2350 and produced a binary whose first act on entering the game was:

    gpio_set_dir(PICOSYSTEM_LED_R_PIN, GPIO_OUT);   // GPIO 14
    gpio_put(PICOSYSTEM_LED_R_PIN, 1);

On a PicoSystem GPIO 14 is the red LED. On a Tufty it is BW_RESET_SW, the line
wired to the RESET button. Dust Rider was the only game of the ten on the
console that would not launch, and it was the only game with that include.

Everything about that was invisible to the tooling: it compiled, it linked, it
passed every host test, and on the board it was written for it worked exactly
as intended. Only the second board could see it, and only by failing to boot.

Reading the source rather than compiling it is the same bargain
test_game_entry.py makes: there is no SDK on a machine running the host tests,
and what went wrong is visible in the text.

Usage:
    test_board_agnostic.py
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
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    return re.sub(r"//[^\n]*", "", text)


# `#include "boards/anything.h"` or <boards/anything.h>. The SDK ships every
# board's header regardless of which one is selected, so including one is
# always a game deciding for itself which console it is on.
BOARD_HEADER = re.compile(r"""#\s*include\s*[<"]boards/[^">]+[">]""")

# Naming a board's own macros, which is the same mistake without the include:
# PICOSYSTEM_LED_R_PIN, BW_SWITCH_A, and anything else prefixed for one board.
#
# Non capturing, so findall reports the whole macro rather than just the
# prefix: "names PICOSYSTEM" is not something you can go and look for.
BOARD_MACRO = re.compile(r"\b(?:PICOSYSTEM|BW)_[A-Z0-9_]+\b")

# Direct GPIO. A game has no business here on either board: the buttons arrive
# through blit::buttons and the LED through the SDK's own API, both of which
# are already per board.
GPIO_HEADER = re.compile(r"""#\s*include\s*[<"]hardware/gpio\.h[">]""")

# The SDK's own LED, `blit::LED = Pen(...)`, which the raw GPIO patterns above
# would not see. Both boards have one and neither game uses it: the only thing
# it has ever been for here is a debugging aid somebody meant to remove.
# `\bLED\b` rather than a looser match so LEDGE and led_count are not swept up.
BLIT_LED = re.compile(r"\bLED\b")


def sources_of(game):
    src = os.path.join(game.directory, "src")
    out = []
    for root, _dirs, files in os.walk(src):
        for name in sorted(files):
            if not name.endswith((".cpp", ".cc", ".h", ".hpp")):
                continue
            path = os.path.join(root, name)
            with open(path, "r", encoding="utf-8") as handle:
                out.append((os.path.relpath(path, REPO_ROOT), handle.read()))
    return out


def test_no_game_includes_a_board_header():
    for game in discover_games():
        for path, text in sources_of(game):
            body = strip_comments(text)
            if BOARD_HEADER.search(body):
                check(False,
                      "%s includes a board header, so it is built for one "
                      "console whichever one it is compiled for (%s)"
                      % (game.slug, path))
            if GPIO_HEADER.search(body):
                check(False,
                      "%s includes hardware/gpio.h: a pin number means a "
                      "different thing on each board, and a game should not "
                      "know any (%s)" % (game.slug, path))
            if BLIT_LED.search(body):
                check(False,
                      "%s drives the LED, which is a debugging aid rather "
                      "than part of a game (%s)" % (game.slug, path))
            found = BOARD_MACRO.findall(body)
            if found:
                check(False,
                      "%s names %s, which is one board's own pin (%s)"
                      % (game.slug, sorted(set(found))[0], path))


def test_this_test_can_see_the_thing_it_looks_for():
    """A walker that silently finds nothing passes forever."""
    games = list(discover_games())
    check(len(games) > 0, "there are games to check")
    check(any(sources_of(g) for g in games), "games have sources to read")

    check(bool(BOARD_HEADER.search('#include "boards/pimoroni_picosystem.h"')),
          "a quoted board include matches")
    check(bool(BOARD_HEADER.search("#include <boards/pimoroni_tufty2350.h>")),
          "an angled one matches too")
    check(not BOARD_HEADER.search('#include "pse/blit_target.hpp"'),
          "an ordinary include does not")

    check(bool(GPIO_HEADER.search('#include "hardware/gpio.h"')),
          "the gpio include matches")

    check(bool(BLIT_LED.search("LED = Pen(255, 0, 0);")), "the SDK LED matches")
    check(bool(BLIT_LED.search("blit::LED = c;")), "namespaced too")
    check(not BLIT_LED.search("int ledge = 0;"), "and a word containing it does not")

    check(bool(BOARD_MACRO.search("gpio_put(PICOSYSTEM_LED_R_PIN, 1);")),
          "a PicoSystem pin macro matches")
    check(bool(BOARD_MACRO.search("if (gpio & (1 << BW_SWITCH_A))")),
          "a Tufty pin macro matches")
    check(not BOARD_MACRO.search("PSE_RENDER_WIDTH"),
          "and an engine macro does not")

    check(not BOARD_HEADER.search(
              strip_comments('// #include "boards/pimoroni_picosystem.h"')),
          "comments do not count")


def main():
    test_this_test_can_see_the_thing_it_looks_for()
    test_no_game_includes_a_board_header()
    if failures:
        print("board_agnostic: %d check(s) failed" % len(failures))
        return 1
    print("board_agnostic: all checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
