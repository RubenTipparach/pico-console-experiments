#!/usr/bin/env python3
"""How big the shared depth buffer has to be for the games in one build.

The engine keeps one depth buffer for every 3D game to share, because only one
game runs at a time and nothing in it survives leaving. That already worked for
games drawing the default 120x120 lores square. It did not work for a game
drawing a different shape: jokerreels renders a 240x112 band, could not use a
120x120 buffer, and so carried its own 26,880 byte one alongside the shared
one, which is exactly the duplication the shared one exists to prevent.

So the buffer is sized here instead of being fixed: the largest window any game
in THIS build asks for, and games ask in their own `game.yml`.

    render:
      width: 240
      height: 112

Sizing it per build rather than per repo is the whole point. A standalone
dustrider is 120x120 and pays 14,400 bytes, exactly what it paid before. Put
jokerreels on the console and the shared buffer becomes 26,880 for everyone,
which is still 14,400 less than the two separate buffers it replaces.

Usage:
    depth_arena.py                     every game in the repo
    depth_arena.py --only-game <slug>  one game, as PICO_ONLY_GAME builds do
    depth_arena.py --console           the games console.yaml lists
"""

import argparse
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO_ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)

# The default window, which must match PSE_RENDER_WIDTH/HEIGHT in
# engine/include/pse/config.hpp. A game that says nothing wants this.
DEFAULT_WIDTH = 120
DEFAULT_HEIGHT = 120


def read_render_size(game_yml):
    """The `render:` block of one game.yml, as (width, height).

    Deliberately a small hand parse rather than a yaml import: this runs at
    CMake configure time on whatever python is on PATH, and the rest of the
    build tooling already treats a yaml dependency as optional.
    """
    width, height = DEFAULT_WIDTH, DEFAULT_HEIGHT
    in_render = False
    try:
        with open(game_yml, "r", encoding="utf-8") as handle:
            for raw in handle:
                line = raw.split("#", 1)[0].rstrip()
                if not line.strip():
                    continue
                indented = line[:1].isspace()
                if not indented:
                    in_render = line.strip().startswith("render:")
                    continue
                if not in_render:
                    continue
                key, _, value = line.strip().partition(":")
                value = value.strip()
                if not value.isdigit():
                    continue
                if key.strip() == "width":
                    width = int(value)
                elif key.strip() == "height":
                    height = int(value)
    except OSError:
        pass
    return width, height


def console_slugs():
    path = os.path.join(REPO_ROOT, "console.yaml")
    slugs = []
    in_menu = False
    try:
        with open(path, "r", encoding="utf-8") as handle:
            for raw in handle:
                line = raw.split("#", 1)[0].rstrip()
                if not line.strip():
                    continue
                if not line[:1].isspace():
                    in_menu = line.strip().startswith("menu:")
                    continue
                if not in_menu:
                    continue
                stripped = line.strip().lstrip("-").strip()
                key, _, value = stripped.partition(":")
                if key.strip() == "game" and value.strip():
                    slugs.append(value.strip())
    except OSError:
        pass
    return slugs


def game_dirs(only_game, console):
    games = os.path.join(REPO_ROOT, "games")
    if only_game:
        return [os.path.join(games, only_game)]
    if console:
        return [os.path.join(games, slug) for slug in console_slugs()]
    if not os.path.isdir(games):
        return []
    return [os.path.join(games, name) for name in sorted(os.listdir(games))]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--only-game", default="")
    parser.add_argument("--console", action="store_true")
    args = parser.parse_args()

    # Never smaller than the default: a build with no games at all still has
    # to link an engine, and a zero byte buffer would draw nothing with no
    # explanation.
    largest = DEFAULT_WIDTH * DEFAULT_HEIGHT
    for directory in game_dirs(args.only_game, args.console):
        game_yml = os.path.join(directory, "game.yml")
        if not os.path.isfile(game_yml):
            continue
        width, height = read_render_size(game_yml)
        largest = max(largest, width * height)

    print(largest)
    return 0


if __name__ == "__main__":
    sys.exit(main())
