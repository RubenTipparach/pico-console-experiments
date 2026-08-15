#!/usr/bin/env python3
"""Render the gallery index from the published build manifest.

The gallery is generated, never hand edited, because adding a game must not
require touching shared files. Everything shown comes from each game's
`game.yml` plus the `builds.json` that the publish step maintains.

Usage:
    gen_gallery.py --manifest site/builds.json --site site --repo owner/name
"""

import argparse
import html
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from build_plan import (GameError, discover_games, load_config,  # noqa: E402
                        load_published)

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def escape(value):
    return html.escape(str(value), quote=True)


class Card:
    """One game as the gallery sees it. Knows how to render itself and nothing
    about how the page around it is assembled."""

    def __init__(self, game, published, site_dir):
        self.game = game
        self.published = published or {}
        self.site_dir = site_dir

    @property
    def is_published(self):
        return os.path.isfile(
            os.path.join(self.site_dir, self.game.slug, "index.html"))

    @property
    def thumbnail_href(self):
        candidate = os.path.join(self.site_dir, "thumbs",
                                 "%s.png" % self.game.slug)
        return "thumbs/%s.png" % self.game.slug if os.path.isfile(candidate) else None

    @property
    def uf2_href(self):
        candidate = os.path.join(self.site_dir, "uf2", "%s.uf2" % self.game.slug)
        return "uf2/%s.uf2" % self.game.slug if os.path.isfile(candidate) else None

    @property
    def tufty_uf2_href(self):
        """The Tufty 2350 build, when one was published.

        Same existence test as uf2_href and for the same reason: a card offers
        what is actually on the site, never what the config says should be. A
        run that built only the PicoSystem, or a game carried forward from the
        state branch before the Tufty existed, has no file here and gets no
        button, rather than a link to a 404."""
        candidate = os.path.join(self.site_dir, "uf2-tufty",
                                 "%s.uf2" % self.game.slug)
        return ("uf2-tufty/%s.uf2" % self.game.slug
                if os.path.isfile(candidate) else None)

    def render_screen(self):
        thumb = self.thumbnail_href
        if thumb:
            return ('<div class="screen">'
                    '<img src="%s" alt="%s gameplay" width="240" height="240" '
                    'loading="lazy"></div>'
                    % (escape(thumb), escape(self.game.title)))
        return ('<div class="screen"><div class="pending">'
                '<svg width="28" height="28" viewBox="0 0 16 16" fill="none" '
                'stroke="currentColor" stroke-width="1.5" aria-hidden="true">'
                '<rect x="1" y="3.5" width="14" height="10"/>'
                '<circle cx="8" cy="8.5" r="2.5"/></svg>'
                '<span>no capture yet</span></div></div>')

    def render_actions(self, base=""):
        """base prefixes every link, for pages that are not at the site root."""
        actions = []
        if self.game.web and self.is_published:
            # The version query gives every build a distinct page URL, which
            # is what defeats the ten minute Pages cache: a link to the new
            # build can never be satisfied by a cached copy of the old one.
            version = self.published.get("commit") or ""
            href = "%s%s/" % (base, escape(self.game.slug))
            if version:
                href += "?v=%s" % escape(version)
            actions.append('<a class="btn play" data-slug="%s" href="%s">'
                           'Play</a>' % (escape(self.game.slug), href))
        # One board or two, and the card says which only when it has to.
        #
        # With a single build there is nothing to disambiguate, so the button
        # stays the `.uf2` it has always been: naming the board on a shelf
        # that only ships one is a question nobody asked. With two, the label
        # becomes the board, because "which of these do I want" is then a real
        # question and the file name does not answer it.
        #
        # A device only game spells `.uf2` out on both, since it has no Play
        # button to make the row obviously a set of downloads.
        uf2 = self.uf2_href
        tufty = self.tufty_uf2_href
        if uf2 and tufty:
            suffix = "" if actions else " .uf2"
            for label, href in (("Pico", uf2), ("Tufty", tufty)):
                actions.append(
                    '<a class="btn small" href="%s%s" download>%s%s</a>'
                    % (base, escape(href), label, suffix))
        elif uf2 or tufty:
            actions.append('<a class="btn" href="%s%s" download>.uf2</a>'
                           % (base, escape(uf2 or tufty)))
        if not actions:
            return ('<div class="actions">'
                    '<span class="btn" aria-disabled="true">building</span>'
                    '</div>')
        return '<div class="actions">%s</div>' % "".join(actions)

    def render_row(self, base=""):
        """A line on the archive page: what the game was, and how to run it.

        No screenshot. A shelf of cartridges is for choosing between things;
        this is a list of what is no longer in the rotation, and a picture of
        each would say the opposite."""
        tag = ('<span class="tag web">web</span>' if self.game.web
               else '<span class="tag device">device</span>')
        return "\n".join([
            '<li class="archived-row">',
            '  <div class="row-head"><h2>%s</h2>%s</div>'
            % (escape(self.game.title), tag),
            '  <p class="blurb">%s</p>'
            % escape(self.game.manifest.get("blurb") or ""),
            "  " + self.render_actions(base),
            "</li>",
        ])

    def render(self):
        """A picture, a name, a line about it, and a way in.

        The card used to print every control the game has under the blurb, one
        boxed line each. It is the wrong page for them twice over. It is a
        SHELF: the question a card answers is "do I want to play this", and a
        list of six keys is not an answer to that, it is a manual for a game
        you have not chosen yet. And it made the shelf ragged, because a card's
        height became a function of how many buttons its game happens to use,
        so a three control game sat next to a six control game with a hole
        under it.

        The controls have a better home now. Rule 12's mini tutorial puts them
        on the game's own page, beside what each one does, in front of the
        player at the moment they are about to press one.
        """
        tag = ('<span class="tag web">web</span>' if self.game.web
               else '<span class="tag device">device</span>')
        return "\n".join([
            '<article class="cart">',
            self.render_screen(),
            '  <div class="body">',
            '    <div class="title-row"><h2>%s</h2>%s</div>'
            % (escape(self.game.title), tag),
            '    <p class="blurb">%s</p>'
            % escape(self.game.manifest.get("blurb") or ""),
            "    " + self.render_actions(),
            "  </div>",
            "</article>",
        ])


