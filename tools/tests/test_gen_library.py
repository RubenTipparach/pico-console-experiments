#!/usr/bin/env python3
"""What the console's menu generator refuses.

Every check here stands for a console that boots into something wrong and
cannot be diagnosed by looking at the device: a menu row that runs a game
which is not in the binary, a name drawn as holes because the font has no
picture for it, a title printed through the battery icon, one game on two
rows fighting over one save. PicoCrystal's ROM generator refuses the same
class of thing for the same reason, and that refusal is the part worth
copying.

A long row name is deliberately not in that list. The menu scrolls one, so it
is a name to look at rather than a build to fail, and the test below holds the
generator to letting it through.

The last test walks the repository's own console.yaml, so the console this
repo actually ships is held to the same rules as the fixtures.

Usage:
    test_gen_library.py
"""

import os
import shutil
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
TOOLS = os.path.dirname(HERE)
REPO_ROOT = os.path.dirname(TOOLS)
sys.path.insert(0, TOOLS)

import gen_library  # noqa: E402

failures = []


def check(ok, what):
    if not ok:
        failures.append(what)
        print("FAIL %s" % what)


def build(config_text, games=("alpha", "beta")):
    """Run the generator over a throwaway repository, return its catalog."""
    tmp = tempfile.mkdtemp()
    try:
        games_dir = os.path.join(tmp, "games")
        for slug in games:
            os.makedirs(os.path.join(games_dir, slug))
            with open(os.path.join(games_dir, slug, "game.yml"), "w",
                      encoding="utf-8") as handle:
                handle.write("slug: %s\ntitle: %s\nsdk: 32blit\nweb: true\n"
                             % (slug, slug.capitalize()))
        config = os.path.join(tmp, "console.yaml")
        with open(config, "w", encoding="utf-8") as handle:
            handle.write(config_text)
        return gen_library.build_catalog(config, games_dir)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def refuses(config_text, because, games=("alpha", "beta")):
    try:
        build(config_text, games)
    except gen_library.LibraryError as error:
        print("  refused: %s" % str(error).split(":")[-1].strip()[:70])
        return True
    print("FAIL accepted a console.yaml that %s" % because)
    failures.append(because)
    return False


BASE = "title: TEST\n\nmenu:\n  - game: alpha\n"


def test_a_list_becomes_a_menu():
    title, entries, slugs = build(BASE + "  - game: beta\n")
    check(title == "TEST", "the title comes through")
    check(slugs == ["alpha", "beta"], "the games come through in menu order")
    check([e["name"] for e in entries] == ["ALPHA", "BETA"],
          "names default to the game.yml title, upper cased")
    check(all(e["icon"] is not None for e in entries),
          "a game with no thumbnail still gets a picture")


def test_headings_are_rows_with_no_game():
    _, entries, slugs = build("title: T\n\nmenu:\n  - heading: GROUP\n"
                              "  - game: alpha\n")
    check(len(entries) == 2, "the heading is a row")
    check(entries[0]["game"] is None, "a heading runs nothing")
    check(slugs == ["alpha"], "and is not compiled in as a game")


def test_a_name_can_be_overridden():
    _, entries, _ = build("title: T\n\nmenu:\n  - game: alpha\n"
                          "    name: SHORT ONE\n")
    check(entries[0]["name"] == "SHORT ONE", "console.yaml wins over game.yml")


def test_it_refuses_a_game_that_is_not_there():
    refuses("title: T\n\nmenu:\n  - game: nosuchgame\n",
            "names a game that does not exist")


def test_it_refuses_the_same_game_twice():
    refuses("title: T\n\nmenu:\n  - game: alpha\n  - game: alpha\n",
            "lists one game on two rows")


def test_it_refuses_a_name_the_font_cannot_draw():
    refuses("title: T\n\nmenu:\n  - game: alpha\n    name: CAFÉ RACER\n",
            "uses a character the font has no picture for")


