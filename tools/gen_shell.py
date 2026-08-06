#!/usr/bin/env python3
"""Build one game's web page from the shared shell and its own game.yml.

The shell is the same for every game, so anything game specific has to be put
into it at build time. What goes in is the mini tutorial: what the game wants
from you, and what each button does, as panels you can page through beside the
screen.

Before this, the page said `Arrows or WASD - Z X C V`. Those are the keys the
SDL build reads, and they told a player nothing: not what any of them does in
this game, and not what the game is asking of them. Every game's game.yml
already carries that in `objective` and `controls`; this puts it on the page.

Usage:
    gen_shell.py --game games/kingfisher --shell web/shell.html --out shell.html
"""

import argparse
import html
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from build_plan import Game, GameError  # noqa: E402

# What the shell marks for replacement. A comment rather than a brace token so
# the file stays valid HTML and can be opened on its own while working on it.
PLACEHOLDER = "<!--PSE_TUTORIAL-->"

# Controls per panel. Four lines is about what fits under the screen on a
# phone without pushing the gamepad off the bottom, which is the constraint
# that decided this rather than any aesthetic.
CONTROLS_PER_PANEL = 4


def escape(value):
    return html.escape(str(value), quote=True)


# How a browser key code reads to a person. Everything else is shown as the
# letter it is: KeyZ is Z.
KEY_LABELS = {
    "ArrowUp": "↑",
    "ArrowDown": "↓",
    "ArrowLeft": "←",
    "ArrowRight": "→",
}


def keyboard_map(shell):
    """What each console button is on a keyboard, read out of the shell.

    The on-screen pad already carries the mapping, one `data-btn`/`data-dir`
    beside its keys, because that is what its buttons dispatch. Reading it from
    there rather than writing it down again means the tutorial cannot drift
    from what the buttons actually do: change the pad and the panels follow on
    the next build.

    A face button carries two keys and the difference matters here. `data-key`
    is what it dispatches, which is what the SDL build reads, and `data-kbd` is
    what a person presses to work it. The panels are read by people, so
    `data-kbd` wins wherever it exists. The D-pad has no `data-kbd` because the
    arrows are already the key you press.
    """
    mapping = {}
    for tag in pad_buttons(shell).values():
        code = tag.get("kbd") or tag.get("key")
        if not code:
            continue
        label = KEY_LABELS.get(code)
        if label is None:
            label = code[3:] if code.startswith("Key") else code
        mapping[tag["name"]] = label
    return mapping


def pad_buttons(shell):
    """The pad's buttons, by console button name, with the attributes each one
    carries.

    Scoped to real `<button>` tags rather than to the attribute on its own,
    because the shell's stylesheet selects on the same attributes: a bare
    `data-btn="x"` matches `.face button[data-btn="x"] { ... }` first, and the
    keys read off that are simply not there. The result was a mapping that
    happened to come out right only because the markup is parsed after the CSS
    and empty matches were skipped.
    """
    found = {}
    for tag in re.findall(r"<button\b[^>]*>", shell):
        attributes = dict(re.findall(r'(?:data-)?(\w+)="([^"]*)"', tag))
        name = attributes.get("btn") or attributes.get("dir")
        if not name:
            continue
        found[name] = {"name": name,
                       "key": attributes.get("key"),
                       "kbd": attributes.get("kbd")}
    return found


def keyboard_hint(text, mapping):
    """The keyboard keys named in a control's key half, in the order given.

    "hold A" is Z, "left/right" is the two arrows. Words that are not buttons
    ("hold", "then") carry no key and are skipped, and a control naming no
    button at all gets no hint rather than an empty one.
    """
    seen = []
    for word in re.findall(r"[A-Za-z]+", text):
        label = mapping.get(word.lower())
        if label and label not in seen:
            seen.append(label)
    return " ".join(seen)


