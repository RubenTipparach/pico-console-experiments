#pragma once

#include <cstdint>

#include "pse/pixel.hpp"

#include "sim.hpp"

namespace slr {

// Which screen the shell is on. The renderer draws all four, including the
// menus, through pse::draw_text into a RenderTarget rather than through the
// SDK's screen.text.
//
// That is a deliberate departure from the other games here, and the reason is
// in this repo's own notes: text drawn with the SDK does not exist in a host
// build, so a change to a menu or a HUD is unverified until somebody runs the
// game on hardware. Everything Starlance puts on screen is drawn by this file,
// so the preview harness renders the title, the pause menu and the whole
// combat HUD as real frames, and a layout mistake is a picture rather than a
// bug report.
enum class Screen : uint8_t { Title, Play, Paused, Debrief };

// The pause menu's rows. Named here because this file draws them and game.cpp
// acts on them, and a count that lived in only one of the two would let the
// highlight run off the end of a menu that had grown.
enum PauseItem : uint8_t {
    kResume,
    kPauseSound,
    kPauseInvert,
    kAbort,
    kPauseItemCount,
};

// The title screen's rows.
enum TitleItem : uint8_t {
    kLaunch,
    kTitleSound,
    kTitleInvert,
    kTitleItemCount,
};

// Everything the renderer needs that is not in the World: which screen, where
// the highlight is, and the two settings the menus show the state of.
struct Chrome {
    Screen screen;
    uint8_t item;
    bool sound_on;
    bool invert_pitch;
    uint32_t best_score;
};

// Draw one frame. Pure presentation: never ticks the sim, and every line of it
// runs on the host preview harness.
void render_scene(const sl::World& world, const Chrome& chrome,
                  const pse::RenderTarget& target, uint32_t time_ms);

// What the last frame cost.
//
// The frame queue has a fixed capacity and silently drops the overflow, which
// on screen is a hole in a frigate rather than a crash. The preview harness
// watches this and fails loudly, because the one frame that overflows is the
// one with the capital ship in it and that is the frame nobody was looking at
// when they decided the budget was fine.
struct FrameStats {
    uint16_t queued;
    uint16_t dropped;
    // The depth range the world was bracketed to this frame, in whole world
    // units. Nothing in the game reads these; the preview prints them, because
    // the one byte depth buffer is the constraint this renderer lives under
    // and a bracket that has quietly gone wrong looks like a modelling bug.
    uint16_t near_units;
    uint16_t far_units;
    // How many enemy hulls were actually drawn, as against how many are alive.
    // A contact past k_draw_range is on the HUD and not out of the window, on
    // purpose, and this is how the preview says so out loud rather than
    // leaving it to be read as a missing ship.
    uint8_t hulls_drawn;
    uint8_t hulls_live;
};
FrameStats last_frame_stats();

}  // namespace slr
