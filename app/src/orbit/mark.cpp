// Orbit Store TV app - The Solid Planet mark, rasterised once into a texture.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "orbit/mark.hpp"

#include <algorithm>
#include <cmath>

namespace orbit::mark
{

namespace
{

// The icon's geometry in its 512-unit design space.
constexpr float kCx = 256.0f;
constexpr float kCy = 262.0f;
constexpr float kPlanet = 118.0f; // planet radius
constexpr float kHidden = 132.0f; // the back ring disappears inside this radius
constexpr float kRx = 214.0f;     // ring ellipse radii
constexpr float kRy = 64.0f;
constexpr float kStroke = 16.0f;    // half the ring's stroke width
constexpr float kGap = 30.0f;       // half the gap cut into the planet by the front ring
constexpr float kTilt = -0.383972f; // -22 degrees

const float kE1x = std::cos(kTilt);
const float kE1y = std::sin(kTilt);
const float kE2x = -std::sin(kTilt);
const float kE2y = std::cos(kTilt);

struct Local
{
    float x;
    float y;
    float ring;   // approximate distance from the ring's centre line
    float radius; // distance from the planet's centre
};

Local locate(float u, float v)
{
    const float dx = u - kCx;
    const float dy = v - kCy;
    Local l;
    l.x = dx * kE1x + dy * kE1y;
    l.y = dx * kE2x + dy * kE2y;
    const float g = (l.x * l.x) / (kRx * kRx) + (l.y * l.y) / (kRy * kRy) - 1.0f;
    const float gx = 2.0f * l.x / (kRx * kRx);
    const float gy = 2.0f * l.y / (kRy * kRy);
    const float gradient = std::sqrt(gx * gx + gy * gy);
    l.ring = gradient > 1e-6f ? std::fabs(g) / gradient : 1e9f;
    l.radius = std::sqrt(dx * dx + dy * dy);
    return l;
}

} // namespace

image::Pixels render(int size, float ring_alpha)
{
    image::Pixels out;
    if (size <= 0)
        return out;
    out.width = size;
    out.height = size;
    out.rgba.assign(static_cast<std::size_t>(size) * size * 4, 0);
    const float scale = 512.0f / static_cast<float>(size);
    constexpr int kSamples = 4;
    for (int py = 0; py < size; ++py)
    {
        for (int px = 0; px < size; ++px)
        {
            float coverage = 0.0f;
            for (int sy = 0; sy < kSamples; ++sy)
            {
                for (int sx = 0; sx < kSamples; ++sx)
                {
                    const float u = (static_cast<float>(px) + (sx + 0.5f) / kSamples) * scale;
                    const float v = (static_cast<float>(py) + (sy + 0.5f) / kSamples) * scale;
                    const Local l = locate(u, v);
                    const bool front = l.y > 0.0f;
                    const bool on_ring = l.ring <= kStroke;
                    const bool planet = l.radius <= kPlanet && !(front && l.ring <= kGap);
                    if (planet)
                        coverage += 1.0f;
                    else if (on_ring && (front || l.radius > kHidden))
                        coverage += ring_alpha;
                }
            }
            coverage /= kSamples * kSamples;
            std::uint8_t *pixel = out.rgba.data() + (static_cast<std::size_t>(py) * size + px) * 4;
            pixel[0] = pixel[1] = pixel[2] = 255;
            pixel[3] =
                static_cast<std::uint8_t>(std::clamp(coverage * 255.0f + 0.5f, 0.0f, 255.0f));
        }
    }
    return out;
}

void draw_light(hui::gfx::DrawList &list, float x, float y, float size, float phase, float opacity)
{
    const float scale = size / 512.0f;
    constexpr int kTrail = 14;
    for (int k = kTrail - 1; k >= 0; --k)
    {
        float p = phase - static_cast<float>(k) * 0.011f;
        p -= std::floor(p);
        // Clockwise as seen on screen: over the top behind the planet, then
        // across the front.
        const float t = p * 6.2831853f + 3.1415927f;
        const float lx = kRx * std::cos(t);
        const float ly = kRy * std::sin(t);
        const float u = kCx + lx * kE1x + ly * kE2x;
        const float v = kCy + lx * kE1y + ly * kE2y;
        const bool front = ly > 0.0f;
        const float radius = std::sqrt((u - kCx) * (u - kCx) + (v - kCy) * (v - kCy));
        if (!front && radius < kHidden)
            continue;
        const float fade = 1.0f - static_cast<float>(k) / kTrail;
        const float alpha = fade * fade * opacity;
        const float r = kStroke * scale * (0.55f + 0.45f * fade);
        const float cx = x + u * scale;
        const float cy = y + v * scale;
        if (k == 0)
            list.glow({cx - r, cy - r, 2 * r, 2 * r}, r, r * 2.6f,
                      hui::gfx::Color::rgb(0xffffff, 0.55f * opacity));
        list.circle(cx, cy, r, hui::gfx::Color::rgb(0xffffff, alpha));
    }
}

} // namespace orbit::mark