def test_a_name_too_wide_for_its_row_is_kept():
    """The menu slides one, so refusing it would be refusing the feature."""
    long_name = "A VERY LONG GAME NAME INDEED"
    _, entries, _ = build("title: T\n\nmenu:\n  - game: alpha\n"
                          "    name: %s\n" % long_name)
    check(gen_library.name_width(long_name) > gen_library.NAME_ROOM_PX,
          "the fixture is wider than a row, or this test proves nothing")
    check(entries[0]["name"] == long_name,
          "a name wider than its row reaches the menu whole")


def test_it_refuses_a_title_too_wide_for_the_header():
    refuses("title: A CONSOLE WITH A VERY LONG NAME\n\nmenu:\n  - game: alpha\n",
            "would print its title through the battery icon")


def test_it_refuses_an_empty_menu():
    refuses("title: T\n\nmenu:\n  - heading: NOTHING HERE\n",
            "would boot to a menu with no games in it")


def test_it_refuses_another_sdk():
    tmp = tempfile.mkdtemp()
    try:
        games_dir = os.path.join(tmp, "games")
        os.makedirs(os.path.join(games_dir, "raw"))
        with open(os.path.join(games_dir, "raw", "game.yml"), "w",
                  encoding="utf-8") as handle:
            handle.write("slug: raw\ntitle: Raw\nsdk: picosystem\n")
        config = os.path.join(tmp, "console.yaml")
        with open(config, "w", encoding="utf-8") as handle:
            handle.write("title: T\n\nmenu:\n  - game: raw\n")
        try:
            gen_library.build_catalog(config, games_dir)
        except gen_library.LibraryError:
            print("  refused: a game built against another SDK")
            return
        check(False, "a game built against another SDK cannot be linked in")
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def test_the_generated_code_declares_what_it_uses():
    _, entries, slugs = build(BASE)
    source = gen_library.emit_library("TEST", entries)
    for slug in slugs:
        symbol = gen_library.symbol(slug)
        check("PSE_GAME_DECL(%s);" % symbol in source,
              "%s is declared before it is pointed at" % symbol)
        check("&pse_game_%s" % symbol in source,
              "%s is in the table" % symbol)
    stubs = gen_library.emit_stubs(slugs)
    for slug in slugs:
        check("PSE_GAME(%s," % gen_library.symbol(slug) in stubs,
              "the host harness gets a stub for %s" % slug)


def test_a_hyphenated_slug_becomes_an_identifier():
    check(gen_library.symbol("pico-santa") == "pico_santa",
          "a directory name with a hyphen becomes a C symbol")


def test_the_repositorys_own_console_builds():
    """The console this repo ships is held to its own rules."""
    title, entries, slugs = gen_library.build_catalog(
        os.path.join(REPO_ROOT, "console.yaml"),
        os.path.join(REPO_ROOT, "games"))
    check(len(slugs) > 0, "console.yaml lists at least one game")
    check(gen_library.name_width(title) <= gen_library.TITLE_ROOM_PX,
          "the console title fits the header")
    for entry in entries:
        check(entry["name"] != "", "every row has a name to draw")
    for slug in slugs:
        check(os.path.isdir(os.path.join(REPO_ROOT, "games", slug)),
              "%s is a real game directory" % slug)


def main():
    test_a_list_becomes_a_menu()
    test_headings_are_rows_with_no_game()
    test_a_name_can_be_overridden()
    test_it_refuses_a_game_that_is_not_there()
    test_it_refuses_the_same_game_twice()
    test_it_refuses_a_name_the_font_cannot_draw()
    test_a_name_too_wide_for_its_row_is_kept()
    test_it_refuses_a_title_too_wide_for_the_header()
    test_it_refuses_an_empty_menu()
    test_it_refuses_another_sdk()
    test_the_generated_code_declares_what_it_uses()
    test_a_hyphenated_slug_becomes_an_identifier()
    test_the_repositorys_own_console_builds()

    if failures:
        print("gen_library: %d check(s) failed" % len(failures))
        return 1
    print("gen_library: all checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
