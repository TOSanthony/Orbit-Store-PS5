// Orbit Store TV app - Browse: search, filters and every game as a grid.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The browser storefront's Browse (src/App.tsx, src/browse.ts): a search by
// name or title ID, All games or Favourites, then Source, Format, Region, Download
// size and Sort by, and Reset filters. Each filter is a dropdown here too.

#include "orbit/screens.hpp"
#include "orbit/i18n.hpp"

#include <algorithm>
#include <cstdio>

namespace orbit
{

namespace
{

// Shared 1920px geometry: six square covers and one compact toolbar.
constexpr int kColumns = 6;
constexpr float kTile = 268.333f;
constexpr float kColumnStep = (1920.0f - 2.0f * ui::kGutter - kTile) / (kColumns - 1);
constexpr float kRowStep = 338.0f;
constexpr float kTitleBaseline = 176.0f; // semibold page title
constexpr float kFieldY = 122.0f;
constexpr float kFieldWidth = 660.0f; // search beside the heading
constexpr float kFieldHeight = 66.0f;
constexpr float kChipsY = 218.0f; // collection tabs share the filter row
constexpr float kChipHeight = 58.0f;
constexpr float kFiltersY = 216.0f; // .browse-selects
constexpr float kResetWidth = 152.0f;
constexpr float kFilterGap = 20.0f;
constexpr float kFilterWidth = (1920.0f - ui::kGutter - 554.0f - 4.0f * kFilterGap) / 5.0f;
constexpr float kGridTop = 308.0f;
constexpr float kKeyboardTop = 560.0f;
constexpr const char *kRegions[] = {"", "EUR", "USA", "JPN", "ASIA", "unknown"};
constexpr const char *kChips[] = {/* i18n */ "All games", /* i18n */ "Favourites"};

// Removes the last UTF-8 character.
void pop_character(std::string &text)
{
    while (!text.empty())
    {
        const unsigned char c = static_cast<unsigned char>(text.back());
        text.pop_back();
        if ((c & 0xC0) != 0x80)
            break;
    }
}

float chip_width(const ui::Type &type, int chip)
{
    (void)type;
    (void)chip;
    return 185.0f;
}

} // namespace

BrowseScreen::BrowseScreen(Context &context) : Screen(context)
{
    keyboard_.style.theme = keyboard_theme();
    keyboard_.style.bindings = hui::ui::KeyboardBindings::standard();
    keyboard_.style.max_length = 40;
    keyboard_.style.done_label = tr("Done");
    keyboard_.set_bounds({ui::kGutter + 220.0f, kKeyboardTop + 40.0f, 1280.0f, 400.0f});
}

void BrowseScreen::refilter()
{
    const Snapshot &state = ctx_.store.state();
    catalog_serial_ = state.catalog_serial;
    favourites_serial_ = state.serial;
    results_ = browse(state.games, filters_, state.favourites);
    tile_focus_.assign(results_.size(), 0.0f);
    index_ = std::clamp(index_, 0, std::max(0, static_cast<int>(results_.size()) - 1));
}

ui::Rect BrowseScreen::tile_rect(int index) const
{
    const int row = index / kColumns;
    const int column = index % kColumns;
    return {ui::kGutter + static_cast<float>(column) * kColumnStep,
            kGridTop + static_cast<float>(row) * kRowStep, kTile, kTile};
}

ui::Rect BrowseScreen::filter_rect(int index) const
{
    if (index >= 5)
        return {970.0f, 134.0f, kResetWidth, 44.0f};
    return {554.0f + static_cast<float>(index) * (kFilterWidth + kFilterGap), kFiltersY,
            kFilterWidth, ui::kFieldHeight};
}

std::vector<std::string> BrowseScreen::filter_options(int filter, int *selected) const
{
    const std::vector<Game> &games = ctx_.store.state().games;
    std::vector<std::string> options;
    *selected = 0;
    switch (filter)
    {
    case 0:
    {
        options.push_back(tr("All sources"));
        const auto sources = source_choices(games);
        for (std::size_t i = 0; i < sources.size(); ++i)
        {
            options.push_back(sources[i].second);
            if (sources[i].first == filters_.source)
                *selected = static_cast<int>(i) + 1;
        }
        break;
    }
    case 1:
    {
        options.push_back(tr("All formats"));
        const auto formats = format_choices(games);
        for (std::size_t i = 0; i < formats.size(); ++i)
        {
            options.push_back(formats[i]);
            if (formats[i] == filters_.format)
                *selected = static_cast<int>(i) + 1;
        }
        break;
    }
    case 2:
        options = {tr("All regions"), "EUR", "USA", "JPN", "ASIA", tr("Unknown region")};
        for (int i = 0; i < 6; ++i)
            if (filters_.region == kRegions[i])
                *selected = i;
        break;
    case 3:
        for (int i = 0; i < static_cast<int>(SizeFilter::count); ++i)
            options.push_back(size_label(static_cast<SizeFilter>(i)));
        *selected = static_cast<int>(filters_.size);
        break;
    default:
        for (int i = 0; i < static_cast<int>(Sort::count); ++i)
            options.push_back(sort_label(static_cast<Sort>(i)));
        *selected = static_cast<int>(filters_.sort);
        break;
    }
    return options;
}

std::string BrowseScreen::filter_value(int filter) const
{
    int selected = 0;
    const std::vector<std::string> options = filter_options(filter, &selected);
    return selected >= 0 && selected < static_cast<int>(options.size())
               ? options[static_cast<std::size_t>(selected)]
               : std::string();
}

void BrowseScreen::choose(int filter, int option)
{
    const std::vector<Game> &games = ctx_.store.state().games;
    switch (filter)
    {
    case 0:
    {
        const auto sources = source_choices(games);
        filters_.source = option > 0 && option <= static_cast<int>(sources.size())
                              ? sources[static_cast<std::size_t>(option - 1)].first
                              : std::string();
        break;
    }
    case 1:
    {
        const auto formats = format_choices(games);
        filters_.format = option > 0 && option <= static_cast<int>(formats.size())
                              ? formats[static_cast<std::size_t>(option - 1)]
                              : std::string();
        break;
    }
    case 2:
        filters_.region = kRegions[std::clamp(option, 0, 5)];
        break;
    case 3:
        filters_.size =
            static_cast<SizeFilter>(std::clamp(option, 0, static_cast<int>(SizeFilter::count) - 1));
        break;
    default:
        filters_.sort = static_cast<Sort>(std::clamp(option, 0, static_cast<int>(Sort::count) - 1));
        break;
    }
    index_ = 0;
    refilter();
}

void BrowseScreen::focus()
{
    if (catalog_serial_ != ctx_.store.state().catalog_serial)
        refilter();
}

void BrowseScreen::start_search()
{
    area_ = Area::field;
    keyboard_open_ = true;
    keyboard_.enter();
    keyboard_.set_length(static_cast<int>(filters_.query.size()));
}

void BrowseScreen::set_query(const std::string &query)
{
    filters_.query = query;
    index_ = 0;
    refilter();
    keyboard_.set_length(static_cast<int>(filters_.query.size()));
}

void BrowseScreen::return_to(const std::string &game_id)
{
    const std::vector<Game> &games = ctx_.store.state().games;
    for (int i = 0; i < static_cast<int>(results_.size()); ++i)
    {
        if (games[static_cast<std::size_t>(results_[static_cast<std::size_t>(i)])].id == game_id)
        {
            index_ = i;
            area_ = Area::grid;
            return;
        }
    }
}

Intent BrowseScreen::update(const InputFrame &input, float dt, Feedback &feedback, bool focused)
{
    Intent intent;
    const Snapshot &state = ctx_.store.state();
    if (catalog_serial_ != state.catalog_serial ||
        (filters_.favourites && favourites_serial_ != state.serial))
        refilter();

    if (focused && keyboard_open_)
    {
        const hui::ui::Event event = keyboard_.handle(input, feedback);
        bool changed = false;
        for (int i = 0; i < keyboard_.erased(); ++i)
        {
            pop_character(filters_.query);
            changed = true;
        }
        if (!keyboard_.typed().empty())
        {
            filters_.query += keyboard_.typed();
            changed = true;
        }
        if (changed)
        {
            index_ = 0;
            refilter();
        }
        if (event == hui::ui::Event::activated || event == hui::ui::Event::cancelled)
        {
            keyboard_open_ = false;
            // Done goes to the results; back returns to the field.
            area_ =
                event == hui::ui::Event::activated && !results_.empty() ? Area::grid : Area::field;
            feedback.play(hui::audio::Cue::modal_close);
        }
    }
    else if (focused && picker_.open)
    {
        int selected = 0;
        const int count = static_cast<int>(filter_options(picking_, &selected).size());
        const int chosen = picker_.update(input, count, feedback);
        if (chosen >= 0)
            choose(picking_, chosen);
    }
    else if (focused)
    {
        const auto move_to = [&](Area area, int column)
        {
            area_ = area;
            column_ = column;
            feedback.play(hui::audio::Cue::focus);
        };
        switch (area_)
        {
        case Area::field:
            if (input.nav == Direction::up)
            {
                intent.kind = Intent::Kind::top_bar;
                return intent;
            }
            if (input.nav == Direction::down)
                move_to(Area::filters, 2);
            else if (input.nav != Direction::none)
                refuse(feedback, input);
            if (input.is_pressed(Action::confirm))
            {
                feedback.play(hui::audio::Cue::modal_open);
                start_search();
            }
            else if (input.is_pressed(Action::west) && !filters_.query.empty())
            {
                filters_.query.clear();
                refilter();
                feedback.play(hui::audio::Cue::erase);
            }
            break;
        case Area::collection:
        case Area::filters:
        {
            const int count =
                area_ == Area::collection
                    ? 2
                    : (filters_.narrowed() || filters_.favourites || !filters_.query.empty() ? kFilterControls
                                                                                             : kFilterControls - 1);
            if (input.nav == Direction::up)
                move_to(Area::field, 0);
            else if (input.nav == Direction::down)
            {
                if (!results_.empty())
                {
                    area_ = Area::grid;
                    index_ = std::min(index_, kColumns - 1);
                    feedback.play(hui::audio::Cue::focus);
                }
                else
                    refuse(feedback, input);
            }
            else if (input.nav == Direction::left || input.nav == Direction::right)
            {
                const int next = column_ + (input.nav == Direction::right ? 1 : -1);
                if (next >= 0 && next < count)
                    move_to(area_, next);
                else if (area_ == Area::collection && next == 2)
                    move_to(Area::filters, 0);
                else if (area_ == Area::filters && next < 0)
                    move_to(Area::collection, 1);
                else
                    refuse(feedback, input);
            }
            if (input.is_pressed(Action::confirm))
            {
                if (area_ == Area::collection)
                {
                    const bool favourites = column_ == 1;
                    if (favourites && !state.paired)
                    {
                        refuse(feedback, input);
                    }
                    else
                    {
                        filters_.favourites = favourites;
                        index_ = 0;
                        refilter();
                        feedback.play(hui::audio::Cue::toggle);
                    }
                }
                else if (column_ >= kFilterControls - 1)
                {
                    filters_ = BrowseFilters{};
                    index_ = 0;
                    refilter();
                    feedback.play(hui::audio::Cue::toggle);
                }
                else
                {
                    int selected = 0;
                    filter_options(column_, &selected);
                    picking_ = column_;
                    picker_.show(selected);
                    feedback.play(hui::audio::Cue::modal_open);
                }
            }
            break;
        }
        case Area::grid:
        {
            const int count = static_cast<int>(results_.size());
            if (count == 0)
            {
                area_ = Area::filters;
                break;
            }
            int next = index_;
            if (input.nav == Direction::left)
                next = index_ % kColumns == 0 ? -1 : index_ - 1;
            else if (input.nav == Direction::right)
                next = index_ % kColumns == kColumns - 1 || index_ + 1 >= count ? -1 : index_ + 1;
            else if (input.nav == Direction::down)
                next = index_ + kColumns < count                    ? index_ + kColumns
                       : index_ / kColumns < (count - 1) / kColumns ? count - 1
                                                                    : -1;
            else if (input.nav == Direction::up)
                next = index_ - kColumns;
            if (input.nav != Direction::none)
            {
                if (input.nav == Direction::up && next < 0)
                {
                    move_to(index_ % kColumns < 2 ? Area::collection : Area::filters,
                            index_ % kColumns < 2 ? index_ % kColumns : index_ % kColumns - 2);
                }
                else if (next >= 0 && next < count)
                {
                    index_ = next;
                    feedback.play(hui::audio::Cue::focus, 1.0f, 0.0f, 0.8f);
                }
                else
                {
                    refuse(feedback, input);
                }
            }
            if (input.is_pressed(Action::confirm))
            {
                feedback.play(hui::audio::Cue::select);
                intent.kind = Intent::Kind::open_game;
                intent.id =
                    state
                        .games[static_cast<std::size_t>(results_[static_cast<std::size_t>(index_)])]
                        .id;
                return intent;
            }
            if (input.is_pressed(Action::north))
            {
                area_ = Area::field;
                feedback.play(hui::audio::Cue::modal_open);
                start_search();
            }
            break;
        }
        }
    }

    // Animation.
    keyboard_.set_active(keyboard_open_);
    keyboard_.update(dt);
    keyboard_in_.target = keyboard_open_ ? 1.0f : 0.0f;
    keyboard_in_.update(dt, 14.0f);
    picker_.animate(dt);
    const bool live = focused && !keyboard_open_ && !picker_.open;
    field_focus_.target = focused && area_ == Area::field ? 1.0f : 0.0f;
    field_focus_.update(dt, 16.0f);
    for (int i = 0; i < 2 + kFilterControls; ++i)
    {
        const bool on = live && ((area_ == Area::collection && i == column_) ||
                                 (area_ == Area::filters && i == 2 + column_));
        control_focus_[i] += ((on ? 1.0f : 0.0f) - control_focus_[i]) * std::min(1.0f, dt * 16.0f);
    }
    // The page scrolls under the top bar to keep the focused row in view.
    float target = 0.0f;
    if (area_ == Area::grid)
    {
        const float top = kGridTop + static_cast<float>(index_ / kColumns) * kRowStep;
        target = std::max(0.0f, top + kRowStep - 1010.0f);
        target = std::min(target, std::max(0.0f, top - 140.0f));
    }
    scroll_.target = target;
    scroll_.update(dt, 10.0f);
    for (int i = 0; i < static_cast<int>(tile_focus_.size()); ++i)
    {
        const bool on = live && area_ == Area::grid && i == index_;
        float &amount = tile_focus_[static_cast<std::size_t>(i)];
        amount += ((on ? 1.0f : 0.0f) - amount) * std::min(1.0f, dt * 16.0f);
    }
    return intent;
}

void BrowseScreen::draw(Frame &frame, bool focused) const
{
    (void)focused;
    hui::gfx::DrawList &list = frame.scene;
    const Snapshot &state = ctx_.store.state();
    const ui::Type &type = ctx_.type;
    const float scroll = scroll_.value;

    list.push_clip({0.0f, 104.0f, 1920.0f, 976.0f});
    list.push_transform(1.0f, 0.0f, 0.0f, 0.0f, -scroll);

    // .page-heading: the title and how many games match.
    const float title = ui::text(list, type.semibold, tr("Browse"), ui::kGutter, kTitleBaseline, 60.0f,
                                 ui::color::text);
    const std::string count =
        trn(static_cast<long>(results_.size()), "{count} game", "{count} games");
    ui::text(list, type.regular, count, ui::kGutter + title + 24.0f, kTitleBaseline, 20.0f,
             ui::color::text3);

    // .search-field: a tonal pill, brighter while typing.
    const ui::Rect field{1920.0f - ui::kGutter - kFieldWidth, kFieldY, kFieldWidth, kFieldHeight};
    ui::focus_ring(list, field, field.h * 0.5f, field_focus_.value);
    ui::surface(list, field, field.h * 0.5f,
                keyboard_open_ ? ui::color::field_open : ui::color::field);
    ui::icon_search(list, field.x + 34.0f, field.cy(), 24.0f, ui::color::text2);
    if (filters_.query.empty())
    {
        ui::text(list, type.regular, tr("Search games or title ID"), field.x + 60.0f, field.cy() + 7.0f,
                 20.0f, ui::color::text3);
    }
    else
    {
        const float w = ui::text_fit(list, type.regular, filters_.query, field.x + 60.0f,
                                     field.cy() + 7.0f, 20.0f, field.w - 90.0f, ui::color::text);
        if (keyboard_open_ && static_cast<int>(ctx_.clock * 2.0f) % 2 == 0)
            list.rounded_rect({field.x + 63.0f + w, field.cy() - 12.0f, 2.0f, 24.0f}, 1.0f,
                              ui::color::text);
    }

    // .collection-tabs: All games and Favourites, one tonal track with the chosen side lit.
    ui::surface(list, {ui::kGutter, kChipsY, 370.0f, kChipHeight}, kChipHeight * .5f,
                ui::color::field);
    list.line(512.0f, kChipsY + 8.0f, 512.0f, kChipsY + kChipHeight - 8.0f, 1.0f,
              ui::Color::rgb(0xffffff, 0.1f));
    float x = ui::kGutter;
    for (int c = 0; c < 2; ++c)
    {
        const ui::Rect chip{x, kChipsY, chip_width(type, c), kChipHeight};
        const ui::Rect lit = chip.inset(5.0f);
        const bool on = (c == 1) == filters_.favourites;
        ui::focus_ring(list, lit, lit.h * 0.5f, control_focus_[c]);
        if (on)
            list.rounded_rect(lit, lit.h * 0.5f, ui::Color::rgb(0xf4f4f4));

        ui::text(list, type.medium, tr(kChips[c]), chip.cx(), chip.cy() + 7.0f, 20.0f,
                 on ? ui::Color::rgb(0x080808) : ui::color::text, ui::Align::center);
        x += chip.w;
    }

    // .browse-selects: five dropdowns and Reset filters.
    for (int f = 0; f < kFilterControls - 1; ++f)
    {
        const ui::Rect r = filter_rect(f);
        ui::select_field(
            list, type, r,
            f == 4 && filters_.sort == Sort::release ? tr("Newest first") : filter_value(f),
            control_focus_[2 + f], picker_.open && picking_ == f, true, ui::FieldLook::pill);
    }
    const ui::Rect reset = filter_rect(kFilterControls - 1);
    if (filters_.narrowed() || filters_.favourites || !filters_.query.empty())
        ui::button(list, type, reset, tr("Reset filters"), ui::ButtonKind::secondary, control_focus_[2 + kFilterControls - 1],
                   filters_.narrowed() || filters_.favourites || !filters_.query.empty(), 18.0f);
    if (filters_.sort == Sort::size || filters_.sort == Sort::size_desc)
        ui::text(list, type.regular,
                 tr("Sorted by the smallest download option that matches your filters."), ui::kGutter,
                 kFiltersY + ui::kFieldHeight + 26.0f, 15.0f, ui::color::text3);

    // ---- results ----
    if (results_.empty() && state.have_catalog)
    {
        ui::text(list, type.light, tr("No games found"), ui::kGutter, kGridTop + 70.0f, 40.0f,
                 ui::color::text);
        ui::text(list, type.regular,
                 filters_.favourites ? tr("Add favourites from a game\xE2\x80\x99s page.")
                                     : tr("Try another name, title ID or filter."),
                 ui::kGutter, kGridTop + 114.0f, 20.0f, ui::color::text2);
    }
    for (int pass = 0; pass < 2; ++pass)
    {
        for (int i = 0; i < static_cast<int>(results_.size()); ++i)
        {
            const ui::Rect r = tile_rect(i);
            if (r.y - scroll > 1080.0f || r.y + kRowStep - scroll < 0.0f)
                continue;
            const float amount = tile_focus_[static_cast<std::size_t>(i)];
            const bool on = amount > 0.5f;
            if ((pass == 0) == on)
                continue; // the focused tile is drawn last, over its neighbours
            const Game &game =
                state.games[static_cast<std::size_t>(results_[static_cast<std::size_t>(i)])];
            draw_tile(ctx_, list, game, r, amount);
            if (state.favourite(game.id))
            {
                // .tile-favorite: a gold star in a dark disc, top right.
                const float scale = 1.0f;
                list.push_transform(scale, r.cx(), r.cy(), 0.0f, 0.0f);
                list.circle(r.x + r.w - 25.0f, r.y + 25.0f, 15.0f, ui::Color::rgb(0x141414, 0.87f));
                list.star(r.x + r.w - 25.0f, r.y + 25.5f, 8.0f, ui::Color::rgb(0xffffff));
                list.pop_transform();
            }
            draw_tile_caption(ctx_, list, game,
                              game_state(game, state.jobs, state.library_games()).label, r.x,
                              r.y + r.h, r.w, amount, false);
        }
    }
    list.pop_transform();
    list.pop_clip();

    // ---- a filter's list ----
    if (picker_.in.value > 0.01f)
    {
        int selected = 0;
        const std::vector<std::string> options = filter_options(picking_, &selected);
        ui::Rect trigger = filter_rect(picking_);
        trigger.y -= scroll;
        frame.glass = true;
        ui::select_menu(frame.overlay, type, trigger, options, selected, picker_.active,
                        picker_.in.value, frame.glass_texture);
    }

    // ---- keyboard ----
    if (keyboard_in_.value > 0.01f)
    {
        hui::gfx::DrawList &overlay = frame.overlay;
        frame.glass = true;
        const float in = keyboard_in_.value;
        overlay.push_opacity(in);
        overlay.push_transform(1.0f, 0.0f, 0.0f, 0.0f, 60.0f * (1.0f - in));
        const ui::Rect panel{0.0f, kKeyboardTop, 1920.0f, 1080.0f - kKeyboardTop};
        overlay.glass(frame.glass_texture, panel, 0.0f, ui::Color::rgb(0xffffff));
        overlay.rounded_rect(panel, 0.0f, ui::color::night1.with_alpha(0.78f));
        overlay.rounded_rect({0.0f, kKeyboardTop, 1920.0f, 1.5f}, 0.0f, ui::color::line);
        hui::ui::Canvas canvas{overlay, ctx_.fonts, frame.glass_texture, ctx_.clock};
        keyboard_.draw(canvas);
        overlay.pop_transform();
        overlay.pop_opacity();
    }
}

std::vector<hui::ui::Hint> BrowseScreen::hints() const
{
    if (keyboard_open_)
        return {{hui::ui::Button::square, tr("Delete")},
                {hui::ui::Button::triangle, tr("Space")},
                {hui::ui::Button::circle, tr("Close")}};
    if (picker_.open)
        return {{hui::ui::Button::cross, tr("Choose")}, {hui::ui::Button::circle, tr("Close")}};
    if (area_ == Area::grid)
        return {{hui::ui::Button::cross, tr("Open game")},
                {hui::ui::Button::triangle, tr("Search")},
                {hui::ui::Button::l1, tr("Tabs"), hui::ui::Button::r1}};
    if (area_ == Area::collection || area_ == Area::filters)
        return {{hui::ui::Button::cross, tr("Select")},
                {hui::ui::Button::l1, tr("Tabs"), hui::ui::Button::r1}};
    if (!filters_.query.empty())
        return {{hui::ui::Button::cross, tr("Type")},
                {hui::ui::Button::square, tr("Clear")},
                {hui::ui::Button::l1, tr("Tabs"), hui::ui::Button::r1}};
    return {{hui::ui::Button::cross, tr("Type")}, {hui::ui::Button::l1, tr("Tabs"), hui::ui::Button::r1}};
}

} // namespace orbit