def count_mockups(site_dir):
    """How many mockups are on the assembled site.

    Counted off the site rather than off the repo, so the door on the front
    page only appears when gen_mockups actually wrote a page behind it. A link
    to a 404 is worse than no link, and the two can disagree: the mockups are
    copied by the publish job, which a preview build or a hand assembled tree
    may not have run.
    """
    root = os.path.join(site_dir, "mockups")
    if not os.path.isfile(os.path.join(root, "index.html")):
        return 0
    return sum(1 for entry in os.listdir(root)
               if os.path.isfile(os.path.join(root, entry, "index.html")))


def render_page(cards, repo, generated_at, title, active_cards=None,
                mockups=0):
    if active_cards is None:
        active_cards = cards
        archived_cards = []
    else:
        archived_cards = [c for c in cards if c not in active_cards]

    if active_cards:
        shelf = ('<div class="shelf">\n%s\n</div>'
                 % "\n".join(card.render() for card in active_cards))
    else:
        shelf = '<div class="empty">No games published yet</div>'

    # Held games get a door rather than a shelf of their own. They are not
    # choices any more, so putting their screenshots under the live ones asked
    # the front page to sell something that is not for sale.
    archived_section = ""
    if archived_cards:
        archived_section = (
            '\n  <p class="archive-link"><a class="btn" href="archived/">'
            'Archive &rarr;</a> <span>%d game%s no longer in the rotation'
            '</span></p>' % (len(archived_cards),
                             "" if len(archived_cards) == 1 else "s"))

    # Mockups get a door too, and for the same reason: they are not games and
    # a shelf of them under the live ones would offer something that cannot be
    # played. The count comes from what was actually indexed, so this link
    # cannot appear pointing at a page that is not there.
    if mockups:
        archived_section += (
            '\n  <p class="archive-link"><a class="btn" href="mockups/">'
            'Mockups &rarr;</a> <span>%d design%s argued before any code'
            '</span></p>' % (mockups, "" if mockups == 1 else "s"))

    playable = sum(1 for c in active_cards if c.game.web and c.is_published)
    stats = "%d game%s &middot; %d playable" % (
        len(active_cards), "" if len(active_cards) == 1 else "s", playable)

    repo_url = "https://github.com/%s" % repo if repo else "#"

    return """<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>%(title)s</title>
<meta name="description" content="Games for the PicoSystem and the Tufty 2350, built and published on every push.">
<link rel="stylesheet" href="gallery.css">
<link rel="icon" href="data:image/svg+xml,<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 16 16'><text y='14' font-size='14'>&#127918;</text></svg>">
</head>
<body>
<header class="marquee">
  <h1>%(title)s</h1>
  <p>PicoSystem and Tufty 2350, built and published on every push</p>
  <div class="stats">%(stats)s</div>
</header>

<main class="wrap">
%(shelf)s
%(archived_section)s

  <footer class="shelf-footer">
    <span>PicoSystem: hold <kbd>X</kbd>, press power, drop the file on <kbd>RPI-RP2</kbd>.</span>
    <span>Tufty: hold <kbd>BOOT</kbd>, tap <kbd>RESET</kbd>, drop it on <kbd>RP2350</kbd>. <a href="https://github.com/pimoroni/tufty2350/releases/latest">Back to badge OS</a></span>
    <span><a href="%(repo_url)s">Source</a></span>
    <span>Built %(generated_at)s</span>
  </footer>
</main>
<script>
(function () {
  // This page itself sits behind the ten minute Pages cache, so its links
  // can lag one build behind. The manifest fetched with a unique query is
  // always fresh; rewriting the links from it means opening a game from
  // here always gets the newest build, however stale this page is.
  fetch('builds.json?_=' + Date.now())
    .then(function (r) { return r.json(); })
    .then(function (m) {
      (m.games || []).forEach(function (g) {
        var v = g.commit || g.fingerprint;
        if (!v) return;
        var a = document.querySelector('a.play[data-slug="' + g.slug + '"]');
        if (a) a.href = g.slug + '/?v=' + v;
      });
    })
    .catch(function () {});
})();
</script>
</body>
</html>
""" % {
        "shelf": shelf,
        "archived_section": archived_section,
        "stats": stats,
        "repo_url": escape(repo_url),
        "generated_at": escape(generated_at),
        "title": escape(title),
    }


