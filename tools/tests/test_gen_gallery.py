#!/usr/bin/env python3
"""What the gallery generator promises about the page it writes.

The marquee is the sign over the cabinet, so it gets the repo's name with its
dashes read as spaces. That default lived in the generator while the workflow
computed its own title and passed it in, which made the default dead code and
put `pico-console-experiments` back on the front page. So the contract is
pinned here in both directions: derived titles are humanised, given titles are
used exactly, because a preview shows a branch name and a branch name is an
identifier.

Usage:
    test_gen_gallery.py
"""

import os
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
TOOLS = os.path.dirname(HERE)
REPO_ROOT = os.path.dirname(TOOLS)
GEN = os.path.join(TOOLS, "gen_gallery.py")

failures = []


def check(ok, what):
    if not ok:
        failures.append(what)
        print("FAIL %s" % what)


def render(site, extra):
    """Run the generator the way the workflow does and return the page."""
    subprocess.run(
        [sys.executable, GEN, "--site", site,
         "--repo", "RubenTipparach/pico-console-experiments",
         "--timestamp", "2026-01-01T00:00:00Z"] + extra,
        cwd=REPO_ROOT, check=True, capture_output=True)
    with open(os.path.join(site, "index.html"), encoding="utf-8") as handle:
        return handle.read()


def test_repo_name_loses_its_dashes():
    site = tempfile.mkdtemp()
    try:
        page = render(site, [])
        check("<h1>pico console experiments</h1>" in page,
              "the derived title reads as words, not a path")
        check("pico-console-experiments</h1>" not in page,
              "no dashes survive into the heading")
    finally:
        shutil.rmtree(site, ignore_errors=True)


def test_an_empty_title_still_derives_one():
    """The workflow passes --title "" for the main site. An empty string has
    to fall through to the derived name rather than render a blank sign."""
    site = tempfile.mkdtemp()
    try:
        page = render(site, ["--title", ""])
        check("<h1>pico console experiments</h1>" in page,
              "an empty title falls back to the derived name")
    finally:
        shutil.rmtree(site, ignore_errors=True)


def test_a_given_title_is_left_alone():
    """Previews are titled by branch, and a branch name is an identifier: it
    has to survive verbatim, dashes and all."""
    site = tempfile.mkdtemp()
    try:
        page = render(site, ["--title", "claude/game-thumbnails (preview)"])
        check("<h1>claude/game-thumbnails (preview)</h1>" in page,
              "an explicit title is used exactly as given")
    finally:
        shutil.rmtree(site, ignore_errors=True)


def test_held_games_leave_the_shelf_for_the_archive():
    """The front page is a shelf of what is live. Held games are not on it at
    all: they get a link to their own page and nothing more."""
    site = tempfile.mkdtemp()
    try:
        page = render(site, [])
        for held in ("Chicken", "Pico Santa"):
            check(held not in page, "%s is off the front page" % held)
        for active in ("Dust Rider", "Kingfisher"):
            check(active in page, "%s is on the shelf" % active)
        check('href="archived/"' in page, "the archive has a door")
        check("2 games" in page, "the stats count only the shelf")

        archive = os.path.join(site, "archived", "index.html")
        check(os.path.isfile(archive), "the archive page is written")
        with open(archive, encoding="utf-8") as handle:
            listing = handle.read()
        for held in ("Chicken", "Pico Santa"):
            check(held in listing, "%s is listed in the archive" % held)
        for active in ("Dust Rider", "Kingfisher"):
            check(active not in listing, "%s is not in the archive" % active)

        # A list, not a shelf: no screenshots, and every link has to climb out
        # of archived/ to reach the site root.
        check("<img" not in listing, "the archive shows no screenshots")
        check('href="../"' in listing, "the archive links back to the shelf")
        check('href="../gallery.css"' in listing,
              "the archive finds the stylesheet from its subdirectory")
        for bad in ('href="chicken/', 'href="pico-santa/'):
            check(bad not in listing,
                  "archive game links are not relative to the wrong root")
    finally:
        shutil.rmtree(site, ignore_errors=True)


def test_a_card_is_a_picture_a_name_and_a_line():
    """The shelf answers "do I want to play this", and nothing else.

    Cards used to carry every control the game has, one boxed line each under
    the blurb. That is a manual for a game you have not chosen yet, and it made
    the shelf ragged: a card's height became a function of how many buttons its
    game happens to use. The controls are on the game's own page now, beside
    what each does.

    Checked against the real game.yml strings rather than a fixture, because
    the failure to catch is a control string turning up on the front page, and
    only the real ones can do that.
    """
    site = tempfile.mkdtemp()
    try:
        page = render(site, [])
        check("Outrun the screen through cactus country." in page,
              "the blurb, which is the description, is on the card")
        check("Dust Rider" in page, "and so is the title")

        # Every control of every game, from the manifests themselves. A card
        # that starts printing one of these again fails.
        #
        # The WHOLE control string, as the card used to escape it, not the half
        # after the colon: the first version of this looked for the meaning on
        # its own and reported that the shelf was printing chicken's "jump",
        # which is a substring of kingfisher's blurb, and pico-santa's "turn",
        # which is a substring of "return" in the page's own script.
        sys.path.insert(0, TOOLS)
        from build_plan import discover_games  # noqa: E402
        from gen_gallery import escape  # noqa: E402
        measured = 0
        for game in discover_games():
            for control in game.manifest.get("controls") or []:
                if not str(control).strip():
                    continue
                measured += 1
                check(escape(str(control)) not in page,
                      "%s's card does not print \"%s\""
                      % (game.slug, control))
        check(measured > 10, "there were controls to look for (%d)" % measured)
        check('class="controls"' not in page, "and no empty box left behind")
    finally:
        shutil.rmtree(site, ignore_errors=True)


