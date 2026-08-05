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


def main():
    test_repo_name_loses_its_dashes()
    test_an_empty_title_still_derives_one()
    test_a_given_title_is_left_alone()
    test_held_games_leave_the_shelf_for_the_archive()
    if failures:
        print("gen_gallery: %d check(s) failed" % len(failures))
        return 1
    print("gen_gallery: all checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
