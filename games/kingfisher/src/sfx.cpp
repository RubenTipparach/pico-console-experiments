#include "sfx.hpp"

#include "audio/audio.hpp"

namespace kfs {
namespace {

struct Step {
    uint16_t freq;   // 0 = rest
    uint8_t ticks;   // duration at the 10 ms update rate
};

constexpr int k_max_steps = 6;
Step g_steps[k_max_steps];
int g_step_count = 0;
int g_step_index = 0;
int g_ticks_left = 0;
bool g_enabled = true;

void play(const Step* steps, int count) {
    // Later events replace earlier ones: a snap should never wait politely
    // behind a nibble tick.
    g_step_count = count < k_max_steps ? count : k_max_steps;
    for (int i = 0; i < g_step_count; i++) g_steps[i] = steps[i];
    g_step_index = -1;
    g_ticks_left = 0;
}

}  // namespace

void sfx_init() {
    auto& channel = blit::channels[0];
    channel.waveforms = blit::Waveform::SQUARE;
    channel.attack_ms = 4;
    channel.decay_ms = 30;
    channel.sustain = 0x6fff;
    channel.release_ms = 30;
    channel.volume = 0x5fff;

    // The reel ratchet lives on its own channel: a low triangle thump whose
    // envelope decays to silence on its own, so it can tick away under a
    // bite jingle without either interrupting the other.
    auto& click = blit::channels[1];
    click.waveforms = blit::Waveform::TRIANGLE;
    click.attack_ms = 2;
    click.decay_ms = 45;
    click.sustain = 0;
    click.release_ms = 10;
    click.volume = 0x42ff;
}

void sfx_set_enabled(bool enabled) {
    if (g_enabled && !enabled) {
        blit::channels[0].trigger_release();
        blit::channels[1].trigger_release();
        g_step_count = 0;
    }
    g_enabled = enabled;
}

bool sfx_enabled() { return g_enabled; }

void sfx_handle(const kf::Events& ev) {
    if (!g_enabled) return;

    // The ratchet is independent of the priority chain below: it is texture,
    // not an announcement, and it plays alongside whatever else fires.
    if (ev.reel_click) {
        auto& click = blit::channels[1];
        click.frequency = 72;
        click.trigger_attack();
    }

    // Order matters: the most important sound of the tick wins.
    if (ev.new_record) {
        const Step s[] = {{523, 6}, {659, 6}, {784, 6}, {1046, 14}};
        play(s, 4);
    } else if (ev.caught) {
        const Step s[] = {{523, 5}, {659, 5}, {784, 10}};
        play(s, 3);
    } else if (ev.snap) {
        const Step s[] = {{220, 4}, {160, 14}};
        play(s, 2);
    } else if (ev.escape) {
        const Step s[] = {{240, 8}, {180, 10}};
        play(s, 2);
    } else if (ev.hooked) {
        const Step s[] = {{440, 3}, {880, 5}};
        play(s, 2);
    } else if (ev.bite) {
        const Step s[] = {{700, 4}, {0, 3}, {700, 4}};
        play(s, 3);
    } else if (ev.leap) {
        const Step s[] = {{620, 3}, {930, 4}};
        play(s, 2);
    } else if (ev.nibble) {
        const Step s[] = {{1100, 2}};
        play(s, 1);
    } else if (ev.splash) {
        const Step s[] = {{300, 3}, {210, 4}};
        play(s, 2);
    } else if (ev.cast) {
        const Step s[] = {{900, 3}, {620, 3}};
        play(s, 2);
    } else if (ev.wiggle) {
        const Step s[] = {{500, 1}};
        play(s, 1);
    }
}

void sfx_tick() {
    auto& channel = blit::channels[0];

    if (g_ticks_left > 0) {
        g_ticks_left--;
        return;
    }

    g_step_index++;
    if (g_step_index >= g_step_count) {
        if (g_step_count > 0) {
            channel.trigger_release();
            g_step_count = 0;
        }
        return;
    }

    const Step& step = g_steps[g_step_index];
    g_ticks_left = step.ticks;
    if (step.freq == 0) {
        channel.trigger_release();
    } else {
        channel.frequency = step.freq;
        channel.trigger_attack();
    }
}

}  // namespace kfs
