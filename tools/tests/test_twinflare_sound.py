#!/usr/bin/env python3
"""The one file in Twin Flare that nothing else can check.

`games/twinflare/src/sfx.cpp` includes the 32blit SDK's audio header, so it
cannot be compiled by the host test harness and is not compiled by anything
until the cross build runs in CI. Every other promise this game makes is proven
by a binary that runs here; the sound is proven by reading the source, which is
worth doing precisely because nothing else will.

What is checked is the set of things that have gone wrong in a beeper before,
and one that is specific to this arrangement:

  - every event the sim can raise has a sound, so adding a field to Events and
    forgetting the cue is a failed build rather than a silent feature;
  - every frequency is inside a band a piezo can actually make;
  - the sequencer's terminal trigger_release is present. The cue channel
    sustains, so a sequence that just stops parks it in SUSTAIN, where it holds
    the device's only voice forever and the engine under it is never heard
    again;
  - the held voices are guarded rather than re-attacked, because attacking a
    held note every frame is an amplitude pump at the frame rate;
  - the drone is not on channel 0, which is the whole voice allocation argument
    in sfx.hpp reduced to the one line that would break it;
  - sfx.cpp is built by the game and NOT by the host test harness, which cannot
    link it.

Usage:
    test_twinflare_sound.py
"""

import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO_ROOT = os.path.dirname(os.path.dirname(HERE))
GAME = os.path.join(REPO_ROOT, "games", "twinflare")
SFX = os.path.join(GAME, "src", "sfx.cpp")
SIM_HPP = os.path.join(GAME, "src", "sim.hpp")

failures = []


def check(ok, what):
    if not ok:
        failures.append(what)
        print("FAIL: %s" % what)


def read(path):
    with open(path, encoding="utf-8") as handle:
        return handle.read()


def event_fields(sim_src):
    """The bool members of struct Events, which are the cues the sim can raise.

    Read out of the header rather than listed here, because a list here is a
    second copy of the struct and the whole point of the check is to notice
    when the struct grows.
    """
    body = re.search(r"struct Events \{(.*?)\n\};", sim_src, re.S)
    assert body, "struct Events not found in sim.hpp"
    names = []
    for line in body.group(1).splitlines():
        line = line.split("//")[0].strip()
        if not line.startswith("bool "):
            continue
        for name in line[len("bool "):].rstrip(";").split(","):
            names.append(name.strip())
    return names


def test_every_event_makes_a_sound():
    sim = read(SIM_HPP)
    sfx = read(SFX)
    names = event_fields(sim)
    check(len(names) >= 10, "the Events struct was actually parsed")
    for name in names:
        check(re.search(r"\bev\.%s\b" % re.escape(name), sfx) is not None,
              "the event '%s' has a sound" % name)


def test_the_levels_are_read_as_levels():
    """`rev` and `grinding` are held states, not edges, and are not bools.

    They are the two members that must NOT be in the priority chain: a drone
    handled as a one shot cue is a click.
    """
    sfx = read(SFX)
    for name in ("rev", "grinding"):
        check(re.search(r"\bev\.%s\b" % name, sfx) is not None,
              "the level '%s' is read" % name)
        check(re.search(r"else if \(ev\.%s\)" % name, sfx) is None,
              "the level '%s' is not treated as a one shot cue" % name)


def test_every_frequency_is_playable():
    """A piezo has a band. Below it there is a click, above it there is nothing.

    Every literal in a Step table and every frequency assignment is checked,
    which also catches a fat fingered 6600 for 660.
    """
    sfx = read(SFX)
    freqs = [int(m) for m in re.findall(r"\{(\d+),\s*\d+\}", sfx)]
    check(len(freqs) >= 15, "the cue tables were actually parsed")
    for f in freqs:
        check(100 <= f <= 4000, "cue frequency %d is inside the piezo's band" % f)
    # Every step also has a duration, and a zero one is a note nobody hears.
    durations = [int(m) for m in re.findall(r"\{\d+,\s*(\d+)\}", sfx)]
    for d in durations:
        check(1 <= d <= 60, "cue step length %d is a sane number of frames" % d)


