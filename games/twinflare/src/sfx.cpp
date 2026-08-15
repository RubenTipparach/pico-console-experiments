#include "sfx.hpp"

#include "audio/audio.hpp"
#include "tuning.hpp"

namespace tfs {
namespace {

using twinflare::Events;

// A step of a cue. `ticks` is in calls to sfx_tick, which is frames, not sim
// ticks: the sim runs at a fixed 100 Hz and the display does not, and a cue
// measured in sim ticks would change length with the frame rate.
struct Step {
    uint16_t freq;   // 0 = rest
    uint8_t ticks;
};

// Four, because the flag fanfare is four notes and nothing here needs five.
constexpr int k_max_steps = 4;
Step g_steps[k_max_steps];
int g_step_count = 0;
int g_step_index = 0;
int g_ticks_left = 0;
bool g_enabled = true;

// Whether the drone is currently sounding. Guarded rather than re-attacked
// every frame: trigger_attack restarts the envelope, so attacking a held note
// at the frame rate is an amplitude pump at the frame rate rather than an
// engine.
bool g_engine_on = false;
// And the grind, which is the same problem with a different voice.
bool g_grind_on = false;

void play(const Step* steps, int count) {
    // Later cues replace earlier ones: a crash should never wait politely
    // behind a countdown beep.
    g_step_count = count < k_max_steps ? count : k_max_steps;
    for (int i = 0; i < g_step_count; ++i) g_steps[i] = steps[i];
    g_step_index = -1;
    g_ticks_left = 0;
}

// The engine's note from the sim's rev level. The whole mapping is here rather
// than in the sim because it is a fact about a piezo, not about a podracer.
//
// 420 to 930 Hz. The bottom is the piezo floor rather than a choice: below
// about 400 the device makes no useful sound at all, so an engine drone down
// there is an engine nobody hears. The top stays clear of where the cues sit,
// so a flat out engine and a lap chime are never the same note.
uint16_t engine_freq(uint8_t rev) {
    return static_cast<uint16_t>(420 + rev * 2);
}

}  // namespace

void sfx_init() {
    // The cues. The device's one voice, and it pre-empts the engine below by
    // being on a lower channel.
    auto& cue = blit::channels[0];
    cue.waveforms = blit::Waveform::SQUARE;
    cue.attack_ms = 4;
    cue.decay_ms = 28;
    cue.sustain = 0x6fff;
    cue.release_ms = 26;
    cue.volume = 0x5fff;

    // The engine. Square as well, because on the device only a square channel
    // reaches the piezo at all, and this one has to be audible whenever a cue
    // is not playing. A long attack so it fades in rather than clicking, and a
    // long release so it fades out.
    auto& engine = blit::channels[1];
    engine.waveforms = blit::Waveform::SQUARE;
    engine.attack_ms = 90;
    engine.decay_ms = 40;
    engine.sustain = 0x3800;
    engine.release_ms = 160;
    engine.volume = 0x2800;

    // The grind. Noise, so it is texture rather than a note, and quiet: it runs
    // for as long as the pod is against the rock and a loud one would be the
    // only thing in the mix.
    auto& grind = blit::channels[2];
    grind.waveforms = blit::Waveform::NOISE;
    grind.attack_ms = 10;
    grind.decay_ms = 40;
    grind.sustain = 0x5000;
    grind.release_ms = 60;
    grind.volume = 0x1c00;
}

void sfx_set_enabled(bool enabled) {
    if (g_enabled && !enabled) sfx_silence();
    g_enabled = enabled;
}

bool sfx_enabled() { return g_enabled; }

void sfx_silence() {
    blit::channels[0].trigger_release();
    blit::channels[1].trigger_release();
    blit::channels[2].trigger_release();
    g_step_count = 0;
    g_engine_on = false;
    g_grind_on = false;
}

void sfx_handle(const Events& ev) {
    if (!g_enabled) return;

    // ---- the engine, which is a level and not an event ----------------------
    auto& engine = blit::channels[1];
    if (ev.rev > 0) {
        engine.frequency = engine_freq(ev.rev);
        if (!g_engine_on) {
            engine.trigger_attack();
            g_engine_on = true;
        }
    } else if (g_engine_on) {
        engine.trigger_release();
        g_engine_on = false;
    }

    // ---- the grind, likewise held ------------------------------------------
    auto& grind = blit::channels[2];
    if (ev.grinding) {
        if (!g_grind_on) {
            grind.frequency = 900;
            grind.trigger_attack();
            g_grind_on = true;
        }
    } else if (g_grind_on) {
        grind.trigger_release();
        g_grind_on = false;
    }

    // ---- the cues ----------------------------------------------------------
    // One chain, most consequential first, so the loudest thing that happened
    // this frame is the thing that is heard. A frame where the pod lands hard,
    // clips a rival and completes a lap is one sound, and it should be the lap.
    if (ev.wreck) {
        const Step s[] = {{620, 6}, {500, 10}, {410, 22}};
        play(s, 3);
        // The only cue that reaches across: a wrecked pod has no engine note.
        engine.trigger_release();
        g_engine_on = false;
    } else if (ev.finish) {
        const Step s[] = {{523, 5}, {659, 5}, {784, 5}, {1046, 22}};
        play(s, 4);
    } else if (ev.engine_out) {
        const Step s[] = {{660, 4}, {500, 9}, {430, 13}};
        play(s, 3);
    } else if (ev.flood) {
        const Step s[] = {{600, 4}, {440, 14}};
        play(s, 2);
    } else if (ev.go) {
        const Step s[] = {{1320, 18}};
        play(s, 1);
    } else if (ev.lap) {
        const Step s[] = {{784, 5}, {1046, 12}};
        play(s, 2);
    } else if (ev.launch) {
        const Step s[] = {{440, 3}, {880, 4}, {1320, 10}};
        play(s, 3);
    } else if (ev.boost) {
        const Step s[] = {{520, 3}, {880, 10}};
        play(s, 2);
    } else if (ev.count) {
        const Step s[] = {{660, 7}};
        play(s, 1);
    } else if (ev.slam) {
        const Step s[] = {{440, 11}};
        play(s, 1);
    } else if (ev.bump) {
        const Step s[] = {{580, 3}, {460, 6}};
        play(s, 2);
    } else if (ev.scrape) {
        const Step s[] = {{540, 5}};
        play(s, 1);
    }
}

void sfx_tick() {
    auto& cue = blit::channels[0];

    if (g_ticks_left > 0) {
        --g_ticks_left;
        return;
    }

    ++g_step_index;
    if (g_step_index >= g_step_count) {
        // THE RELEASE IS LOAD BEARING. The cue channel sustains, so a sequence
        // that simply stops parks the channel in SUSTAIN, where it is still not
        // idle: on the device that means it holds the piezo forever and the
        // engine below it is never heard again.
        if (g_step_count > 0) {
            cue.trigger_release();
            g_step_count = 0;
        }
        return;
    }

    const Step& step = g_steps[g_step_index];
    g_ticks_left = step.ticks;
    if (step.freq == 0) {
        cue.trigger_release();
    } else {
        cue.frequency = step.freq;
        cue.trigger_attack();
    }
}

}  // namespace tfs
