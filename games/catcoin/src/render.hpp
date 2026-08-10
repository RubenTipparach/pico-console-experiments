#pragma once

#include "pse/pixel.hpp"

#include "sim.hpp"

namespace cc {

// Draw one frame of `world` into `target`.
//
// All of it, including the panel and the shop. Nothing here calls the SDK, so
// the preview harness renders the same screen the device does and a layout
// mistake is visible without a PicoSystem in hand.
void render_world(const World& world, const pse::RenderTarget& target);

// The layout, in device pixels, exposed so the preview harness can check that
// the rows it names do not overlap. Everything else is measured off these.
constexpr int k_hud_h = 11;
constexpr int k_rail_y = 41;
constexpr int k_tray_y = 142;
constexpr int k_bar_y = 172;
constexpr int k_bar_h = 12;
constexpr int k_row_y = 188;
constexpr int k_row_h = 26;
constexpr int k_desc_y = 219;
constexpr int k_slot_w = 24;
constexpr int k_slot_gap = 2;
constexpr int k_buy_x = 138;
constexpr int k_buy_w = 44;
constexpr int k_end_x = 186;
constexpr int k_end_w = 48;

// What a special is called and what it does, for the panel and the shop. One
// table, so the name on a coin and the name in the shop cannot drift apart.
const char* special_name(uint8_t stype);
const char* special_effect(uint8_t stype);

// Every string the description line under the row can hold, so the preview
// harness can check all of them fit rather than the ones somebody listed.
constexpr int k_description_count = 3;
const char* description_line(int index);
constexpr int k_desc_x = 6;

}  // namespace cc