def split_controls(controls, per_panel=CONTROLS_PER_PANEL):
    return [controls[i:i + per_panel]
            for i in range(0, len(controls), per_panel)]


def render_panels(game, mapping=None):
    """The tutorial panels for one game, innermost markup only.

    Panel one is what the game is for. The rest are its controls, in the order
    game.yml lists them, because that order is the game's own idea of which
    button matters most.

    mapping turns the console buttons a game names into the keys a keyboard
    player actually presses. A game says "A: throttle" because that is the
    button on the console and on the on-screen pad, and someone at a keyboard
    has no way to know that is Z.
    """
    mapping = mapping or {}
    panels = []

    objective = game.manifest.get("objective") or game.manifest.get("blurb")
    if objective:
        panels.append(
            '<article class="panel" data-panel>'
            '<h2>Objective</h2><p>%s</p></article>' % escape(objective))

    controls = game.manifest.get("controls") or []
    if not isinstance(controls, list):
        controls = []
    pages = split_controls([c for c in controls if str(c).strip()])
    for index, page in enumerate(pages):
        rows = []
        for entry in page:
            # "A: throttle" splits into the key and what it does. Anything
            # without a colon is a whole line of its own: some things a game
            # needs to say are not one button.
            key, sep, meaning = str(entry).partition(":")
            if sep:
                hint = keyboard_hint(key, mapping)
                keys = '<kbd>%s</kbd>' % escape(key.strip())
                if hint:
                    keys += '<kbd class="kb">%s</kbd>' % escape(hint)
                rows.append('<li>%s<span>%s</span></li>'
                            % (keys, escape(meaning.strip())))
            else:
                rows.append('<li><span>%s</span></li>' % escape(str(entry)))
        heading = "Controls" if len(pages) == 1 else "Controls %d/%d" % (
            index + 1, len(pages))
        panels.append('<article class="panel" data-panel><h2>%s</h2>'
                      '<ul class="keys">%s</ul></article>'
                      % (escape(heading), "".join(rows)))

    if not panels:
        return ""

    dots = "".join('<span class="dot" data-dot></span>'
                   for _ in range(len(panels)))
    # The arrows are only worth drawing when there is somewhere to go.
    nav = ""
    if len(panels) > 1:
        nav = ('<button class="tut-arrow" id="tut-prev" type="button" '
               'aria-label="Previous">&#9664;</button>'
               '<div class="dots">%s</div>'
               '<button class="tut-arrow" id="tut-next" type="button" '
               'aria-label="Next">&#9654;</button>' % dots)

    return ('<section id="tutorial" aria-label="How to play">'
            '<div class="panels">%s</div>'
            '<div class="tut-nav">%s</div>'
            '</section>' % ("".join(panels), nav))


def build(game_dir, shell_path, out_path):
    game = Game(os.path.abspath(game_dir))
    with open(shell_path, "r", encoding="utf-8") as handle:
        shell = handle.read()

    if PLACEHOLDER not in shell:
        raise GameError("%s has no %s to fill in" % (shell_path, PLACEHOLDER))

    page = shell.replace(PLACEHOLDER,
                         render_panels(game, keyboard_map(shell)))
    page = page.replace("<title>PicoSystem</title>",
                        "<title>%s</title>" % escape(game.title))

    out_dir = os.path.dirname(os.path.abspath(out_path))
    if out_dir:
        os.makedirs(out_dir, exist_ok=True)
    with open(out_path, "w", encoding="utf-8") as handle:
        handle.write(page)
    return game


def main(argv):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--game", required=True, help="the game's directory")
    parser.add_argument("--shell", required=True, help="the shared shell")
    parser.add_argument("--out", required=True, help="where to write it")
    args = parser.parse_args(argv)

    try:
        game = build(args.game, args.shell, args.out)
    except GameError as error:
        sys.stderr.write("gen_shell: %s\n" % error)
        return 1
    sys.stderr.write("gen_shell: %s -> %s\n" % (game.slug, args.out))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
