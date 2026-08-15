#include "sfx.hpp"

#include "audio/audio.hpp"

namespace drs {
namespace {

using dr::Events;

// A step of a cue. `ticks` counts calls to sfx_tick, which are frames rather
// than sim ticks: the sim runs at a fixed 100 Hz and the display does not, so
// a cue measured in sim ticks would change length with the frame rate.
struct Step {
    uint16_t freq;   // 0 = rest
    uint8_t ticks;
};

constexpr int k_max_steps = 4;
Step g_steps[k_max_steps];
int g_step_count = 0;
int g_step_index = 0;
int g_ticks_left = 0;
bool g_enabled = true;

// Held voices are attacked once and left alone. trigger_attack restarts the
// envelope, so attacking a held note every frame is an amplitude pump at the
// frame rate rather than an engine.
bool g_engine_on = false;
bool g_rough_on = false;

void play(const Step* steps, int count) {
    // Later cues replace earlier ones: a wreck should never wait politely
    // behind a distance chime.
    g_step_count = count < k_max_steps ? count : k_max_steps;
    for (int i = 0; i < g_step_count; ++i) g_steps[i] = steps[i];
    g_step_index = -1;
    g_ticks_left = 0;
}

// The engine note from the sim's rev level. The mapping is here rather than in
// the sim because it is a fact about a piezo, not about a bike.
//
// 430 to 940 Hz. The bottom is the piezo floor rather than a choice: under
// about 400 the device makes no useful sound, so an idle down there is an idle
// nobody hears. The top stays clear of where the cues sit, so a bike at full
// chat and a distance chime are never the same note.
uint16_t engine_freq(uint8_t rev) {
    return static_cast<uint16_t>(430 + rev * 2);
}

}  // namespace

void sfx_init() {
    auto& cue = blit::channels[0];
    cue.waveforms = blit::Waveform::SQUARE;
    cue.attack_ms = 3;
    cue.decay_ms = 26;
    cue.sustain = 0x6fff;
    cue.release_ms = 24;
    cue.volume = 0x5fff;

    auto& engine = blit::channels[1];
    engine.waveforms = blit::Waveform::SQUARE;
    engine.attack_ms = 70;
    engine.decay_ms = 40;
    engine.sustain = 0x3400;
    engine.release_ms = 140;
    engine.volume = 0x2600;

    // Sand and the closing window share a noise voice, because they are both
    // texture rather than notes and they cannot both be true in a way that
    // matters: a rider cornered by the window has a bigger problem than the
    // surface under them.
    auto& rough = blit::channels[2];
    rough.waveforms = blit::Waveform::NOISE;
    rough.attack_ms = 12;
    rough.decay_ms = 40;
    rough.sustain = 0x5000;
    rough.release_ms = 60;
    rough.volume = 0x1a00;
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
    g_rough_on = false;
}

void sfx_handle(const Events& ev) {
    if (!g_enabled) return;

    // ---- the engine, a level and not an event ----
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

    // ---- sand, and the window closing, on the one noise voice ----
    auto& rough = blit::channels[2];
    const bool rough_now = ev.offroad || ev.cornered;
    if (rough_now) {
        // Cornered is the higher, tighter hiss, so the two are told apart by
        // more than being present. It also wins the voice when both are true.
        const uint16_t want = ev.cornered ? 1800 : 800;
        if (!g_rough_on || rough.frequency != want) {
            rough.frequency = want;
            if (!g_rough_on) rough.trigger_attack();
            g_rough_on = true;
        }
    } else if (g_rough_on) {
        rough.trigger_release();
        g_rough_on = false;
    }

    // ---- the cues, most consequential first ----
    if (ev.died) {
        // A fall through the band. It cannot be a low thud, which is what this
        // wanted to be: see the header.
        const Step s[] = {{600, 5}, {480, 9}, {410, 20}};
        play(s, 3);
        // The only cue that reaches across: a wrecked bike has no engine note.
        engine.trigger_release();
        g_engine_on = false;
        rough.trigger_release();
        g_rough_on = false;
    } else if (ev.record) {
        // Never heard on its own: a record is set by dying, so died wins the
        // chain above and this is here for the day the rules change.
        const Step s[] = {{784, 5}, {988, 5}, {1319, 16}};
        play(s, 3);
    } else if (ev.launched) {
        const Step s[] = {{520, 3}, {780, 8}};
        play(s, 2);
    } else if (ev.milestone) {
        const Step s[] = {{1046, 4}};
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
        // that simply stops parks it in SUSTAIN, where it is still not idle:
        // on the device that holds the piezo forever and the engine under it is
        // never heard again.
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

}  // namespace drs