def test_a_card_names_the_board_only_when_there_are_two():
    """One build is a download. Two builds are a choice, and only then a label.

    The card reads the site rather than the config, because the two go out of
    step in both directions: a game held back before the Tufty existed keeps
    the PicoSystem binary the state branch carried for it and has no Tufty file
    at all, and a run can publish one board and not the other. Offering a
    button for a file that is not there is a 404 on the front page, which is
    worse than a card with one button.

    The labels are checked in the actions, not in the page, because the footer
    now names both boards too and "Tufty is somewhere on this page" would pass
    on a card that never gained a button.
    """
    slug = "dustrider"
    site = tempfile.mkdtemp()
    try:
        os.makedirs(os.path.join(site, "uf2"))
        with open(os.path.join(site, "uf2", "%s.uf2" % slug), "wb") as handle:
            handle.write(b"\0")

        page = render(site, [])
        check("uf2/%s.uf2" % slug in page, "the one board that exists is offered")
        check("uf2-tufty/%s.uf2" % slug not in page,
              "and the board that does not is not")
        check(">.uf2<" in page,
              "with one build the button is the .uf2 it has always been")

        os.makedirs(os.path.join(site, "uf2-tufty"))
        with open(os.path.join(site, "uf2-tufty", "%s.uf2" % slug), "wb") as f:
            f.write(b"\0")

        page = render(site, [])
        check("uf2/%s.uf2" % slug in page, "both boards are offered")
        check("uf2-tufty/%s.uf2" % slug in page, "including the Tufty")
        check(">Pico" in page, "and the buttons name the boards")
        check(">Tufty" in page, "both of them")
        # The row must not grow a third download or wrap onto a second line.
        check(page.count("uf2-tufty/%s.uf2" % slug) == 1,
              "one Tufty button per card, not one per action row")
    finally:
        shutil.rmtree(site, ignore_errors=True)


def test_this_test_would_notice_a_missing_button():
    """A card assertion that cannot fail is worse than no assertion."""
    slug = "dustrider"
    site = tempfile.mkdtemp()
    try:
        # No uf2 directory at all: neither board, so neither href.
        page = render(site, [])
        check("uf2/%s.uf2" % slug not in page,
              "with nothing published there is no download to offer")
        check("uf2-tufty/%s.uf2" % slug not in page,
              "and no Tufty one either")
    finally:
        shutil.rmtree(site, ignore_errors=True)


def test_the_mockups_door_follows_the_site_not_the_repo():
    """The front page links the mockups only when a page is there to link.

    The repo always has mockups; the assembled site only has them once the
    publish job has copied them, which a preview or a hand assembled tree may
    not have done. Reading the repo here would put a door on the page with a
    404 behind it."""
    site = tempfile.mkdtemp()
    try:
        page = render(site, [])
        check('href="mockups/"' not in page,
              "no door when the site has no mockups")

        os.makedirs(os.path.join(site, "mockups", "alpha"))
        os.makedirs(os.path.join(site, "mockups", "beta"))
        for path in (("mockups", "index.html"),
                     ("mockups", "alpha", "index.html"),
                     ("mockups", "beta", "index.html")):
            with open(os.path.join(site, *path), "w") as handle:
                handle.write("<html></html>")
        page = render(site, [])
        check('href="mockups/"' in page, "a door once the pages are there")
        check("2 designs" in page, "the door counts the mockups it found")

        # An index with nothing behind it still counts nothing, and a
        # directory with no page of its own is not a mockup.
        os.makedirs(os.path.join(site, "mockups", "notamockup"))
        page = render(site, [])
        check("2 designs" in page,
              "a directory with no index.html is not counted")
    finally:
        shutil.rmtree(site, ignore_errors=True)


def main():
    test_repo_name_loses_its_dashes()
    test_an_empty_title_still_derives_one()
    test_a_given_title_is_left_alone()
    test_held_games_leave_the_shelf_for_the_archive()
    test_a_card_is_a_picture_a_name_and_a_line()
    test_a_card_names_the_board_only_when_there_are_two()
    test_this_test_would_notice_a_missing_button()
    test_the_mockups_door_follows_the_site_not_the_repo()
    if failures:
        print("gen_gallery: %d check(s) failed" % len(failures))
        return 1
    print("gen_gallery: all checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
