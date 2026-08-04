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

from fingerprint import GameError, discover_games, load_published  # noqa: E402

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

    def render_controls(self):
        controls = self.game.manifest.get("controls") or []
        if not isinstance(controls, list) or not controls:
            return ""
        items = "".join("<span>%s</span>" % escape(c) for c in controls)
        return '<div class="controls">%s</div>' % items

    def render_actions(self):
        actions = []
        if self.game.web and self.is_published:
            # The version query gives every build a distinct page URL, which
            # is what defeats the ten minute Pages cache: a link to the new
            # build can never be satisfied by a cached copy of the old one.
            version = (self.published.get("fingerprint")
                       or self.published.get("commit") or "")
            href = "%s/" % escape(self.game.slug)
            if version:
                href += "?v=%s" % escape(version)
            actions.append('<a class="btn play" data-slug="%s" href="%s">'
                           'Play</a>' % (escape(self.game.slug), href))
        uf2 = self.uf2_href
        if uf2:
            actions.append('<a class="btn" href="%s" download>.uf2</a>'
                           % escape(uf2))
        if not actions:
            return ('<div class="actions">'
                    '<span class="btn" aria-disabled="true">building</span>'
                    '</div>')
        return '<div class="actions">%s</div>' % "".join(actions)

    def render(self):
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
            "    " + self.render_controls(),
            "    " + self.render_actions(),
            "  </div>",
            "</article>",
        ])


def render_page(cards, repo, generated_at, title):
    if cards:
        shelf = ('<div class="shelf">\n%s\n</div>'
                 % "\n".join(card.render() for card in cards))
    else:
        shelf = '<div class="empty">No games published yet</div>'

    playable = sum(1 for c in cards if c.game.web and c.is_published)
    stats = "%d game%s &middot; %d playable" % (
        len(cards), "" if len(cards) == 1 else "s", playable)

    repo_url = "https://github.com/%s" % repo if repo else "#"

    return """<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>%(title)s</title>
<meta name="description" content="PicoSystem games, built and published on every push.">
<link rel="stylesheet" href="gallery.css">
<link rel="icon" href="data:image/svg+xml,<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 16 16'><text y='14' font-size='14'>&#127918;</text></svg>">
</head>
<body>
<header class="marquee">
  <h1>%(title)s</h1>
  <p>PicoSystem games, built and published on every push</p>
  <div class="stats">%(stats)s</div>
</header>

<main class="wrap">
%(shelf)s

  <footer class="shelf-footer">
    <span>Flash a .uf2: hold <kbd>X</kbd>, press power, drop the file on <kbd>RPI-RP2</kbd>.</span>
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
        if (!g.fingerprint) return;
        var a = document.querySelector('a.play[data-slug="' + g.slug + '"]');
        if (a) a.href = g.slug + '/?v=' + g.fingerprint;
      });
    })
    .catch(function () {});
})();
</script>
</body>
</html>
""" % {
        "shelf": shelf,
        "stats": stats,
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
                        help="gallery heading. Defaults to the repo name, so "
                             "renaming the repo does not mean editing the "
                             "generator.")
    parser.add_argument("--timestamp", default="")
    args = parser.parse_args(argv)

    try:
        games = discover_games()
    except GameError as error:
        sys.stderr.write("gen_gallery: %s\n" % error)
        return 1

    published = load_published(args.manifest)
    cards = [Card(game, published.get(game.slug), args.site) for game in games]

    os.makedirs(args.site, exist_ok=True)
    index_path = os.path.join(args.site, "index.html")
    with open(index_path, "w", encoding="utf-8") as handle:
        title = args.title or (args.repo.split("/")[-1] if args.repo
                               else "games")
        handle.write(render_page(cards, args.repo, args.timestamp, title))

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
