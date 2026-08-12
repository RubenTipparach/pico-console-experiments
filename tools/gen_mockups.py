#!/usr/bin/env python3
"""Render the index for the design mockups the repo carries.

A mockup is a self contained HTML page under `mockups/<slug>/index.html` that
argues for a design before anyone writes C++, which is CLAUDE.md rule 10. They
were repo only, so the only way to look at one was to clone the repo and open a
file, which is exactly the audience a mockup does not have: the point of a
mockup is to be sent to somebody and argued about.

The index is generated, never hand edited, for the same reason the gallery is
(rule 7, open/closed): adding a mockup must be adding a directory and nothing
else. Everything shown is read out of the mockup's own page, so there is no
second copy of its title or its pitch to go stale.

Usage:
    gen_mockups.py --source mockups --out site/mockups --repo owner/name
"""

import argparse
import html
import os
import re
import sys

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

TITLE_RE = re.compile(r"<title>(.*?)</title>", re.I | re.S)
DESC_RE = re.compile(
    r"""<meta\s+name=["']?description["']?\s+content=["'](.*?)["']\s*/?>""",
    re.I | re.S)
LEDE_RE = re.compile(r"""<p\s+class=["']lede["']\s*>(.*?)</p>""", re.I | re.S)
TAG_RE = re.compile(r"<[^>]+>")
SCRIPT_RE = re.compile(r"<script", re.I)


def escape(value):
    return html.escape(str(value), quote=True)


def markup_of(page):
    """Everything before the first <script>.

    A mockup is one file with its whole simulation inlined, so jokerreels is
    97 KB of JavaScript that mentions `<title>` in a string literal and
    twinflare builds markup in JS. Searching the whole file could pick a
    decoy out of a program; searching a fixed first few KB misses the real
    thing, because picomon's lede is 12 KB in. Every page here puts its
    markup first and its script last, so the cut is where the markup ends.
    """
    match = SCRIPT_RE.search(page)
    return page[:match.start()] if match else page


def text_of(fragment):
    """Strip tags and collapse whitespace, the way a card wants it."""
    return " ".join(html.unescape(TAG_RE.sub(" ", fragment)).split())


def first_sentence(text, limit=220):
    """The lede is a paragraph and a card is a line, so take the first
    sentence. Cut on a full stop followed by a space, not on any full stop:
    `240x240` and `pse::Texture` both contain one."""
    match = re.search(r"(?<=[.!?])\s", text)
    if match and match.start() <= limit:
        return text[:match.start()]
    if len(text) <= limit:
        return text
    cut = text.rfind(" ", 0, limit)
    return text[:cut if cut > 0 else limit].rstrip(" ,;:") + "..."


class MockupError(Exception):
    pass


class Mockup:
    """One mockup as the index sees it, read from its own page."""

    def __init__(self, directory):
        self.directory = directory
        self.slug = os.path.basename(directory.rstrip(os.sep))
        page = os.path.join(directory, "index.html")
        if not os.path.isfile(page):
            raise MockupError("%s has no index.html" % self.slug)
        with open(page, encoding="utf-8", errors="replace") as handle:
            head = markup_of(handle.read())

        title = TITLE_RE.search(head)
        if not title or not text_of(title.group(1)):
            raise MockupError(
                "%s has no <title>, so the index has nothing to call it"
                % self.slug)
        self.title = text_of(title.group(1))

        # A description is written for this exact job, so it wins. A lede is
        # written for the page itself, so it is trimmed to a line.
        desc = DESC_RE.search(head)
        if desc and text_of(desc.group(1)):
            self.blurb = text_of(desc.group(1))
        else:
            lede = LEDE_RE.search(head)
            if not lede or not text_of(lede.group(1)):
                raise MockupError(
                    "%s has neither a meta description nor a p.lede, so the "
                    "index has nothing to say about it" % self.slug)
            self.blurb = first_sentence(text_of(lede.group(1)))

        # The name is the marquee half of the title. Every page here is called
        # "Thing // what it is", and the right hand side is the same sentence
        # on every card, which is no information at all. A trailing "mockup"
        # goes too: the page it is on is called Mockups.
        name = self.title.split("//")[0].strip()
        name = re.sub(r"\s+mockup$", "", name, flags=re.I).strip()
        self.name = name or self.title

    @property
    def has_readme(self):
        return os.path.isfile(os.path.join(self.directory, "README.md"))

    def render(self):
        return (
            '  <li class="mockup">\n'
            '    <h2><a href="%(slug)s/">%(name)s</a></h2>\n'
            '    <p>%(blurb)s</p>\n'
            '    <p class="meta"><a class="btn" href="%(slug)s/">Open '
            '&rarr;</a></p>\n'
            '  </li>' % {
                "slug": escape(self.slug),
                "name": escape(self.name),
                "blurb": escape(self.blurb),
            })


def discover(source):
    """Every directory with an index.html, in name order. A directory is a
    mockup: there is no list to keep in step."""
    found = []
    if not os.path.isdir(source):
        return found
    for entry in sorted(os.listdir(source)):
        directory = os.path.join(source, entry)
        if not os.path.isdir(directory):
            continue
        if not os.path.isfile(os.path.join(directory, "index.html")):
            continue
        found.append(Mockup(directory))
    return found


def render_page(mockups, repo, generated_at):
    if mockups:
        body = ('<ul class="mockups">\n%s\n</ul>'
                % "\n".join(m.render() for m in mockups))
    else:
        body = '<div class="empty">No mockups yet</div>'
    repo_url = "https://github.com/%s" % repo if repo else "#"
    return """<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Mockups</title>
<meta name="description" content="Design mockups: playable arguments for games that are not built yet.">
<link rel="stylesheet" href="../gallery.css">
<link rel="icon" href="data:image/svg+xml,<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 16 16'><text y='14' font-size='14'>&#128736;</text></svg>">
</head>
<body>
<header class="marquee">
  <h1>Mockups</h1>
  <p>Playable arguments for games that are not built yet</p>
  <div class="stats">%(count)d mockup%(plural)s</div>
</header>

<main class="wrap">
  <p class="note">These are not games. Each one is a single page that runs the
  design it is arguing for, at the real 240x240, so it can be played and
  argued about before anyone writes C++. Some became games and some did not,
  and the ones that did have drifted from what shipped: a mockup is a record
  of what was proposed, not of what exists.</p>

%(body)s

  <footer class="shelf-footer">
    <span><a href="../">&larr; Games</a></span>
    <span><a href="%(repo_url)s">Source</a></span>
    <span>Built %(generated_at)s</span>
  </footer>
</main>
</body>
</html>
""" % {
        "body": body,
        "count": len(mockups),
        "plural": "" if len(mockups) == 1 else "s",
        "repo_url": escape(repo_url),
        "generated_at": escape(generated_at),
    }


def main(argv):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", default=os.path.join(REPO_ROOT, "mockups"),
                        help="directory holding <slug>/index.html")
    parser.add_argument("--out", required=True,
                        help="where the index is written")
    parser.add_argument("--repo", default="")
    parser.add_argument("--timestamp", default="")
    args = parser.parse_args(argv)

    try:
        mockups = discover(args.source)
    except MockupError as error:
        sys.stderr.write("gen_mockups: %s\n" % error)
        return 1

    os.makedirs(args.out, exist_ok=True)
    target = os.path.join(args.out, "index.html")
    with open(target, "w", encoding="utf-8") as handle:
        handle.write(render_page(mockups, args.repo, args.timestamp))
    sys.stderr.write("gen_mockups: %d mockup(s) indexed at %s\n"
                     % (len(mockups), target))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
