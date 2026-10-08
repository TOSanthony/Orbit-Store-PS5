// Orbit Store TV app - The Solid Planet mark, rasterised once into a texture.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The mark is the home-screen icon's drawing (launcher/sce_sys/icon0.svg): a
// planet with a tilted ring whose back half passes behind it and whose front
// half crosses it with a gap. The gap and the hidden half are cut-outs, so the
// mark is drawn with real transparency on the CPU (anti-aliased by 4 x 4
// supersampling) and works over any background, art included.

#pragma once

#include "gfx/draw_list.hpp"
#include "orbit/image.hpp"

namespace orbit::mark
{

// size x size white pixels on transparent; ring_alpha dims only the ring
// (the opening screen draws a faint ring with a bright light running on it).
image::Pixels render(int size, float ring_alpha = 1.0f);

// The orbiting light at phase 0..1 around the ring, drawn on a mark placed at
// (x, y) with the given size. Parts of the trail behind the planet are hidden.
void draw_light(hui::gfx::DrawList &list, float x, float y, float size, float phase,
                float opacity = 1.0f);

} // namespace orbit::mark
