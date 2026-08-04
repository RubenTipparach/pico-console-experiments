#!/usr/bin/env bash
# Get a browser for the boot check and the thumbnail capture, without apt.
#
# The obvious call was `playwright install --with-deps chromium`, and it cost
# us a publish. `--with-deps` shells out to apt for Chromium's full system
# dependency set, 21 MB of it CJK font packages, and that apt run hung a job
# on the archive mirror. None of those fonts matter here: the pages under test
# draw a canvas, and the few labels around it are fine in whatever sans the
# runner already ships.
#
# So, in order: a browser the image already has, otherwise Playwright's own
# chromium, which needs no system packages on the GitHub runners. Never add
# --with-deps back.
#
# Prints the executable to use on stdout. Empty output means "let Playwright
# pick its own", which is what both tools do when the variable is unset.
# Everything else goes to stderr so the caller can capture the path cleanly:
#
#     export PLAYWRIGHT_CHROMIUM_EXECUTABLE="$(tools/setup_browser.sh)"
set -euo pipefail

pip3 install --user --quiet playwright >&2

for candidate in "${PLAYWRIGHT_CHROMIUM_EXECUTABLE:-}" \
                 /usr/bin/google-chrome \
                 /usr/bin/chromium-browser \
                 /usr/bin/chromium; do
    if [ -n "$candidate" ] && [ -x "$candidate" ]; then
        echo "setup_browser: using $candidate" >&2
        echo "$candidate"
        exit 0
    fi
done

echo "setup_browser: no system browser, fetching Playwright chromium" >&2
python3 -m playwright install chromium >&2
echo ""
