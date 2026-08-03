#!/usr/bin/env python3
"""Screenshot a published game's canvas to make a gallery thumbnail.

Serves the built site locally, loads the game, waits for the canvas to actually
have something on it, then crops the canvas element at the console's native
240x240.

CI calls this only the first time a game is published. Refreshing a thumbnail
afterwards is a deliberate act, because a screenshot that silently changes under
you is worse than a slightly stale one.

Usage:
    capture_thumbnail.py --site site --slug chicken --out site/thumbs/chicken.png
"""

import argparse
import functools
import http.server
import os
import socketserver
import sys
import threading

# How long to let the game run before capturing. A first frame is often a blank
# clear, so this waits for the game to have drawn something worth showing.
DEFAULT_SETTLE_MS = 3500


class QuietHandler(http.server.SimpleHTTPRequestHandler):
    def log_message(self, *args):
        pass


def serve(directory):
    """Start a background HTTP server and return (port, shutdown)."""
    handler = functools.partial(QuietHandler, directory=directory)
    # Bind port 0 so parallel captures cannot collide.
    httpd = socketserver.TCPServer(("127.0.0.1", 0), handler)
    port = httpd.server_address[1]
    thread = threading.Thread(target=httpd.serve_forever, daemon=True)
    thread.start()
    return port, httpd.shutdown


def capture(site, slug, out_path, settle_ms, keys):
    try:
        from playwright.sync_api import sync_playwright
    except ImportError:
        sys.stderr.write(
            "capture_thumbnail: playwright is not installed. "
            "pip install playwright && python -m playwright install chromium\n")
        return 1

    page_dir = os.path.join(site, slug)
    if not os.path.isfile(os.path.join(page_dir, "index.html")):
        sys.stderr.write("capture_thumbnail: no build at %s\n" % page_dir)
        return 1

    port, shutdown = serve(site)
    try:
        with sync_playwright() as playwright:
            # Software GL: CI runners have no GPU, and WebGL failing to create a
            # context shows up as a blank canvas rather than an error.
            launch_args = {
                "args": ["--use-gl=swiftshader", "--enable-unsafe-swiftshader"],
            }
            # Honour a preinstalled browser when the environment provides one,
            # so a machine with a pinned Chromium does not have to download a
            # second copy just to take a screenshot.
            executable = os.environ.get("PLAYWRIGHT_CHROMIUM_EXECUTABLE")
            if executable and os.path.isfile(executable):
                launch_args["executable_path"] = executable
            browser = playwright.chromium.launch(**launch_args)
            page = browser.new_page(viewport={"width": 640, "height": 640})

            errors = []
            page.on("pageerror", lambda e: errors.append(str(e)))

            page.goto("http://127.0.0.1:%d/%s/" % (port, slug),
                      wait_until="load", timeout=60000)

            # The canvas exists immediately but stays 0x0 until the wasm module
            # sizes it, so waiting for the element alone captures nothing.
            page.wait_for_function(
                "() => { const c = document.querySelector('canvas');"
                " return c && c.width > 0 && c.height > 0; }",
                timeout=60000)

            canvas = page.query_selector("canvas")
            canvas.scroll_into_view_if_needed()

            # Give the game a moment of input so the shot is not a title card
            # for a game that has none.
            page.wait_for_timeout(settle_ms // 2)
            for key in keys:
                page.keyboard.down(key)
            page.wait_for_timeout(settle_ms // 2)
            for key in keys:
                page.keyboard.up(key)

            os.makedirs(os.path.dirname(os.path.abspath(out_path)), exist_ok=True)
            canvas.screenshot(path=out_path)
            browser.close()

            if errors:
                sys.stderr.write("capture_thumbnail: page reported %d error(s); "
                                 "first: %s\n" % (len(errors), errors[0]))
    finally:
        shutdown()

    if not os.path.isfile(out_path) or os.path.getsize(out_path) == 0:
        sys.stderr.write("capture_thumbnail: produced no image for %s\n" % slug)
        return 1

    sys.stderr.write("capture_thumbnail: %s -> %s (%d bytes)\n"
                     % (slug, out_path, os.path.getsize(out_path)))
    return 0


def main(argv):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--site", required=True)
    parser.add_argument("--slug", required=True)
    parser.add_argument("--out", required=True)
    parser.add_argument("--settle-ms", type=int, default=DEFAULT_SETTLE_MS)
    parser.add_argument("--keys", default="ArrowRight",
                        help="comma separated keys held while capturing")
    args = parser.parse_args(argv)

    keys = [k.strip() for k in args.keys.split(",") if k.strip()]
    return capture(args.site, args.slug, args.out, args.settle_ms, keys)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
