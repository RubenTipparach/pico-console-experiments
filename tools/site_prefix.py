#!/usr/bin/env python3
"""Work out where in the published site a build belongs.

The default branch owns the site root. Every other branch gets its own preview
subdirectory, so a branch can deploy on every push without ever overwriting the
live gallery.

Both the detect job and the publish job need this answer and they must agree: if
they disagree, a branch reads one branch's fingerprints and writes another's,
and the build plan silently stops making sense. Hence one implementation rather
than the same `if` written twice in YAML.

Prints the prefix with a trailing slash, or an empty line for the root.

Usage:
    site_prefix.py --ref-name main --default-branch main            ->
    site_prefix.py --ref-name feature/pad --default-branch main     -> preview/feature-pad/
    site_prefix.py --ref-name feature/pad --default-branch main --force-root ->
"""

import argparse
import re
import sys

# Anything outside this set gets folded to a dash. Branch names can contain
# slashes, dots and characters that have no business in a URL path.
UNSAFE = re.compile(r"[^A-Za-z0-9._-]+")


def branch_slug(ref_name):
    slug = UNSAFE.sub("-", ref_name).strip("-.")
    # Guard against a name that collapses to nothing, or to something that would
    # escape the preview directory.
    slug = slug.replace("..", "-")
    return slug or "branch"


def site_prefix(ref_name, default_branch, force_root=False):
    if force_root or ref_name == default_branch:
        return ""
    return "preview/%s/" % branch_slug(ref_name)


def main(argv):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--ref-name", default="")
    parser.add_argument("--default-branch", default="main")
    parser.add_argument("--force-root", action="store_true",
                        help="deploy to the root even from a non default branch")
    args = parser.parse_args(argv)

    sys.stdout.write(site_prefix(args.ref_name, args.default_branch,
                                 args.force_root) + "\n")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
