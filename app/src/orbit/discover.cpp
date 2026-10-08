// Orbit Store TV app - Discover: featured art, latest releases and a paged game grid.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "orbit/screens.hpp"
#include "orbit/i18n.hpp"

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

namespace orbit
{

namespace
{

// Refined Discover: a fixed hero above six covers, with 22px gutters.
constexpr float kTile = 268.333f;
constexpr float kTileGap = 22.0f;
constexpr float kStep = kTile + kTileGap;
constexpr int kColumns = 6;
constexpr int kPageSize = 96;
constexpr float kGridRowStep = 338.0f;
constexpr float kFirstRail = 615.0f;  // first rail's heading baseline
constexpr float kRailPitch = 420.0f;  // heading to heading
constexpr float kCoversBelow = 27.0f; // heading baseline to the covers
constexpr float kHeroBottom = 540.0f; // the View game button's lower edge
constexpr float kTitleSize = 73.0f;   // shared semibold hero title
constexpr float kTitleLine = 80.0f;   // two-line title leading
constexpr float kTitleWidth = 640.0f; // title width

// The title's lines, balanced as text-wrap: balance does (the narrowest
// width that keeps the same number of lines), at most two: a longer title
// ends its second line with an ellipsis (main's -webkit-line-clamp: 2).
std::vector<std::string> hero_lines(const hui::ui::FontRef &font, const std::string &title)
{
    std::vector<std::string> lines = font.font->wrap(title, kTitleSize, kTitleWidth);
    if (lines.size() > 2)
    {
        std::string rest = lines[1];
        for (std::size_t i = 2; i < lines.size(); ++i)
            rest += " " + lines[i];
        lines.resize(2);
        lines[1] = font.font->fit(rest + " \xE2\x80\xA6", kTitleSize, kTitleWidth);
        return lines;
    }
    if (lines.size() < 2)
        return lines;
    for (float width = kTitleWidth - 20.0f; width > 300.0f; width -= 20.0f)
    {
        std::vector<std::string> narrower = font.font->wrap(title, kTitleSize, width);
        if (narrower.size() != lines.size())
            break;
        lines = std::move(narrower);
    }
    return lines;
}

} // namespace

DiscoverScreen::DiscoverScreen(Context &context) : Screen(context)
{
    hero_in_.snap(1.0f);
}

void DiscoverScreen::rebuild()
{
    const Snapshot &state = ctx_.store.state();
    catalog_serial_ = state.catalog_serial;
    rails_.clear();
    Rail latest;
    latest.title = tr("Latest releases");
    latest.note = tr("Newest first");
    latest.games = latest_games(state.games, ctx_.today);
    if (!latest.games.empty())
        rails_.push_back(std::move(latest));
    Rail added;
    added.title = tr("New on Orbit");
    added.games = newly_added_games(state.games);
    if (!added.games.empty())
        rails_.push_back(std::move(added));
    Rail all;
    all.title = tr("All games");
    all.grid = true;
    all.note = trn(static_cast<long>(state.games.size()), "{count} game", "{count} games");
    BrowseFilters all_filters;
    all_filters.sort = Sort::size_desc;
    all_games_ = browse(state.games, all_filters, {});
    if (!all_games_.empty())
        rails_.push_back(std::move(all));

    rebuild_grid();

    columns_.resize(rails_.size(), 0);
    rail_scroll_.resize(rails_.size());
    tile_focus_.resize(rails_.size());
    for (std::size_t r = 0; r < rails_.size(); ++r)
    {
        columns_[r] =
            std::clamp(columns_[r], 0, std::max(0, static_cast<int>(rails_[r].games.size()) - 1));
        tile_focus_[r].assign(rails_[r].games.size(), 0.0f);
    }
    row_ = std::min(row_, static_cast<int>(rails_.size()));
    if (state.game(featured_) == nullptr)
    {
        featured_ = rails_.empty() || rails_[0].games.empty()
                        ? std::string()
                        : state.games[static_cast<std::size_t>(rails_[0].games[0])].id;
        hero_in_.snap(1.0f);
    }
}

int DiscoverScreen::page_count() const
{
    return std::max(1, (static_cast<int>(all_games_.size()) + kPageSize - 1) / kPageSize);
}

bool DiscoverScreen::on_pager() const
{
    return page_count() > 1 && row_ == static_cast<int>(rails_.size()) + 1;
}

void DiscoverScreen::rebuild_grid()
{
    page_ = std::clamp(page_, 0, page_count() - 1);
    if (rails_.empty() || !rails_.back().grid)
        return;
    Rail &all = rails_.back();
    const int first = page_ * kPageSize;
    const int end = std::min(first + kPageSize, static_cast<int>(all_games_.size()));
    all.games.assign(all_games_.begin() + first, all_games_.begin() + end);
}

const Game *DiscoverScreen::game_at(int row, int column) const
{
    if (row < 1 || row > static_cast<int>(rails_.size()))
        return nullptr;
    const Rail &rail = rails_[static_cast<std::size_t>(row - 1)];
    if (column < 0 || column >= static_cast<int>(rail.games.size()))
        return nullptr;
    const std::vector<Game> &games = ctx_.store.state().games;
    const int index = rail.games[static_cast<std::size_t>(column)];
    return index >= 0 && index < static_cast<int>(games.size())
               ? &games[static_cast<std::size_t>(index)]
               : nullptr;
}

float DiscoverScreen::row_top(int row) const
{
    if (rails_.empty() || row <= 0)
        return 0.0f;
    if (row < static_cast<int>(rails_.size()))
        return kRailPitch * static_cast<float>(row - 1);
    const float top = kFirstRail + kRailPitch * static_cast<float>(rails_.size() - 1) + kCoversBelow;
    const int rows = (static_cast<int>(rails_.back().games.size()) + kColumns - 1) / kColumns;
    const float bottom = top + static_cast<float>(rows) * kGridRowStep + (page_count() > 1 ? 100.0f : 0.0f);
    const float wanted = on_pager() ? bottom - 950.0f
                                   : top + static_cast<float>(columns_.back() / kColumns) * kGridRowStep - 308.0f;
    return std::clamp(wanted, 0.0f, std::max(0.0f, bottom - 980.0f));
}

void DiscoverScreen::focus()
{
    if (catalog_serial_ != ctx_.store.state().catalog_serial)
        rebuild();
}

void DiscoverScreen::return_to(const std::string &game_id)
{
    if (row_ >= 1 && row_ <= static_cast<int>(rails_.size()))
    {
        const Game *game = game_at(row_, columns_[static_cast<std::size_t>(row_ - 1)]);
        if (game != nullptr && game->id == game_id)
            return;
    }
    featured_ = game_id;
}

Intent DiscoverScreen::update(const InputFrame &input, float dt, Feedback &feedback, bool focused)
{
    Intent intent;
    if (catalog_serial_ != ctx_.store.state().catalog_serial)
        rebuild();

    if (focused && !rails_.empty())
    {
        if (on_pager())
        {
            if (input.nav == Direction::up)
            {
                --row_;
                feedback.play(hui::audio::Cue::focus);
            }
            else if (input.nav == Direction::left || input.nav == Direction::right)
            {
                const int next = input.nav == Direction::right ? 1 : 0;
                const bool enabled = next == 0 ? page_ > 0 : page_ + 1 < page_count();
                if (next != page_button_ && enabled)
                {
                    page_button_ = next;
                    feedback.play(hui::audio::Cue::focus);
                }
                else
                    refuse(feedback, input);
            }
            else if (input.nav == Direction::down)
                refuse(feedback, input);
            if (input.is_pressed(Action::confirm))
            {
                const int next = page_ + (page_button_ == 1 ? 1 : -1);
                if (next >= 0 && next < page_count())
                {
                    page_ = next;
                    rebuild_grid();
                    columns_.back() = 0;
                    tile_focus_.back().assign(rails_.back().games.size(), 0.0f);
                    row_ = static_cast<int>(rails_.size());
                    const Game *first = game_at(row_, 0);
                    if (first != nullptr)
                        featured_ = first->id;
                    scroll_.snap(row_top(row_));
                    feedback.play(hui::audio::Cue::select);
                }
                else
                    refuse(feedback, input);
                return intent; // changing a page must never open its first game
            }
        }
        else if (row_ >= 1 && rails_[static_cast<std::size_t>(row_ - 1)].grid)
        {
            int &index = columns_.back();
            const int count = static_cast<int>(rails_.back().games.size());
            int next = index;
            if (input.nav == Direction::up && index < kColumns)
            {
                --row_;
                feedback.play(hui::audio::Cue::focus);
            }
            else if (input.nav == Direction::down && index / kColumns == (count - 1) / kColumns)
            {
                if (page_count() > 1)
                {
                    ++row_;
                    page_button_ = page_ + 1 < page_count() ? 1 : 0;
                    feedback.play(hui::audio::Cue::focus);
                }
                else
                    refuse(feedback, input);
            }
            else if (input.nav != Direction::none)
            {
                if (input.nav == Direction::left)
                    next = index % kColumns == 0 ? -1 : index - 1;
                else if (input.nav == Direction::right)
                    next = index % kColumns == kColumns - 1 ? -1 : index + 1;
                else if (input.nav == Direction::up)
                    next = index - kColumns;
                else if (input.nav == Direction::down)
                    next = std::min(index + kColumns, count - 1);
                if (next >= 0 && next < count)
                {
                    index = next;
                    feedback.play(hui::audio::Cue::focus);
                }
                else
                    refuse(feedback, input);
            }
        }
        else if (input.nav == Direction::up)
        {
            if (row_ == 0)
            {
                intent.kind = Intent::Kind::top_bar;
                return intent;
            }
            --row_;
            feedback.play(hui::audio::Cue::focus);
        }
        else if (input.nav == Direction::down)
        {
            if (row_ < static_cast<int>(rails_.size()))
            {
                ++row_;
                feedback.play(hui::audio::Cue::focus);
            }
            else
            {
                refuse(feedback, input);
            }
        }
        else if ((input.nav == Direction::left || input.nav == Direction::right) && row_ >= 1)
        {
            int &column = columns_[static_cast<std::size_t>(row_ - 1)];
            const int next = column + (input.nav == Direction::right ? 1 : -1);
            const int count =
                static_cast<int>(rails_[static_cast<std::size_t>(row_ - 1)].games.size());
            if (next >= 0 && next < count)
            {
                column = next;
                feedback.play(hui::audio::Cue::focus, 1.0f, 0.0f, 0.8f);
            }
            else
            {
                refuse(feedback, input);
            }
        }
        else if (input.nav == Direction::left || input.nav == Direction::right)
        {
            const int next = input.nav == Direction::right ? 1 : 0;
            if (next == hero_action_)
                refuse(feedback, input);
            else
            {
                hero_action_ = next;
                feedback.play(hui::audio::Cue::focus);
            }
        }

        if (row_ >= 1 && !on_pager())
        {
            // The hero follows the focus; nothing changes it otherwise.
            const Game *game = game_at(row_, columns_[static_cast<std::size_t>(row_ - 1)]);
            if (game != nullptr && game->id != featured_)
            {
                previous_featured_ = featured_;
                featured_ = game->id;
                hero_in_.snap(0.0f);
            }
        }

        if (input.is_pressed(Action::confirm) && !on_pager())
        {
            const Game *game = row_ == 0
                                   ? ctx_.store.state().game(featured_)
                                   : game_at(row_, columns_[static_cast<std::size_t>(row_ - 1)]);
            if (game != nullptr)
            {
                if (row_ == 0 && hero_action_ == 1)
                {
                    if (!ctx_.store.favourite_action().busy)
                    {
                        ctx_.store.set_favourite(game->id, !ctx_.store.state().favourite(game->id));
                        feedback.play(hui::audio::Cue::toggle);
                    }
                    else
                        refuse(feedback, input);
                    return intent;
                }
                feedback.play(hui::audio::Cue::select);
                intent.kind = Intent::Kind::open_game;
                intent.id = game->id;
                return intent;
            }
        }
    }
    else if (focused && input.nav == Direction::up)
    {
        intent.kind = Intent::Kind::top_bar;
        return intent;
    }

    // Animation.
    hero_in_.target = 1.0f;
    hero_in_.update(dt, 7.0f);
    scroll_.target = row_top(row_);
    scroll_.update(dt, 10.0f);
    button_focus_.target = focused && row_ == 0 && hero_action_ == 0 ? 1.0f : 0.0f;
    button_focus_.update(dt, 16.0f);
    favourite_focus_.target = focused && row_ == 0 && hero_action_ == 1 ? 1.0f : 0.0f;
    favourite_focus_.update(dt, 16.0f);
    for (std::size_t r = 0; r < rails_.size(); ++r)
    {
        const int count = static_cast<int>(rails_[r].games.size());
        const float max_scroll =
            std::max(0.0f, count * kStep - kTileGap - (1920.0f - 2.0f * ui::kGutter));
        rail_scroll_[r].target = rails_[r].grid ? 0.0f :
            std::clamp(static_cast<float>(columns_[r] - 2) * kStep, 0.0f, max_scroll);
        rail_scroll_[r].update(dt, 11.0f);
        for (int i = 0; i < count; ++i)
        {
            const bool on = focused && row_ == static_cast<int>(r) + 1 && columns_[r] == i;
            float &amount = tile_focus_[r][static_cast<std::size_t>(i)];
            amount += ((on ? 1.0f : 0.0f) - amount) * std::min(1.0f, dt * 16.0f);
        }
    }
    return intent;
}

void DiscoverScreen::draw(Frame &frame, bool focused) const
{
    (void)focused;
    hui::gfx::DrawList &list = frame.scene;
    const Snapshot &state = ctx_.store.state();
    const ui::Type &type = ctx_.type;
    const float scroll = scroll_.value;

    // ---- the featured game, bottom-aligned on the first rail ----
    const Game *game = state.game(featured_);
    const float hero_alpha = std::clamp(1.0f - scroll / kRailPitch, 0.0f, 1.0f);
    if (game != nullptr && hero_alpha > 0.01f)
    {
        const float left = ui::kGutter;
        const float button_top = kHeroBottom - 60.0f;
        const float in = hero_in_.value;
        list.push_opacity(hero_alpha * in);
        list.push_transform(1.0f, 0.0f, 0.0f, 0.0f, -scroll);
        ui::text(list, type.medium, tr("DISCOVER"), left, 166.0f, 16.0f, ui::color::text2);
        const std::vector<std::string> lines = hero_lines(type.semibold, game->title);
        for (std::size_t i = 0; i < lines.size(); ++i)
            ui::text(list, type.semibold, lines[i], left,
                     254.0f + kTitleLine * static_cast<float>(i), kTitleSize, ui::color::text);
        ui::text_fit(list, type.regular, game->tagline, left, 454.0f, 23.0f, 880.0f,
                     ui::color::text2);
        list.pop_transform();
        list.pop_opacity();
        list.push_opacity(hero_alpha);
        ui::button(list, type, {left, button_top - scroll, 234.0f, 60.0f}, tr("View game"),
                   ui::ButtonKind::light, button_focus_.value);
        const ui::Rect favourite{left + 252.0f, button_top - scroll, 60.0f, 60.0f};
        ui::focus_ring(list, favourite, 30.0f, favourite_focus_.value);
        list.bordered_rect(favourite, 30.0f, ui::color::night1.with_alpha(0.45f), 1.5f,
                           ui::color::text2.with_alpha(0.6f));
        ui::icon_heart(list, favourite.cx(), favourite.cy(), 25.0f,
                       state.favourite(game->id) ? ui::color::action_hi : ui::color::text);
        if (!ctx_.store.favourite_action().error.empty())
            ui::text_fit(list, type.regular, tr(ctx_.store.favourite_action().error), left,
                         kHeroBottom + 26.0f - scroll, 16.0f, 840.0f, ui::color::bad);
        list.pop_opacity();
    }

    // ---- rails ----
    for (std::size_t r = 0; r < rails_.size(); ++r)
    {
        const Rail &rail = rails_[r];
        const float base = kFirstRail + kRailPitch * static_cast<float>(r) - scroll;
        if (rail.grid)
        {
            list.push_clip({0.0f, 106.0f, 1920.0f, 926.0f});
            if (base >= 106.0f && base < 1080.0f)
            {
                ui::text(list, type.semibold, rail.title, ui::kGutter, base, 28.0f, ui::color::text);
                ui::text(list, type.regular, rail.note, ui::kGutter + type.semibold.font->measure(rail.title, 28.0f) + 24.0f, base, 18.0f, ui::color::text3);
            }
            const int count = static_cast<int>(rail.games.size());
            const float top = base + kCoversBelow;
            // Only the rows intersecting the viewport are submitted to the GPU.
            for (int pass = 0; pass < 2; ++pass)
            {
                for (int i = 0; i < count; ++i)
                {
                    const float y = top + static_cast<float>(i / kColumns) * kGridRowStep;
                    if (y > 1032.0f || y + kGridRowStep < 106.0f)
                        continue;
                    const float amount = tile_focus_[r][static_cast<std::size_t>(i)];
                    if ((pass == 0) == (amount > 0.5f))
                        continue;
                    const float x = ui::kGutter + static_cast<float>(i % kColumns) * kStep;
                    const Game &tile_game = state.games[static_cast<std::size_t>(rail.games[static_cast<std::size_t>(i)])];
                    draw_tile(ctx_, list, tile_game, {x, y, kTile, kTile}, amount);
                    if (state.favourite(tile_game.id))
                    {
                        list.circle(x + kTile - 25.0f, y + 25.0f, 15.0f, ui::Color::rgb(0x141414, 0.87f));
                        list.star(x + kTile - 25.0f, y + 25.5f, 8.0f, ui::Color::rgb(0xffffff));
                    }
                    draw_tile_caption(ctx_, list, tile_game,
                                      game_state(tile_game, state.jobs, state.library_games()).label,
                                      x, y + kTile, kTile, amount, false);
                }
            }
            if (page_count() > 1)
            {
                const float y = top + static_cast<float>((count + kColumns - 1) / kColumns) * kGridRowStep + 10.0f;
                if (y >= 106.0f && y < 1032.0f)
                {
                    ui::button(list, type, {620.0f, y, 190.0f, 58.0f}, tr("Previous"), ui::ButtonKind::secondary,
                               focused && on_pager() && page_button_ == 0 ? 1.0f : 0.0f, page_ > 0);
                    const std::string page = tr("Page {page} of {pages}", {{"page", i18n::count(page_ + 1)}, {"pages", i18n::count(page_count())}});
                    ui::text(list, type.regular, page, 960.0f, y + 36.0f, 20.0f, ui::color::text2, ui::Align::center);
                    ui::button(list, type, {1110.0f, y, 190.0f, 58.0f}, tr("Next"), ui::ButtonKind::secondary,
                               focused && on_pager() && page_button_ == 1 ? 1.0f : 0.0f, page_ + 1 < page_count());
                }
            }
            list.pop_clip();
            continue;
        }
        // The next rail peeks in below, as on the web; rails scrolled past go.
        if (base < 60.0f || base > 1080.0f)
            continue;
        ui::text(list, type.semibold, rail.title, ui::kGutter, base, 28.0f, ui::color::text);

        const float top = base + kCoversBelow;
        list.push_clip({0.0f, top - 60.0f, 1920.0f, 1080.0f - (top - 60.0f)});
        const float offset = rail_scroll_[r].value;
        const bool dated = false;
        // Draw the focused tile last so its growth and ring sit on top.
        for (int pass = 0; pass < 2; ++pass)
        {
            for (int i = 0; i < static_cast<int>(rail.games.size()); ++i)
            {
                const float x = ui::kGutter + static_cast<float>(i) * kStep - offset;
                if (x < -kStep || x > 1920.0f)
                    continue;
                const float amount = tile_focus_[r][static_cast<std::size_t>(i)];
                if ((pass == 0) == (amount > 0.5f))
                    continue;
                const Game &tile_game =
                    state.games[static_cast<std::size_t>(rail.games[static_cast<std::size_t>(i)])];
                const float current = row_ == 0 && tile_game.id == featured_ ? 1.0f : 0.0f;
                draw_tile(ctx_, list, tile_game, {x, top, kTile, kTile}, amount, current);
                draw_tile_caption(ctx_, list, tile_game,
                                  game_state(tile_game, state.jobs, state.library_games()).label, x,
                                  top + kTile, kTile, amount, dated);
            }
        }
        list.pop_clip();
    }
}

std::vector<hui::ui::Hint> DiscoverScreen::hints() const
{
    return {{hui::ui::Button::cross, on_pager() ? tr("Change page") : tr("Open game")},
            {hui::ui::Button::l1, tr("Tabs"), hui::ui::Button::r1}};
}

} // namespace orbit
