#!/usr/bin/env python3
"""What the mockups index promises, and what every mockup owes it.

The index is generated from each mockup's own page, so there is no second copy
of a title or a pitch to go stale. The price is that a mockup has to carry
both, and the last test here is the one that keeps that true: it walks every
mockup in the repo rather than a fixture, so a new mockup with no description
fails the build instead of shipping a card with nothing on it.

Usage:
    test_gen_mockups.py
"""

import os
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
TOOLS = os.path.dirname(HERE)
REPO_ROOT = os.path.dirname(TOOLS)
sys.path.insert(0, TOOLS)

import gen_mockups  # noqa: E402
from gen_mockups import MockupError, Mockup  # noqa: E402

failures = []


def check(ok, what):
    if not ok:
        failures.append(what)
        print("FAIL %s" % what)


def write(directory, body):
    os.makedirs(directory, exist_ok=True)
    with open(os.path.join(directory, "index.html"), "w",
              encoding="utf-8") as handle:
        handle.write(body)
    return directory


PAGE = ("<!doctype html><html><head><title>%s</title>%s</head>"
        "<body>%s</body></html>")


with tempfile.TemporaryDirectory() as tmp:
    # The title is what the card is called, and the "// what it is" half is
    # the same sentence on every page, so it is dropped.
    d = write(os.path.join(tmp, "alpha"),
              PAGE % ("Alpha // 240x240 mockup",
                      '<meta name="description" content="A thing.">', ""))
    m = Mockup(d)
    check(m.name == "Alpha", "the marquee half of the title is the name")
    check(m.blurb == "A thing.", "a meta description is used verbatim")
    check(m.slug == "alpha", "the slug is the directory")

    # A trailing "mockup" goes too: the page it is listed on is called Mockups.
    d = write(os.path.join(tmp, "beta"),
              PAGE % ("Beta mockup",
                      '<meta name="description" content="Another.">', ""))
    check(Mockup(d).name == "Beta", "a trailing 'mockup' is dropped")

    # No description: fall back to the lede, trimmed to its first sentence.
    d = write(os.path.join(tmp, "gamma"),
              PAGE % ("Gamma", "",
                      '<p class="lede">First one. Second one, which is not '
                      'wanted on a card.</p>'))
    check(Mockup(d).blurb == "First one.",
          "the lede is trimmed to its first sentence")

    # A full stop inside a token is not the end of a sentence. Cutting on any
    # full stop would make this "A 240x240" and read as a truncation bug.
    d = write(os.path.join(tmp, "delta"),
              PAGE % ("Delta", "",
                      '<p class="lede">A 240x240 page using pse::Texture.</p>'))
    check(Mockup(d).blurb == "A 240x240 page using pse::Texture.",
          "a full stop with no space after it does not end the sentence")

    # Markup inside the lede is text, not tags.
    d = write(os.path.join(tmp, "epsilon"),
              PAGE % ("Epsilon", "",
                      '<p class="lede">Uses <code>row_stride</code> to '
                      'window.</p>'))
    check(Mockup(d).blurb == "Uses row_stride to window.",
          "tags inside the lede are stripped")

    # Neither: a hard error, because a card with no line on it is worse than a
    # build that stops. Built in its own directory, since leaving a broken
    # mockup in `tmp` would make every discovery below throw.
    with tempfile.TemporaryDirectory() as bad:
        d = write(os.path.join(bad, "zeta"), PAGE % ("Zeta", "", "<p>x</p>"))
        try:
            Mockup(d)
            check(False, "a mockup with no description is an error")
        except MockupError:
            check(True, "a mockup with no description is an error")

        # And discovery propagates it rather than quietly dropping the mockup,
        # because a mockup missing from the index is the failure nobody sees.
        try:
            gen_mockups.discover(bad)
            check(False, "discovery propagates a broken mockup")
        except MockupError:
            check(True, "discovery propagates a broken mockup")

        # A broken mockup stops the generator, and with a message naming it.
        code = gen_mockups.main(["--source", bad,
                                 "--out", os.path.join(bad, "out")])
        check(code == 1, "the generator exits non zero on a broken mockup")

    # The scan stops at the first <script>, so a page cannot be described by
    # its own program. Both of these would win if the whole file were searched.
    d = write(os.path.join(tmp, "eta"),
              PAGE % ("Eta", '<meta name="description" content="Real.">',
                      '<script>var s = "<title>Decoy</title>";'
                      'var t = \'<meta name="description" content="Fake.">\';'
                      '</script>'))
    m = Mockup(d)
    check(m.name == "Eta" and m.blurb == "Real.",
          "a title or description in a script literal is not picked up")

    # Discovery is by directory, in name order, and a directory with no page
    # is not a mockup.
    os.makedirs(os.path.join(tmp, "notamockup"), exist_ok=True)
    with open(os.path.join(tmp, "loose.txt"), "w") as handle:
        handle.write("x")
    slugs = [m.slug for m in gen_mockups.discover(tmp)]
    check(slugs == sorted(slugs), "discovery is in name order")
    check("notamockup" not in slugs, "a directory with no index.html is skipped")
    check("alpha" in slugs and "eta" in slugs, "every page directory is found")

    # A missing source directory is empty, not a crash: the repo is allowed to
    # have no mockups, and the workflow skips the step rather than failing.
    check(gen_mockups.discover(os.path.join(tmp, "nope")) == [],
          "a missing source directory yields nothing")

    page = gen_mockups.render_page(gen_mockups.discover(tmp), "owner/name", "t")
    check('href="alpha/"' in page, "the index links each mockup by slug")
    check('href="../"' in page, "the index links back to the games")
    check('href="../gallery.css"' in page,
          "the index uses the gallery stylesheet one level up")


# The rule, applied to the repo rather than to a fixture. This is the check
# that makes the index trustworthy: it fails the build for a mockup that
# cannot be listed, at the point the mockup is added.
real = os.path.join(REPO_ROOT, "mockups")
if os.path.isdir(real):
    for entry in sorted(os.listdir(real)):
        directory = os.path.join(real, entry)
        if not os.path.isfile(os.path.join(directory, "index.html")):
            continue
        try:
            m = Mockup(directory)
        except MockupError as error:
            check(False, "mockups/%s: %s" % (entry, error))
            continue
        check(bool(m.name), "mockups/%s has a name" % entry)
        check(bool(m.blurb), "mockups/%s has a description" % entry)
        # A card is one line. Anything much longer is a paragraph that got in.
        check(len(m.blurb) <= 240,
              "mockups/%s description is %d chars, which is not a line"
              % (entry, len(m.blurb)))

    # The mockups live at /mockups/<slug>/ and games live at /<slug>/, so the
    # two only collide on one name. A game called "mockups" would have its
    # published directory overwritten by the copy step and nothing would say
    # so, which is cheap to rule out and expensive to debug.
    sys.path.insert(0, TOOLS)
    from build_plan import discover_games  # noqa: E402
    check(all(game.slug != "mockups" for game in discover_games()),
          "no game claims the slug 'mockups', which the index sits on")

if failures:
    print("\n%d check(s) failed" % len(failures))
    sys.exit(1)
print("test_gen_mockups: all checks pass")
