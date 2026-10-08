// Orbit Store TV app - Drawing shared by the screens: covers, tiles, backdrops.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "orbit/screens.hpp"

#include <algorithm>

namespace orbit
{

namespace
{

// New art fades in over this long instead of popping.
constexpr float kArtFade = 0.25f;

float art_alpha(const Context &ctx, const Artwork &art)
{
    (void)ctx;
    const double age = now_seconds() - art.ready_at;
    return std::clamp(static_cast<float>(age / kArtFade), 0.0f, 1.0f);
}

} // namespace

void draw_cover(Context &ctx, hui::gfx::DrawList &list, const Game &game, const ui::Rect &r,
                float radius, float opacity)
{
    const Artwork *art = ctx.art.get(game.id, api::ArtKind::cover);
    const float alpha = art != nullptr ? art_alpha(ctx, *art) : 0.0f;
    if (alpha < 1.0f)
    {
        list.push_opacity(opacity);
        ui::cover_placeholder(list, ctx.type, r, game.title, radius);
        list.pop_opacity();
    }
    if (art != nullptr)
        ui::image_cover(list, art->texture, art->width, art->height, r,
                        ui::Color::rgb(0xffffff, alpha * opacity), radius);
}

void draw_tile(Context &ctx, hui::gfx::DrawList &list, const Game &game, const ui::Rect &r,
               float focus, float current, float opacity)
{
    // Fixed-size covers preserve the grid; the ring carries focus.
    const float scale = 1.0f;
    list.push_transform(scale, r.cx(), r.cy(), 0.0f, 0.0f);
    // The ring first: its glow belongs behind the art, as a box-shadow is.
    ui::focus_ring(list, r, ui::kArtRadius, focus * opacity);
    draw_cover(ctx, list, game, r, ui::kArtRadius, opacity);
    // The game the hero shows, while the focus is elsewhere.
    const float outline = current * (1.0f - focus) * opacity;
    if (outline > 0.01f)
        list.bordered_rect(r.inset(-2.0f), ui::kArtRadius + 2.0f, ui::Color{0, 0, 0, 0}, 2.0f,
                           ui::Color::rgb(0xffffff, 0.75f * outline));
    list.pop_transform();
}

float draw_tile_caption(Context &ctx, hui::gfx::DrawList &list, const Game &game, const char *state,
                        float x, float cover_bottom, float width, float focus, bool date)
{
    // .tile-title (0.95 rem, 1.3 line height, secondary until focused), then
    // .tile-collection-state and .tile-date.
    const ui::Type &type = ctx.type;
    const ui::Color ink = hui::gfx::mix(ui::color::text2, ui::color::text, focus);
    float y = ui::paragraph(list, type.regular, game.title, x, cover_bottom + 30.0f, 20.0f, width,
                            27.0f, ink, 1);
    if (state != nullptr && state[0] != '\0')
    {
        ui::text(list, type.medium, state, x, y + 2.0f, 15.0f, ui::Color::rgb(0xd0d0d0));
        y += 22.0f;
    }
    if (date && !game.release_date.empty())
    {
        ui::text(list, type.regular, format_date(game.release_date), x, y - 1.0f, 15.6f,
                 ui::color::text3);
        y += 24.0f;
    }
    return y;
}

void draw_backdrop_art(Context &ctx, hui::gfx::DrawList &list, const Game &game, float opacity,
                       float height)
{
    if (opacity <= 0.01f)
        return;
    const ui::Rect screen{0.0f, 0.0f, 1920.0f, height};
    // As the browser's Backdrop: the cover becomes a colour wash (blurred,
    // 75 %), and a wide banner, where the game has one, replaces it.
    const Artwork *hero = ctx.art.get(game.id, api::ArtKind::hero);
    const bool wide = hero != nullptr && hero->width >= hero->height * 1.4f;
    const float hero_alpha = wide ? art_alpha(ctx, *hero) : 0.0f;
    const Artwork *cover = ctx.art.get(game.id, api::ArtKind::cover);
    if (cover != nullptr && cover->ambient != 0 && hero_alpha < 1.0f)
    {
        const float alpha = art_alpha(ctx, *cover) * opacity * 0.75f;
        ui::image_cover(list, cover->ambient, 48, 48, screen.inset(-230.0f),
                        ui::Color::rgb(0xffffff, alpha));
    }
    if (wide)
        ui::image_cover(list, hero->texture, hero->width, hero->height, screen,
                        ui::Color::rgb(0xffffff, hero_alpha * opacity), 0.0f, 0.72f, 0.3f);
}

void draw_veils(hui::gfx::DrawList &list, bool discover)
{
    if (discover)
    {
        list.gradient_rect_h({0.0f, 0.0f, 540.0f, 800.0f}, 0.0f, ui::color::night1,
                             ui::color::night1.with_alpha(0.85f));
        list.gradient_rect_h({540.0f, 0.0f, 700.0f, 800.0f}, 0.0f,
                             ui::color::night1.with_alpha(0.85f),
                             ui::color::night1.with_alpha(0.0f));
        list.gradient_rect({0.0f, 460.0f, 1920.0f, 340.0f}, 0.0f,
                           ui::color::night0.with_alpha(0.0f), ui::color::night0);
        list.rounded_rect({0.0f, 800.0f, 1920.0f, 280.0f}, 0.0f, ui::color::night0);
        return;
    }
    // Same wide-art canvas as the browser game hub, with readable left/bottom fades.
    list.gradient_rect_h({0, 0, 557, 1080}, 0, ui::color::night0.with_alpha(0.87f),
                         ui::color::night0.with_alpha(0.60f));
    list.gradient_rect_h({557, 0, 634, 1080}, 0, ui::color::night0.with_alpha(0.60f),
                         ui::color::night0.with_alpha(0.0f));
    list.gradient_rect({0, 497, 1920, 421}, 0, ui::color::night0.with_alpha(0.0f),
                       ui::color::night0.with_alpha(0.67f));
    list.gradient_rect({0, 918, 1920, 162}, 0, ui::color::night0.with_alpha(0.67f), ui::color::night0);

}

int Picker::update(const InputFrame &input, int count, Feedback &feedback)
{
    if (!open || count <= 0)
        return -1;
    active = std::clamp(active, 0, count - 1);
    if (input.nav == Direction::up || input.nav == Direction::down)
    {
        const int next = active + (input.nav == Direction::down ? 1 : -1);
        if (next >= 0 && next < count)
        {
            active = next;
            feedback.play(hui::audio::Cue::focus, 1.0f, 0.0f, 0.8f);
        }
        else
        {
            refuse(feedback, input);
        }
    }
    if (input.is_pressed(Action::confirm))
    {
        open = false;
        feedback.play(hui::audio::Cue::select);
        return active;
    }
    if (input.is_pressed(Action::back))
    {
        open = false;
        feedback.play(hui::audio::Cue::modal_close);
    }
    return -1;
}

void Picker::animate(float dt)
{
    in.target = open ? 1.0f : 0.0f;
    in.update(dt, 20.0f);
}

void refuse(Feedback &feedback, const InputFrame &input)
{
    if (!input.nav_repeat)
        feedback.play(hui::audio::Cue::error, 1.0f, 0.0f, 0.45f);
}

hui::ui::Theme keyboard_theme()
{
    hui::ui::Theme theme = hui::ui::themes()[0];
    for (const hui::ui::Theme &candidate : hui::ui::themes())
    {
        if (std::string_view(candidate.id) == "acrylic")
            theme = candidate;
    }
    theme.page = ui::color::night1;
    theme.surface = ui::color::night2.with_alpha(0.82f);
    theme.surface_high = ui::Color::rgb(0xffffff, 0.1f);
    theme.text = ui::color::text;
    theme.text_muted = ui::color::text2;
    theme.primary = ui::color::action;
    theme.on_primary = ui::color::night1;
    theme.secondary = ui::Color::rgb(0xffffff, 0.1f);
    theme.accent = ui::color::action;
    theme.focus = ui::color::text;
    theme.radius = 12.0f;
    theme.radius_card = ui::kPanelRadius;
    // Solid keys: glass keys would pick up the colours of the covers behind.
    theme.style = hui::ui::SurfaceStyle::flat;
    theme.surface_high = ui::color::night3;
    theme.secondary = ui::color::night3;
    return theme;
}

} // namespace orbit
