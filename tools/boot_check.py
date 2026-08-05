#!/usr/bin/env python3
"""Boot a built game headless and fail on any page error.

Compiling proves nothing about booting: a wasm whose EM_ASM addresses no
longer match its JS loads, prints the runtime banner, and dies on the first
frame, all with green CI. This runs the actual build in the actual runtime
for a few seconds and fails the publish if the page throws, so a dead game
can never reach the site again.

Usage:
    boot_check.py --dir incoming/web --slug kingfisher
"""

import argparse
import functools
import http.server
import os
import socketserver
import sys
import threading

# How long to let the game run after load. The EM_ASM crash that motivated
# this check fired within the first second; a few gives slower failures room.
RUN_MS = 6000


class QuietHandler(http.server.SimpleHTTPRequestHandler):
    def log_message(self, *args):
        pass


def serve(directory):
    handler = functools.partial(QuietHandler, directory=directory)
    httpd = socketserver.TCPServer(("127.0.0.1", 0), handler)
    port = httpd.server_address[1]
    thread = threading.Thread(target=httpd.serve_forever, daemon=True)
    thread.start()
    return port, httpd.shutdown


def check(directory, slug):
    try:
        from playwright.sync_api import sync_playwright
    except ImportError:
        sys.stderr.write("boot_check: playwright is not installed\n")
        return 1

    if not os.path.isfile(os.path.join(directory, slug, "index.html")):
        sys.stderr.write("boot_check: no build at %s/%s\n" % (directory, slug))
        return 1

    port, shutdown = serve(directory)
    errors = []
    panels = 0
    leftover = 0
    try:
        with sync_playwright() as playwright:
            launch_args = {
                "args": ["--use-gl=swiftshader", "--enable-unsafe-swiftshader"],
            }
            executable = os.environ.get("PLAYWRIGHT_CHROMIUM_EXECUTABLE")
            if executable and os.path.isfile(executable):
                launch_args["executable_path"] = executable
            browser = playwright.chromium.launch(**launch_args)
            page = browser.new_page(viewport={"width": 640, "height": 640})
            page.on("pageerror", lambda e: errors.append(str(e)))
            page.goto("http://127.0.0.1:%d/%s/" % (port, slug),
                      wait_until="load", timeout=60000)
            page.wait_for_timeout(RUN_MS)
            # The tutorial is built into each game's page at configure time,
            # and nothing downstream would notice if that stopped happening:
            # the game would boot and play, and only a person landing on it
            # cold would find it says nothing about itself. Checked on the
            # page that is about to deploy, because that is the only place
            # the whole chain from game.yml to the served HTML is visible.
            panels = page.locator("#tutorial [data-panel]").count()
            leftover = page.content().count("PSE_TUTORIAL")
            browser.close()
    finally:
        shutdown()

    if errors:
        sys.stderr.write("boot_check: %s threw %d error(s):\n"
                         % (slug, len(errors)))
        for error in errors[:5]:
            sys.stderr.write("  %s\n" % error.splitlines()[0][:300])
        return 1

    if leftover:
        sys.stderr.write("boot_check: %s still has the tutorial placeholder: "
                         "gen_shell.py did not run for this build\n" % slug)
        return 1

    if panels < 1:
        sys.stderr.write("boot_check: %s has no tutorial panels, so its page "
                         "cannot say what the game is or what the buttons "
                         "do\n" % slug)
        return 1

    sys.stderr.write("boot_check: %s boots clean, %d tutorial panel(s)\n"
                     % (slug, panels))
    return 0


def main(argv):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dir", required=True,
                        help="directory holding <slug>/index.html")
    parser.add_argument("--slug", required=True)
    args = parser.parse_args(argv)
    return check(args.dir, args.slug)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