def render_archive_page(cards, repo, generated_at, title):
    """The archive: what used to be in the rotation, as a list.

    No screenshots by design. The shelf on the front page is for choosing
    between games; this is a record of the ones that are done, and giving each
    a picture would make it read as a second shelf."""
    rows = "\n".join(card.render_row("../") for card in cards)
    repo_url = "https://github.com/%s" % repo if repo else "#"

    return """<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Archive &middot; %(title)s</title>
<meta name="description" content="Games no longer in the build rotation.">
<link rel="stylesheet" href="../gallery.css">
<link rel="icon" href="data:image/svg+xml,<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 16 16'><text y='14' font-size='14'>&#127918;</text></svg>">
</head>
<body>
<header class="marquee">
  <h1>Archive</h1>
  <p>Out of the rotation, still playable</p>
</header>

<main class="wrap">
  <p class="archive-link"><a class="btn" href="../">&larr; Back to the shelf</a></p>

  <ul class="archive-list">
%(rows)s
  </ul>

  <footer class="shelf-footer">
    <span>These are held in <code>build.yaml</code>: not rebuilt, last build kept.</span>
    <span><a href="%(repo_url)s">Source</a></span>
    <span>Built %(generated_at)s</span>
  </footer>
</main>
</body>
</html>
""" % {
        "rows": rows,
        "repo_url": escape(repo_url),
        "generated_at": escape(generated_at),
        "title": escape(title),
    }


def main(argv):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--site", required=True, help="publish root")
    parser.add_argument("--manifest", help="path to builds.json")
    parser.add_argument("--repo", default="", help="owner/name, for the link")
    parser.add_argument("--title", default="",
                        help="gallery heading. Defaults to the repo name with "
                             "its dashes read as spaces, so renaming the repo "
                             "does not mean editing the generator.")
    parser.add_argument("--timestamp", default="")
    args = parser.parse_args(argv)

    try:
        games = discover_games()
    except GameError as error:
        sys.stderr.write("gen_gallery: %s\n" % error)
        return 1

    published = load_published(args.manifest)
    cards = [Card(game, published.get(game.slug), args.site) for game in games]

    # Held games are archived. build_plan owns reading build.yaml, including
    # the checks that a held slug actually exists: doing it here with a second
    # parser meant a typo under `hold` silently left the game on the shelf.
    try:
        _, held, _ = load_config([game.slug for game in games])
    except GameError as error:
        sys.stderr.write("gen_gallery: %s\n" % error)
        return 1

    active_cards = [c for c in cards if c.game.slug not in held]

    os.makedirs(args.site, exist_ok=True)
    index_path = os.path.join(args.site, "index.html")
    with open(index_path, "w", encoding="utf-8") as handle:
        # A repo name is hyphenated because a path has to be; a sign over a
        # cabinet is not. Dashes read as spaces so the marquee says what the
        # place is called rather than where it lives.
        title = args.title or (args.repo.split("/")[-1].replace("-", " ")
                               if args.repo else "games")
        handle.write(render_page(cards, args.repo, args.timestamp, title,
                                 active_cards, count_mockups(args.site)))

    # The archive is its own page so the front page can stay a shelf of what
    # is live. Written under archived/ rather than as archived.html so the
    # link is a directory and the URL has no extension in it.
    archived_cards = [c for c in cards if c not in active_cards]
    if archived_cards:
        archive_dir = os.path.join(args.site, "archived")
        os.makedirs(archive_dir, exist_ok=True)
        with open(os.path.join(archive_dir, "index.html"), "w",
                  encoding="utf-8") as handle:
            handle.write(render_archive_page(archived_cards, args.repo,
                                             args.timestamp, title))

    # Jekyll would otherwise eat any path starting with an underscore, which
    # emscripten output can contain.
    with open(os.path.join(args.site, ".nojekyll"), "w") as handle:
        handle.write("")

    css_source = os.path.join(REPO_ROOT, "web", "gallery.css")
    with open(css_source, "r", encoding="utf-8") as src:
        with open(os.path.join(args.site, "gallery.css"), "w",
                  encoding="utf-8") as dst:
            dst.write(src.read())

    sys.stderr.write("gen_gallery: %d card(s) -> %s\n" % (len(cards), index_path))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
