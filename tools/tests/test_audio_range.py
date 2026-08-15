#!/usr/bin/env python3
"""Every note this console plays has to be a note it can actually make.

The PicoSystem's audio output is a piezo sounder, and a piezo is a high pass
device by construction: its output climbs with frequency toward a mechanical
resonance in the low kilohertz and falls away steeply below that. A few hundred
hertz is faint and the bottom of a piano is nothing at all. The web and desktop
builds run the SDK's full mixer through a real speaker and will happily play
any of it, so a tone that is silent on hardware sounds fine everywhere a
developer is likely to hear it.

Which is exactly how this got in. Reported from playing: "the lower tones from
the fishing game aren't really working". They are not: kingfisher's reel
ratchet was 72 Hz. Looking across the repo it was not one game's mistake but
the same reasonable decision made five times over, because low reads as bad
news and every game wanted its failure sound to be the darkest thing it had:

    stardancer  a kill at 60 Hz, a hit at 90
    kingfisher  the ratchet at 72
    picospace   a wreck at 96
    tomlander   a crash at 110
    twinflare   a wreck at 110

A piezo cannot do dark. What replaces it is a FALL: a figure that drops through
the audible band reads as failure on a one voice beeper far better than a low
tone nobody hears. That is what those cues are now.

This walks every game rather than a fixture, so a new sound below the floor
fails the build instead of shipping as silence.

Usage:
    test_audio_range.py
"""

import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO_ROOT = os.path.dirname(os.path.dirname(HERE))
GAMES = os.path.join(REPO_ROOT, "games")

# The floor, and it is a judgement rather than a measurement: nobody here has a
# spectrum analyser on the device. It comes from what a piezo is (output falls
# away below its resonance) plus the one empirical data point we have, which is
# a player saying the sub-100 Hz cues cannot be heard. Anything at or above this
# is at least in the register a small sounder can move air at; 500 Hz and up is
# where it starts to be comfortable.
FLOOR_HZ = 400

# And a ceiling, because the other end is real too: a piezo will happily be
# driven above where it is pleasant, and the SDK's channels go far past
# anything worth listening to on a handheld.
CEILING_HZ = 4000

failures = []


def check(ok, what):
    if not ok:
        failures.append(what)
        print("FAIL: %s" % what)


def audio_sources():
    """Every game source file that talks to the audio channels."""
    out = []
    for game in sorted(os.listdir(GAMES)):
        src = os.path.join(GAMES, game, "src")
        if not os.path.isdir(src):
            continue
        for name in sorted(os.listdir(src)):
            if not name.endswith((".cpp", ".hpp")):
                continue
            path = os.path.join(src, name)
            with open(path, encoding="utf-8") as handle:
                text = handle.read()
            if "channels[" in text or "audio/audio.hpp" in text:
                out.append((game, os.path.join("src", name), text))
    return out


def strip_comments(text):
    """So a frequency quoted in a comment is not read as a frequency played."""
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    return re.sub(r"//[^\n]*", "", text)


def frequencies(text):
    """Every frequency the file can produce, as (hz, how it was written).

    Three shapes are used across the repo and all three are read:
      - `{freq, ticks}` entries in a step table
      - a cue helper called with a literal, `sound_cue(440, ...)` / `cue(ch, 440, ...)`
      - an assignment to `.frequency`, literal or `BASE + something`

    For the last shape only the BASE is checked, which is the bottom of the
    ramp and therefore the quietest note it can reach. That is the number that
    matters: the top of a ramp being audible says nothing about an idle.
    """
    text = strip_comments(text)
    found = []
    for m in re.finditer(r"\{\s*(\d{2,5})\s*,\s*\d+\s*\}", text):
        found.append((int(m.group(1)), "step table"))
    for m in re.finditer(r"\bsound_cue\(\s*(\d+)\s*,", text):
        found.append((int(m.group(1)), "sound_cue"))
    for m in re.finditer(r"\bcue\(\s*[A-Za-z_][A-Za-z0-9_]*\s*,\s*(\d+)\s*,", text):
        found.append((int(m.group(1)), "cue"))
    for m in re.finditer(r"\.frequency\s*=\s*(?:static_cast<\w+>\()?\s*(\d+)", text):
        found.append((int(m.group(1)), "frequency ="))
    for m in re.finditer(r"\breturn\s+static_cast<uint16_t>\(\s*(\d+)\s*\+", text):
        found.append((int(m.group(1)), "frequency helper"))
    # A rest is a rest, not a 0 Hz note.
    return [(hz, how) for hz, how in found if hz > 0]


def test_every_note_is_one_the_piezo_can_make():
    sources = audio_sources()
    check(len(sources) >= 4, "the audio sources were actually found")
    total = 0
    for game, name, text in sources:
        freqs = frequencies(text)
        for hz, how in freqs:
            total += 1
            check(hz >= FLOOR_HZ,
                  "%s %s: %d Hz (%s) is below the %d Hz floor, so it is "
                  "silent on the device" % (game, name, hz, how, FLOOR_HZ))
            check(hz <= CEILING_HZ,
                  "%s %s: %d Hz (%s) is above the %d Hz ceiling"
                  % (game, name, hz, how, CEILING_HZ))
    check(total >= 30, "and enough of them were parsed to mean anything")
    print("  %d frequencies across %d files, all within %d..%d Hz"
          % (total, len(sources), FLOOR_HZ, CEILING_HZ))


def test_the_parser_can_see_a_bad_note():
    """The check above is only worth having if it can fail.

    Every pattern the repo actually uses is put past the parser with a note
    below the floor in it, because a regex that silently matches nothing is a
    test that passes for every possible source file.
    """
    cases = [
        ("step table", "const Step s[] = {{72, 4}, {880, 6}};"),
        ("sound_cue", "sound_cue(96, 460, 6000);"),
        ("cue", "cue(k_ch_hit, 60, 320, 4800);"),
        ("frequency =", "channel.frequency = 72;"),
        ("frequency = cast", "ch.frequency = static_cast<uint16_t>(240 + x);"),
        ("frequency helper", "return static_cast<uint16_t>(150 + rev * 2);"),
    ]
    for name, snippet in cases:
        got = frequencies(snippet)
        check(bool(got) and min(hz for hz, _ in got) < FLOOR_HZ,
              "the parser sees a low note written as '%s'" % name)
    # And does not invent one out of a comment or a rest.
    check(frequencies("// a wreck at 96 Hz, low and long") == [],
          "a frequency named in a comment is not a frequency played")
    check(frequencies("const Step s[] = {{0, 3}};") == [],
          "a rest is a rest, not a note at zero hertz")


def main():
    test_the_parser_can_see_a_bad_note()
    test_every_note_is_one_the_piezo_can_make()
    if failures:
        print("audio range: %d check(s) failed" % len(failures))
        return 1
    print("audio range: all checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