def test_the_sequencer_lets_go_at_the_end():
    """The load bearing release.

    The cue channel has a non-zero sustain, so a sequence that runs out of
    steps and simply stops leaves the channel in SUSTAIN: on the device that is
    the piezo held forever.
    """
    sfx = read(SFX)
    tail = sfx[sfx.index("void sfx_tick()"):]
    end = re.search(r"if \(g_step_index >= g_step_count\) \{(.*?)\n    \}", tail, re.S)
    check(end is not None, "sfx_tick has an end of sequence branch")
    if end:
        check("trigger_release" in end.group(1),
              "the end of a sequence releases the cue channel")


def test_the_held_voices_are_guarded():
    """Attacking a held note every frame is a pump, not a drone."""
    sfx = read(SFX)
    for guard in ("g_engine_on", "g_grind_on"):
        check(re.search(r"if \(!%s\)" % guard, sfx) is not None,
              "the held voice behind '%s' is attacked once, not every frame" % guard)
        check(re.search(r"} else if \(%s\)" % guard, sfx) is not None,
              "and released once when it stops")


def test_the_drone_is_not_on_the_device_voice():
    """The whole voice allocation argument, as the one line that would break it.

    A held note on channel 0 owns the piezo for the entire race, and no cue is
    ever heard on hardware again.
    """
    sfx = read(SFX)
    init = sfx[sfx.index("void sfx_init()"):sfx.index("void sfx_set_enabled")]
    # Which channel each named voice is bound to.
    bindings = dict((name, int(ch)) for name, ch in
                    re.findall(r"auto& (\w+) = blit::channels\[(\d+)\];", init))
    check(bindings.get("cue") == 0, "the cues are on channel 0, the device's voice")
    check(bindings.get("engine", 0) > 0, "the engine drone is NOT on channel 0")
    check(bindings.get("grind", 0) > 0, "and neither is the grind")
    # And the drone really is a drone: a channel meant to be held needs a
    # sustain to hold at.
    engine = re.search(r"engine\.sustain = (0x[0-9a-fA-F]+|\d+);", init)
    check(engine is not None and int(engine.group(1), 0) > 0,
          "the engine channel sustains, so a held note holds")


def test_the_sound_is_built_by_the_game_and_not_by_the_tests():
    """sfx.cpp needs the SDK. The host harness has no SDK and cannot link it."""
    game_cmake = read(os.path.join(GAME, "CMakeLists.txt"))
    test_cmake = read(os.path.join(GAME, "tests", "CMakeLists.txt"))
    check("src/sfx.cpp" in game_cmake, "the game builds sfx.cpp")
    check("sfx.cpp" not in test_cmake, "and the host test harness does not")


def test_the_game_drives_the_sequencer_everywhere():
    """A screen that skips sfx_tick freezes whatever cue was playing, mid note.

    game_update has one call at the end, outside the screen switch, rather than
    one per screen.
    """
    game = read(os.path.join(GAME, "src", "game.cpp"))
    check(game.count("sfx_tick()") == 1, "sfx_tick is called from exactly one place")
    update = game[game.index("void game_update"):]
    switch_end = update.index("\n    }\n")
    check("sfx_tick()" in update[switch_end:],
          "and it is after the screen switch, so every screen reaches it")


def main():
    test_every_event_makes_a_sound()
    test_the_levels_are_read_as_levels()
    test_every_frequency_is_playable()
    test_the_sequencer_lets_go_at_the_end()
    test_the_held_voices_are_guarded()
    test_the_drone_is_not_on_the_device_voice()
    test_the_sound_is_built_by_the_game_and_not_by_the_tests()
    test_the_game_drives_the_sequencer_everywhere()
    if failures:
        print("twinflare sound: %d check(s) failed" % len(failures))
        return 1
    print("twinflare sound: all checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
