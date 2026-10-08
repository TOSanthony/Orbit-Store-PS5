// Orbit Store TV app - Library: the games on the console and its drives.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The browser storefront's Library (src/Library.tsx, src/LibraryActions.tsx,
// docs/library.md) for a controller. Orbit reads ShadowMount's local games API;
// this screen shows what it reports, never infers games from downloads, and
// sends only explicit actions: scan, mount, unmount, measure, copy, move and
// cancel, with the same confirmations as the browser.

#include "orbit/screens.hpp"
#include "orbit/i18n.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace orbit
{

namespace
{

constexpr int kColumns = 6;
constexpr float kTile = 268.333f;
constexpr float kColumnStep = (1920.0f - 2.0f * ui::kGutter - kTile) / (kColumns - 1);
constexpr float kCardStep = 346.0f;
constexpr float kKeyboardTop = 560.0f;
constexpr const char *kViews[] = {/* i18n */ "All games", /* i18n */ "Installed", /* i18n */ "On drive", /* i18n */ "Unavailable"};
constexpr int kViewCount = 4;
constexpr const char *kSections[] = {/* i18n */ "Games", /* i18n */ "Storage"};
constexpr float kPanelWidth = 1000.0f;

void library_icon(hui::gfx::DrawList &list, float cx, float cy, bool scan, ui::Color ink)
{
    if (scan)
    {
        ui::icon_search(list, cx, cy, 20.0f, ink);
        for (float x : {-13.0f, 13.0f})
            for (float y : {-13.0f, 13.0f})
            {
                list.line(cx + x, cy + y, cx + x * 0.5f, cy + y, 1.5f, ink);
                list.line(cx + x, cy + y, cx + x, cy + y * 0.5f, 1.5f, ink);
            }
        return;
    }
    for (int i = 0; i < 3; ++i)
    {
        const float y = cy - 9.0f + static_cast<float>(i) * 7.0f;
        list.bordered_rect({cx - 10.0f, y, 20.0f, 6.0f}, 3.0f, ui::Color{0, 0, 0, 0}, 1.5f, ink);
    }
}

void header_action(hui::gfx::DrawList &list, const ui::Type &type, const ui::Rect &r,
                   std::string_view text, float focus, bool scan, bool enabled)
{
    const ui::Color ink = ui::color::text.with_alpha(enabled ? 1.0f : 0.4f);
    ui::focus_ring(list, r, r.h * 0.5f, focus);
    list.bordered_rect(r, r.h * 0.5f, ui::color::night3.with_alpha(0.5f), 2.0f,
                       ui::color::text2.with_alpha(0.45f));
    const float width = type.medium.measure(text, 20.0f) + 40.0f;
    library_icon(list, r.cx() - width * 0.5f + 12.0f, r.cy(), scan, ink);
    ui::text(list, type.medium, text, r.cx() - width * 0.5f + 40.0f, r.cy() + 7.0f, 20.0f, ink);
}

bool in_view(const LibraryGame &game, int view)
{
    switch (view)
    {
    case 1:
        return game.installed;
    case 2:
        return game.on_drive;
    case 3:
        return !game.on_drive;
    default:
        return true;
    }
}

bool folded_contains(std::string_view text, std::string_view query)
{
    if (query.empty())
        return true;
    const auto lower = [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c + 32) : c; };
    for (std::size_t i = 0; i + query.size() <= text.size(); ++i)
    {
        std::size_t j = 0;
        while (j < query.size() && lower(text[i + j]) == lower(query[j]))
            ++j;
        if (j == query.size())
            return true;
    }
    return false;
}

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

// How long ago the Library was read.
std::string checked_time(double unix)
{
    return format_age(unix, unix_seconds());
}

// .library-label: a small outlined tag, tinted by meaning.
float label(hui::gfx::DrawList &list, const ui::Type &type, float x, float y, std::string_view text,
            int tint)
{
    static const ui::Color kInk[] = {ui::color::text3, ui::Color::rgb(0xd0d0d0),
                                     ui::Color::rgb(0xd0d0d0), ui::Color::rgb(0xd0d0d0),
                                     ui::Color::rgb(0xd0d0d0)};
    static const ui::Color kFill[] = {
        ui::Color{0, 0, 0, 0}, ui::Color::rgb(0xffffff, 0.13f), ui::Color::rgb(0xffffff, 0.1f),
        ui::Color::rgb(0xffffff, 0.12f), ui::Color::rgb(0xffffff, 0.09f)};
    static const ui::Color kEdge[] = {
        ui::color::line, ui::Color::rgb(0xffffff, 0.25f), ui::Color::rgb(0xffffff, 0.22f),
        ui::Color::rgb(0xffffff, 0.24f), ui::Color::rgb(0xffffff, 0.25f)};
    const float width = type.regular.measure(text, 14.0f) + 20.0f;
    list.bordered_rect({x, y, width, 26.0f}, 5.0f, kFill[tint], 1.0f, kEdge[tint]);
    ui::text(list, type.regular, text, x + 10.0f, y + 18.0f, 14.0f, kInk[tint]);
    return width;
}

float status_labels(hui::gfx::DrawList &list, const ui::Type &type, float x, float y,
                    const LibraryGame &game, float max_x)
{
    // Installed / Not installed, Mounted, On drive / Source missing.
    const float start = x;
    x += label(list, type, x, y, game.installed ? tr("Installed") : tr("Not installed"),
               game.installed ? 1 : 0) +
         7.0f;
    if (game.mounted)
        x += label(list, type, x, y, tr("Mounted"), 3) + 7.0f;
    if (x + 90.0f > max_x)
    {
        x = start;
        y += 33.0f;
    }
    label(list, type, x, y, game.on_drive ? tr("On drive") : tr("Source missing"), game.on_drive ? 2 : 4);
    return y + 26.0f;
}

const Game *catalogue_game(const Snapshot &state, std::string_view title_id)
{
    for (const Game &game : state.games)
    {
        for (const Release &release : game.releases)
        {
            if (release.title_id == title_id)
                return &game;
        }
    }
    return nullptr;
}

std::string file_name(const std::string &path)
{
    const std::size_t slash = path.rfind('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

} // namespace

LibraryScreen::LibraryScreen(Context &context) : Screen(context)
{
    keyboard_.style.theme = keyboard_theme();
    keyboard_.style.bindings = hui::ui::KeyboardBindings::standard();
    keyboard_.style.max_length = 40;
    keyboard_.style.done_label = tr("Done");
    keyboard_.set_bounds({ui::kGutter + 220.0f, kKeyboardTop + 40.0f, 1280.0f, 400.0f});
}

void LibraryScreen::focus()
{
    refilter();
    ctx_.store.want_library_storage(section_ == 1 || mode_ >= 2);
}

void LibraryScreen::blur()
{
    ctx_.store.want_library_storage(false);
}

void LibraryScreen::show_game(const std::string &title_id, const std::string &source_key)
{
    section_ = 0;
    selected_ = title_id;
    selected_key_ = source_key;
    mode_ = 0;
    panel_kind_ = Kind::action;
    panel_index_ = 0;
    error_.clear();
    action_serial_ = ctx_.store.library_action().serial;
    panel_in_.snap(0.0f);
}

void LibraryScreen::refilter()
{
    const Snapshot &state = ctx_.store.state();
    serial_ = state.serial;
    rows_.clear();
    const std::vector<LibraryGame> &games =
        state.have_library ? state.library.games : std::vector<LibraryGame>{};
    for (int i = 0; i < static_cast<int>(games.size()); ++i)
    {
        const LibraryGame &game = games[static_cast<std::size_t>(i)];
        if (!in_view(game, view_))
            continue;
        if (!location_.empty() && game.location != location_)
            continue;
        if (!format_.empty() && game.format != format_)
            continue;
        if (!folded_contains(game.title, query_) && !folded_contains(game.title_id, query_) &&
            !folded_contains(game.path, query_))
            continue;
        rows_.push_back(i);
    }
    std::stable_sort(rows_.begin(), rows_.end(),
                     [&](int a, int b)
                     {
                         return games[static_cast<std::size_t>(a)].title <
                                games[static_cast<std::size_t>(b)].title;
                     });
}

const LibraryGame *LibraryScreen::selected_game() const
{
    if (selected_.empty())
        return nullptr;
    const LibrarySnapshot &library = ctx_.store.state().library;
    if (const LibraryGame *game = library.game(selected_, selected_key_))
        return game;
    for (const LibraryGame &game : library.games)
    {
        if (game.title_id == selected_)
            return &game;
    }
    return nullptr;
}

std::vector<const LibraryDrive *> LibraryScreen::destinations() const
{
    // Writable destinations other than the source's own folder.
    std::vector<const LibraryDrive *> out;
    const LibraryGame *game = selected_game();
    if (game == nullptr)
        return out;
    const std::string parent = game->path.substr(0, game->path.rfind('/'));
    for (const LibraryDrive &drive : ctx_.store.state().library_storage.destinations)
    {
        if (drive.read_only || drive.path == game->path || drive.path == parent ||
            drive.path.rfind(game->path + "/", 0) == 0)
            continue;
        out.push_back(&drive);
    }
    return out;
}

std::vector<const LibraryGame *> LibraryScreen::drive_games() const
{
    std::vector<const LibraryGame *> out;
    const Snapshot &state = ctx_.store.state();
    const LibraryDrive *drive = nullptr;
    for (const LibraryDrive &d : state.library_storage.drives)
    {
        if (d.id == drive_id_)
            drive = &d;
    }
    if (drive == nullptr && !state.library_storage.drives.empty())
        drive = &state.library_storage.drives.front();
    if (drive == nullptr)
        return out;
    for (const LibraryGame &game : state.library.games)
    {
        if (game.on_drive && game.path.rfind(drive->path, 0) == 0)
            out.push_back(&game);
    }
    return out;
}

// Location and Format: every value the inventory reports, sorted. The picker
// shows locations translated, so a choice maps back through this list.
std::vector<std::string> LibraryScreen::filter_values() const
{
    std::vector<std::string> values;
    for (const LibraryGame &game : ctx_.store.state().library.games)
    {
        const std::string &value = picking_ == Kind::location ? game.location : game.format;
        if (!value.empty() && !contains(values, value))
            values.push_back(value);
    }
    std::sort(values.begin(), values.end());
    return values;
}

std::vector<std::string> LibraryScreen::picker_options(int *selected) const
{
    std::vector<std::string> options;
    *selected = 0;
    if (picking_ == Kind::destination)
    {
        options.push_back(tr("Choose a destination"));
        const std::vector<const LibraryDrive *> targets = destinations();
        for (std::size_t i = 0; i < targets.size(); ++i)
        {
            options.push_back(i18n::place(targets[i]->label) + " \xC2\xB7 " + targets[i]->path +
                              " \xC2\xB7 " +
                              tr("{size} free", {{"size", format_bytes(targets[i]->free)}}));
            if (targets[i]->id == destination_id_)
                *selected = static_cast<int>(i) + 1;
        }
        return options;
    }
    const std::vector<std::string> values = filter_values();
    options.push_back(picking_ == Kind::location ? tr("All locations") : tr("All formats"));
    const std::string &current = picking_ == Kind::location ? location_ : format_;
    for (std::size_t i = 0; i < values.size(); ++i)
    {
        options.push_back(picking_ == Kind::location ? i18n::place(values[i]) : values[i]);
        if (values[i] == current)
            *selected = static_cast<int>(i) + 1;
    }
    return options;
}

// ---- layout: the page's focusable controls, top to bottom ----

std::vector<LibraryScreen::Control> LibraryScreen::page_controls() const
{
    const Snapshot &state = ctx_.store.state();
    const ui::Type &type = ctx_.type;
    const LibrarySnapshot &library = state.library;
    std::vector<Control> out;
    int row = 0;
    // Header actions, then one compact row of status tabs, search and location.
    float right = 1920.0f - ui::kGutter;
    if (library.can("rescan"))
    {
        const float w =
            ui::button_width(type, tr("Scan for games"), 20.0f, ui::ButtonKind::secondary) + 40.0f;
        out.push_back({Kind::scan, 0, row, {right - w, 112.0f, w, 66.0f}});
        right -= w + 16.0f;
    }
    if (!state.have_library || !library.ready() || library.stale || section_ == 1)
    {
        const float w = ui::button_width(type, tr("Refresh library"), 18.0f, ui::ButtonKind::secondary);
        out.push_back({Kind::refresh, 0, row, {right - w, 112.0f, w, 66.0f}});
        right -= w + 16.0f;
    }
    if (state.have_library && library.ready())
        out.push_back({Kind::section, 1 - section_, row, {right - 204.0f, 112.0f, 204.0f, 66.0f}});
    std::reverse(out.begin(), out.end());
    ++row;
    if (!state.have_library || !library.ready())
        return out;
    float y = 216.0f + (library.message.empty() && state.library_error.empty() ? 0.0f : 96.0f);
    if (library.stale)
        y += 36.0f;
    if (library.has_storage_job && library.storage_job.id > 0)
        y += 70.0f;
    if (section_ == 0)
    {
        for (int v = 0; v < kViewCount; ++v)
            out.push_back({Kind::view,
                           v,
                           row,
                           {ui::kGutter + static_cast<float>(v) * 185.0f, y, 185.0f, 58.0f}});
        out.push_back({Kind::search, 0, row, {930.0f, y, 580.0f, 58.0f}});
        out.push_back({Kind::location, 0, row, {1530.0f, y, 290.0f, 58.0f}});
        ++row;
        y += 78.0f;
        if (rows_.empty() && !library.games.empty())
        {
            const float rw =
                ui::button_width(type, tr("Reset library filters"), 18.0f, ui::ButtonKind::secondary);
            out.push_back({Kind::reset, 0, row, {960.0f - rw * 0.5f, y + 140.0f, rw, 44.0f}});
            return out;
        }
        for (int i = 0; i < static_cast<int>(rows_.size()); ++i)
        {
            const int r = i / kColumns;
            out.push_back({Kind::card,
                           i,
                           row + r,
                           {ui::kGutter + static_cast<float>(i % kColumns) * kColumnStep,
                            y + static_cast<float>(r) * kCardStep, kTile, kTile}});
        }
        return out;
    }
    // Storage: the drives, then the selected drive's games.
    const std::vector<LibraryDrive> &drives = state.library_storage.drives;
    y += 90.0f;
    const float card_w = 400.0f;
    for (int d = 0; d < static_cast<int>(drives.size()); ++d)
        out.push_back({Kind::drive,
                       d,
                       row,
                       {ui::kGutter + static_cast<float>(d % 4) * (card_w + 20.0f),
                        y + static_cast<float>(d / 4) * 190.0f, card_w, 170.0f}});
    if (!drives.empty())
    {
        ++row;
        y += 190.0f * static_cast<float>((drives.size() + 3) / 4) + 40.0f;
        if (library.can("list_games"))
        {
            const float mw =
                ui::button_width(type, tr("Measure game sizes"), 18.0f, ui::ButtonKind::secondary);
            out.push_back({Kind::measure, 0, row, {1920.0f - ui::kGutter - mw, y, mw, 44.0f}});
            ++row;
        }
        y += 110.0f;
        const std::vector<const LibraryGame *> games = drive_games();
        for (int g = 0; g < static_cast<int>(games.size()); ++g)
            out.push_back({Kind::storage_game,
                           g,
                           row++,
                           {ui::kGutter, y + static_cast<float>(g) * 64.0f, 1400.0f, 58.0f}});
    }
    return out;
}

std::vector<LibraryScreen::Control> LibraryScreen::panel_controls() const
{
    // The details panel: actions, then (when confirming) the destination,
    // then Confirm and Back.
    std::vector<Control> out;
    const LibraryGame *game = selected_game();
    if (game == nullptr)
        return out;
    const LibrarySnapshot &library = ctx_.store.state().library;
    const ui::Type &type = ctx_.type;
    const float left = 960.0f - kPanelWidth * 0.5f + 56.0f;
    float x = left;
    const float y = 400.0f;
    if (mode_ == 0)
    {
        const bool transfer = game->managed && game->can_manage_source && game->on_drive;
        if (game->managed && library.can(game->mounted ? "unmount_game" : "mount_game"))
        {
            const char *text = game->mounted ? tr("Unmount game") : tr("Mount game");
            const float w = ui::button_width(type, text, 18.0f);
            out.push_back({Kind::action, 0, 0, {x, y, w, 48.0f}});
            x += w + 12.0f;
        }
        if (transfer && library.can("copy_game_source") && library.can("storage_job_status"))
        {
            const float w =
                ui::button_width(type, tr("Copy to drive"), 18.0f, ui::ButtonKind::secondary);
            out.push_back({Kind::action, 1, 0, {x, y, w, 48.0f}});
            x += w + 12.0f;
        }
        if (transfer && library.can("move_game_source") && library.can("storage_job_status"))
        {
            const float w =
                ui::button_width(type, tr("Move to drive"), 18.0f, ui::ButtonKind::secondary);
            out.push_back({Kind::action, 2, 0, {x, y, w, 48.0f}});
        }
        return out;
    }
    // In the confirmation box: the destination (copy, move), then the buttons.
    float cy = y + 150.0f;
    if (mode_ >= 2)
    {
        out.push_back({Kind::destination, 0, 0, {left, y + 168.0f, 640.0f, ui::kFieldHeight}});
        cy = y + 340.0f;
    }
    const char *confirm = mode_ == 1   ? tr("Confirm unmount")
                          : mode_ == 2 ? tr("Start copy")
                                       : tr("Confirm move");
    const float cw = ui::button_width(type, confirm, 18.0f);
    out.push_back({Kind::confirm, 0, 1, {left, cy, cw, 48.0f}});
    out.push_back({Kind::back,
                   0,
                   1,
                   {left + cw + 12.0f, cy,
                    ui::button_width(type, tr("Back"), 18.0f, ui::ButtonKind::secondary), 48.0f}});
    return out;
}

float LibraryScreen::content_height() const
{
    const std::vector<Control> controls = page_controls();
    float bottom = 0.0f;
    for (const Control &c : controls)
        bottom = std::max(bottom, c.rect.y + c.rect.h + (c.kind == Kind::card ? 150.0f : 20.0f));
    return bottom;
}

// ---- input ----

Intent LibraryScreen::update(const InputFrame &input, float dt, Feedback &feedback, bool focused)
{
    Intent intent;
    const Snapshot &state = ctx_.store.state();
    if (serial_ != state.serial)
        refilter();
    const LibrarySnapshot &library = state.library;
    if (ctx_.store.library_action().serial != action_serial_ && !ctx_.store.library_action().busy)
    {
        action_serial_ = ctx_.store.library_action().serial;
        error_ = ctx_.store.library_action().error;
        if (error_.empty())
        {
            // An accepted action closes its confirmation; progress follows in the snapshot.
            mode_ = 0;
            scan_open_ = false;
            panel_kind_ = Kind::action;
        }
    }
    const auto run = [&](const char *action, bool confirmed, const LibraryGame *game,
                         const std::string &destination, int job)
    {
        api::Client::LibraryRequest request;
        request.action = action;
        if (game != nullptr)
        {
            request.title_id = game->title_id;
            request.source_key = game->source_key;
        }
        request.destination_id = destination;
        request.job_id = job;
        request.confirmed = confirmed;
        error_.clear();
        ctx_.store.run_library_action(std::move(request));
        feedback.play(hui::audio::Cue::select);
    };

    if (focused && keyboard_open_)
    {
        const hui::ui::Event event = keyboard_.handle(input, feedback);
        bool changed = false;
        for (int i = 0; i < keyboard_.erased(); ++i)
        {
            pop_character(query_);
            changed = true;
        }
        if (!keyboard_.typed().empty())
        {
            query_ += keyboard_.typed();
            changed = true;
        }
        if (changed)
            refilter();
        if (event == hui::ui::Event::activated || event == hui::ui::Event::cancelled)
        {
            keyboard_open_ = false;
            feedback.play(hui::audio::Cue::modal_close);
        }
    }
    else if (focused && picker_.open)
    {
        int selected = 0;
        const std::vector<std::string> options = picker_options(&selected);
        const int chosen = picker_.update(input, static_cast<int>(options.size()), feedback);
        if (chosen >= 0)
        {
            if (picking_ == Kind::destination)
            {
                const std::vector<const LibraryDrive *> targets = destinations();
                destination_id_ = chosen > 0 && chosen <= static_cast<int>(targets.size())
                                      ? targets[static_cast<std::size_t>(chosen - 1)]->id
                                      : std::string();
            }
            else
            {
                const std::vector<std::string> values = filter_values();
                (picking_ == Kind::location ? location_ : format_) =
                    chosen > 0 && chosen <= static_cast<int>(values.size())
                        ? values[static_cast<std::size_t>(chosen - 1)]
                        : std::string();
                refilter();
            }
        }
    }
    else if (focused && scan_open_)
    {
        if (input.nav == Direction::left || input.nav == Direction::right)
        {
            scan_button_ = input.nav == Direction::right ? 1 : 0;
            feedback.play(hui::audio::Cue::focus);
        }
        if (input.is_pressed(Action::back) ||
            (input.is_pressed(Action::confirm) && scan_button_ == 1))
        {
            scan_open_ = false;
            feedback.play(hui::audio::Cue::modal_close);
        }
        else if (input.is_pressed(Action::confirm))
        {
            if (library.working() || ctx_.store.library_action().busy)
                refuse(feedback, input);
            else
                run("scan", true, nullptr, {}, 0);
        }
    }
    else if (focused && !selected_.empty())
    {
        const LibraryGame *game = selected_game();
        std::vector<Control> controls = panel_controls();
        if (input.is_pressed(Action::back))
        {
            if (mode_ != 0)
                mode_ = 0;
            else
                selected_.clear();
            panel_kind_ = Kind::action;
            panel_index_ = 0;
            error_.clear();
            feedback.play(hui::audio::Cue::modal_close);
        }
        else if (!controls.empty())
        {
            int at = 0;
            for (int i = 0; i < static_cast<int>(controls.size()); ++i)
            {
                if (controls[static_cast<std::size_t>(i)].kind == panel_kind_ &&
                    controls[static_cast<std::size_t>(i)].index == panel_index_)
                    at = i;
            }
            int next = at;
            if (input.nav == Direction::left || input.nav == Direction::right)
            {
                next = at + (input.nav == Direction::right ? 1 : -1);
                if (next < 0 || next >= static_cast<int>(controls.size()) ||
                    controls[static_cast<std::size_t>(next)].row !=
                        controls[static_cast<std::size_t>(at)].row)
                    next = -1;
            }
            else if (input.nav == Direction::up || input.nav == Direction::down)
            {
                const int row = controls[static_cast<std::size_t>(at)].row +
                                (input.nav == Direction::down ? 1 : -1);
                next = -1;
                for (int i = 0; i < static_cast<int>(controls.size()); ++i)
                {
                    if (controls[static_cast<std::size_t>(i)].row == row)
                    {
                        next = i;
                        break;
                    }
                }
            }
            if (input.nav != Direction::none)
            {
                if (next >= 0)
                {
                    at = next;
                    feedback.play(hui::audio::Cue::focus);
                }
                else
                {
                    refuse(feedback, input);
                }
            }
            panel_kind_ = controls[static_cast<std::size_t>(at)].kind;
            panel_index_ = controls[static_cast<std::size_t>(at)].index;
            if (input.is_pressed(Action::confirm) && game != nullptr)
            {
                const bool blocked = library.working() || ctx_.store.library_action().busy ||
                                     library.stale || !game->on_drive;
                switch (panel_kind_)
                {
                case Kind::action:
                    if (blocked || (panel_index_ > 0 && game->mounted))
                    {
                        refuse(feedback, input);
                    }
                    else if (panel_index_ == 0 && !game->mounted)
                    {
                        run("mount", false, game, {}, 0);
                    }
                    else
                    {
                        mode_ = panel_index_ == 0 ? 1 : panel_index_ == 1 ? 2 : 3;
                        destination_id_.clear();
                        panel_kind_ = mode_ >= 2 ? Kind::destination : Kind::confirm;
                        panel_index_ = 0;
                        if (mode_ >= 2)
                            ctx_.store.want_library_storage(true, true);
                        feedback.play(hui::audio::Cue::modal_open);
                    }
                    break;
                case Kind::destination:
                {
                    int selected = 0;
                    picking_ = Kind::destination;
                    picker_options(&selected);
                    picker_.show(selected);
                    feedback.play(hui::audio::Cue::modal_open);
                    break;
                }
                case Kind::confirm:
                    if (blocked || (mode_ >= 2 && destination_id_.empty()))
                        refuse(feedback, input);
                    else
                        run(mode_ == 1   ? "unmount"
                            : mode_ == 2 ? "copy"
                                         : "move",
                            true, game, mode_ >= 2 ? destination_id_ : std::string(), 0);
                    break;
                case Kind::back:
                    mode_ = 0;
                    panel_kind_ = Kind::action;
                    panel_index_ = 0;
                    feedback.play(hui::audio::Cue::back);
                    break;
                default:
                    break;
                }
            }
        }
        else if (input.is_pressed(Action::confirm))
        {
            refuse(feedback, input);
        }
    }
    else if (focused)
    {
        std::vector<Control> controls = page_controls();
        int at = -1;
        for (int i = 0; i < static_cast<int>(controls.size()); ++i)
        {
            if (controls[static_cast<std::size_t>(i)].kind == focus_kind_ &&
                controls[static_cast<std::size_t>(i)].index == focus_index_)
                at = i;
        }
        if (at < 0)
            at = 0;
        const Control current = controls[static_cast<std::size_t>(at)];
        int next = -2;
        if (input.nav == Direction::left || input.nav == Direction::right)
        {
            const int step = input.nav == Direction::right ? 1 : -1;
            next = at + step;
            if (next < 0 || next >= static_cast<int>(controls.size()) ||
                controls[static_cast<std::size_t>(next)].row != current.row)
                next = -1;
        }
        else if (input.nav == Direction::up || input.nav == Direction::down)
        {
            // The nearest control, by centre, on the neighbouring row.
            const int row = current.row + (input.nav == Direction::down ? 1 : -1);
            next = -1;
            float best = 1e9f;
            for (int i = 0; i < static_cast<int>(controls.size()); ++i)
            {
                const Control &c = controls[static_cast<std::size_t>(i)];
                if (c.row != row)
                    continue;
                const float d = std::fabs(c.rect.cx() - current.rect.cx());
                if (d < best)
                {
                    best = d;
                    next = i;
                }
            }
            if (next < 0 && input.nav == Direction::up && current.row == 0)
            {
                intent.kind = Intent::Kind::top_bar;
                return intent;
            }
        }
        if (next >= 0)
        {
            at = next;
            feedback.play(hui::audio::Cue::focus, 1.0f, 0.0f, 0.8f);
        }
        else if (next == -1)
        {
            refuse(feedback, input);
        }
        focus_kind_ = controls[static_cast<std::size_t>(at)].kind;
        focus_index_ = controls[static_cast<std::size_t>(at)].index;
        if (input.is_pressed(Action::confirm))
        {
            switch (focus_kind_)
            {
            case Kind::refresh:
                ctx_.store.refresh_library();
                feedback.play(hui::audio::Cue::select);
                break;
            case Kind::scan:
                scan_open_ = true;
                scan_button_ = 0;
                error_.clear();
                feedback.play(hui::audio::Cue::modal_open);
                break;
            case Kind::section:
                section_ = focus_index_;
                ctx_.store.want_library_storage(section_ == 1, section_ == 1);
                feedback.play(hui::audio::Cue::tab);
                break;
            case Kind::view:
                view_ = focus_index_;
                refilter();
                feedback.play(hui::audio::Cue::toggle);
                break;
            case Kind::search:
                keyboard_open_ = true;
                keyboard_.enter();
                keyboard_.set_length(static_cast<int>(query_.size()));
                feedback.play(hui::audio::Cue::modal_open);
                break;
            case Kind::location:
            case Kind::format:
            {
                int selected = 0;
                picking_ = focus_kind_;
                picker_options(&selected);
                picker_.show(selected);
                feedback.play(hui::audio::Cue::modal_open);
                break;
            }
            case Kind::reset:
                view_ = 0;
                query_.clear();
                location_.clear();
                format_.clear();
                refilter();
                feedback.play(hui::audio::Cue::toggle);
                break;
            case Kind::card:
                if (focus_index_ < static_cast<int>(rows_.size()))
                {
                    const LibraryGame &game = library.games[static_cast<std::size_t>(
                        rows_[static_cast<std::size_t>(focus_index_)])];
                    show_game(game.title_id, game.source_key);
                    feedback.play(hui::audio::Cue::open);
                }
                break;
            case Kind::drive:
                if (focus_index_ < static_cast<int>(state.library_storage.drives.size()))
                {
                    drive_id_ =
                        state.library_storage.drives[static_cast<std::size_t>(focus_index_)].id;
                    feedback.play(hui::audio::Cue::toggle);
                }
                break;
            case Kind::measure:
                if (library.working() || ctx_.store.library_action().busy)
                    refuse(feedback, input);
                else
                    run("measure", false, nullptr, {}, 0);
                break;
            case Kind::cancel_job:
                break;
            case Kind::storage_game:
            {
                const std::vector<const LibraryGame *> games = drive_games();
                if (focus_index_ < static_cast<int>(games.size()))
                {
                    show_game(games[static_cast<std::size_t>(focus_index_)]->title_id,
                              games[static_cast<std::size_t>(focus_index_)]->source_key);
                    feedback.play(hui::audio::Cue::open);
                }
                break;
            }
            default:
                break;
            }
        }
        // Cancel a storage operation from anywhere on the page (Square).
        if (input.is_pressed(Action::west) && library.has_storage_job && library.storage_job.active)
        {
            if (!library.storage_job.cancellable || library.storage_job.cancel_requested ||
                !library.can("storage_job_cancel") || ctx_.store.library_action().busy)
                refuse(feedback, input);
            else
                run("cancel", false, nullptr, {}, library.storage_job.id);
        }
    }

    // Animation and scrolling.
    keyboard_.set_active(keyboard_open_);
    keyboard_.update(dt);
    keyboard_in_.target = keyboard_open_ ? 1.0f : 0.0f;
    keyboard_in_.update(dt, 14.0f);
    picker_.animate(dt);
    panel_in_.target = selected_.empty() && !scan_open_ ? 0.0f : 1.0f;
    panel_in_.update(dt, 16.0f);
    const std::vector<Control> controls = page_controls();
    focus_amount_.resize(controls.size(), 0.0f);
    float target = scroll_.target;
    for (std::size_t i = 0; i < controls.size(); ++i)
    {
        const bool on = focused && selected_.empty() && !scan_open_ && !keyboard_open_ &&
                        !picker_.open && controls[i].kind == focus_kind_ &&
                        controls[i].index == focus_index_;
        focus_amount_[i] += ((on ? 1.0f : 0.0f) - focus_amount_[i]) * std::min(1.0f, dt * 16.0f);
        if (on)
        {
            const float bottom = controls[i].rect.y + controls[i].rect.h +
                                 (controls[i].kind == Kind::card ? 72.0f : 30.0f);
            if (bottom - target > 1000.0f)
                target = bottom - 1000.0f;
            if (controls[i].rect.y - target < 150.0f)
                target = controls[i].rect.y - 150.0f;
            if (controls[i].row <= 1)
                target = 0.0f;
        }
    }
    scroll_.target = std::max(0.0f, target);
    scroll_.update(dt, 10.0f);
    return intent;
}

// ---- drawing ----

void LibraryScreen::draw(Frame &frame, bool focused) const
{
    (void)focused;
    hui::gfx::DrawList &list = frame.scene;
    const Snapshot &state = ctx_.store.state();
    const ui::Type &type = ctx_.type;
    const LibrarySnapshot &library = state.library;
    const bool ready = state.have_library && library.ready();
    const std::vector<Control> controls = page_controls();
    const auto amount = [&](Kind kind, int index)
    {
        for (std::size_t i = 0; i < controls.size() && i < focus_amount_.size(); ++i)
        {
            if (controls[i].kind == kind && controls[i].index == index)
                return focus_amount_[i];
        }
        return 0.0f;
    };
    const auto rect = [&](Kind kind, int index)
    {
        for (const Control &c : controls)
        {
            if (c.kind == kind && c.index == index)
                return c.rect;
        }
        return ui::Rect{};
    };

    list.push_clip({0.0f, 104.0f, 1920.0f, 976.0f});
    list.push_transform(1.0f, 0.0f, 0.0f, 0.0f, -scroll_.value);

    // ---- .library-heading ----
    const float title_w =
        ui::text(list, type.semibold, tr("Library"), ui::kGutter, 176.0f, 60.0f, ui::color::text);
    if (ready)
        ui::text(list, type.regular,
                 trn(static_cast<long>(rows_.size()), "{count} game", "{count} games"),
                 ui::kGutter + title_w + 26.0f, 172.0f, 20.0f, ui::color::text2);
    const bool busy = !state.have_library || library.busy;
    if (rect(Kind::refresh, 0).w > 0.0f)
        ui::button(list, type, rect(Kind::refresh, 0), busy ? tr("Refreshing…") : tr("Refresh library"),
                   ui::ButtonKind::secondary, amount(Kind::refresh, 0), !busy, 18.0f);
    if (library.can("rescan"))
        header_action(list, type, rect(Kind::scan, 0), tr("Scan for games"), amount(Kind::scan, 0),
                      true, !library.working());
    if (library.stale)
        ui::text(list, type.regular,
                 library.updated_at > 0.0
                     ? tr("Saved snapshot \xC2\xB7 Last refreshed {time}",
                          {{"time", checked_time(library.updated_at)}})
                     : std::string(tr("Saved snapshot \xC2\xB7 Not refreshed yet")),
                 ui::kGutter, 214.0f, 16.0f, ui::color::text3);
    float y = 216.0f + (library.stale ? 36.0f : 0.0f);
    const std::string &message =
        !state.library_error.empty() ? state.library_error : library.message;
    if (!message.empty())
    {
        const ui::Rect box{ui::kGutter, y, 800.0f, ready ? 82.0f : 72.0f};
        list.rounded_rect(box, 12.0f, ui::Color::rgb(0xffffff, 0.16f));
        ui::text_fit(list, type.semibold, tr(message), box.x + 22.0f, box.y + 32.0f, 17.0f,
                     box.w - 44.0f, ui::Color::rgb(0xd0d0d0));
        if (ready)
            ui::text(list, type.regular,
                     tr("Showing the last successful result. Game and drive status may have "
                        "changed."),
                     box.x + 22.0f, box.y + 60.0f, 15.0f, ui::Color::rgb(0xd0d0d0));
        y += 96.0f;
    }

    if (!ready)
    {
        // .empty.compact
        ui::icon_layers(list, 960.0f, y + 170.0f, 52.0f, ui::color::text2);
        ui::text(list, type.light, busy ? tr("Finding your games") : tr("Connect your library"), 960.0f,
                 y + 270.0f, 32.0f, ui::color::text, ui::Align::center);
        ui::paragraph(list, type.regular,
                      busy ? tr("Reading installed titles and drive availability from ShadowMount.")
                           : tr("Library uses ShadowMount\xE2\x80\x99s local games API on your PS5. "
                             "Start a version with that API enabled, then refresh here."),
                      960.0f, y + 316.0f, 20.0f, 600.0f, 30.0f, ui::color::text2, 3,
                      ui::Align::center);
        if (!busy)
            ui::paragraph(list, type.regular,
                          tr("Games are not inferred from download history. An unavailable library "
                          "does not mean your games are missing."),
                          960.0f, y + 410.0f, 15.0f, 600.0f, 23.0f, ui::color::text3, 2,
                          ui::Align::center);
        list.pop_transform();
        list.pop_clip();
        return;
    }

    const int section_action = 1 - section_;
    header_action(list, type, rect(Kind::section, section_action), tr(kSections[section_action]),
                  amount(Kind::section, section_action), false, true);

    // A running copy, move or measurement, on either section.
    if (library.has_storage_job && library.storage_job.id > 0)
    {
        const StorageJob &job = library.storage_job;
        const ui::Rect box{ui::kGutter, y, 1720.0f, 54.0f};
        list.rounded_rect(box, 14.0f, ui::Color::rgb(0xffffff, 0.09f));
        const float progress = job.state == "completed" ? 1.0f
                               : job.total > 0.0
                                   ? static_cast<float>(std::min(1.0, job.processed / job.total))
                                   : 0.0f;
        std::string title = job.title_id;
        for (const LibraryGame &g : library.games)
        {
            if (g.title_id == job.title_id)
                title = g.title;
        }
        const std::string what = std::string(job.operation == "copy"   ? tr("Copy")
                                             : job.operation == "move" ? tr("Move")
                                                                       : job.operation.c_str()) +
                                 " \xC2\xB7 " + title;
        ui::text_fit(list, type.medium, what, box.x + 18.0f, box.y + 23.0f, 16.0f, 360.0f,
                     ui::color::text);
        std::string right = i18n::percent(progress * 100.0);
        if (job.active && job.cancellable && !job.cancel_requested)
            right += std::string("  \xC2\xB7  \xE2\x96\xA1 ") + tr("Cancel");
        else if (job.cancel_requested)
            right += std::string("  \xC2\xB7  ") + tr("Cancelling\xE2\x80\xA6");
        ui::text(list, type.regular, right, box.x + box.w - 18.0f, box.y + 23.0f, 15.0f,
                 ui::color::text2, ui::Align::right);
        ui::progress_bar(list, {box.x + 18.0f, box.y + 36.0f, box.w - 36.0f, 5.0f}, progress,
                         job.state == "failed" ? ui::color::bad : ui::color::action);
    }

    if (section_ == 0)
    {
        // .library-tabs: one tonal track with the chosen view lit.
        const ui::Rect first_tab = rect(Kind::view, 0);
        ui::surface(list, {first_tab.x, first_tab.y, 740.0f, 58.0f}, 29.0f, ui::color::field);
        list.line(890.0f, first_tab.y, 890.0f, first_tab.y + 58.0f, 1.0f,
                  ui::Color::rgb(0xffffff, 0.1f));
        for (int v = 0; v < kViewCount; ++v)
        {
            const ui::Rect r = rect(Kind::view, v).inset(5.0f);
            const bool on = v == view_;
            ui::focus_ring(list, r, r.h * 0.5f, amount(Kind::view, v));
            if (on)
                list.rounded_rect(r, r.h * 0.5f, ui::Color::rgb(0xf4f4f4));
            const ui::Color ink = on ? ui::Color::rgb(0x080808) : ui::color::text2;
            ui::text(list, type.medium, tr(kViews[v]), r.cx(), r.cy() + 7.0f, 20.0f, ink,
                     ui::Align::center);
        }
        // .library-filters
        const ui::Rect search = rect(Kind::search, 0);
        ui::focus_ring(list, search, search.h * 0.5f, amount(Kind::search, 0));
        ui::surface(list, search, search.h * 0.5f,
                    keyboard_open_ ? ui::color::field_open : ui::color::field);
        ui::icon_search(list, search.x + 34.0f, search.cy(), 24.0f, ui::color::text2);
        ui::text_fit(list, type.regular, query_.empty() ? tr("Search your library") : query_,
                     search.x + 60.0f, search.cy() + 7.0f, 20.0f, search.w - 90.0f,
                     query_.empty() ? ui::color::text3 : ui::color::text);
        const ui::Rect location = rect(Kind::location, 0);
        ui::select_field(list, type, location, location_.empty() ? std::string(tr("All locations")) : i18n::place(location_),
                         amount(Kind::location, 0), picker_.open && picking_ == Kind::location,
                         true, ui::FieldLook::pill);
        const float results_y = location.y + location.h + 20.0f;

        if (rows_.empty())
        {
            const float ey = results_y + 60.0f;
            ui::text(list, type.light,
                     library.games.empty() ? tr("No games reported yet") : tr("No matching games"), 960.0f,
                     ey + 40.0f, 32.0f, ui::color::text, ui::Align::center);
            ui::paragraph(list, type.regular,
                          library.games.empty()
                              ? tr("ShadowMount has not reported any installed or on-drive games. "
                                "Refresh after its inventory updates.")
                              : tr("Try a different title, status or location."),
                          960.0f, ey + 86.0f, 20.0f, 640.0f, 30.0f, ui::color::text2, 2,
                          ui::Align::center);
            if (!library.games.empty())
                ui::button(list, type, rect(Kind::reset, 0), tr("Reset library filters"),
                           ui::ButtonKind::secondary, amount(Kind::reset, 0), true, 18.0f);
        }
        // .library-grid: cards drawn back to front so the focused one sits on top.
        for (int pass = 0; pass < 2; ++pass)
        {
            for (int i = 0; i < static_cast<int>(rows_.size()); ++i)
            {
                const ui::Rect r = rect(Kind::card, i);
                if (r.y - scroll_.value > 1080.0f || r.y + kCardStep - scroll_.value < 0.0f)
                    continue;
                const float focus = amount(Kind::card, i);
                if ((pass == 0) == (focus > 0.5f))
                    continue;
                const LibraryGame &game =
                    library.games[static_cast<std::size_t>(rows_[static_cast<std::size_t>(i)])];
                const float scale = 1.0f;
                list.push_transform(scale, r.cx(), r.cy(), 0.0f, 0.0f);
                ui::focus_ring(list, r, ui::kArtRadius, focus);
                if (const Game *art = catalogue_game(state, game.title_id))
                {
                    draw_cover(ctx_, list, *art, r, ui::kArtRadius);
                }
                else
                {
                    // .library-cover with the Orbit mark when no artwork matches.
                    list.gradient_rect(r, ui::kArtRadius, ui::Color::rgb(0x242424),
                                       ui::Color::rgb(0x151515));
                    if (ctx_.mark != 0)
                        list.image(ctx_.mark, r.inset(48.0f), hui::gfx::kFullUv,
                                   ui::Color::rgb(0xffffff, 0.6f));
                }
                list.pop_transform();
                if (!game.on_drive)
                    list.rounded_rect(r, ui::kArtRadius, ui::color::night0.with_alpha(0.3f));
                ui::text_fit(list, type.medium, game.title, r.x, r.y + r.h + 30.0f, 20.0f, r.w,
                             ui::color::text);
                std::string status = !game.on_drive   ? tr("Unavailable")
                                     : game.mounted   ? tr("Mounted")
                                     : game.installed ? tr("Installed")
                                                      : tr("On drive");
                status += " · " + (game.on_drive ? i18n::place(game.location)
                                                 : std::string(tr("Drive disconnected")));
                ui::text_fit(list, type.regular, status, r.x, r.y + r.h + 58.0f, 18.0f, r.w,
                             ui::color::text2);
            }
        }
        if (!rows_.empty())
        {
            const ui::Rect last = rect(Kind::card, static_cast<int>(rows_.size()) - 1);
            const float footer_y = std::max(1028.0f, last.y + kCardStep + 30.0f);
            float x = ui::kGutter;
            for (const Drive &drive : state.drives)
            {
                const std::string text =
                    i18n::place(drive.label) + ": " +
                    tr("{size} free", {{"size", format_bytes(drive.free_bytes)}});
                const float width = type.regular.measure(text, 17.0f);
                if (x + width > 1100.0f)
                    break;
                ui::text(list, type.regular, text, x, footer_y, 17.0f, ui::color::text2);
                x += width + 36.0f;
            }
        }
    }
    else
    {
        // ---- Storage: .storage-overview ----
        const float top =
            y + (library.has_storage_job && library.storage_job.id > 0 ? 70.0f : 0.0f);
        ui::text(list, type.light, tr("Your drives"), ui::kGutter, top + 40.0f, 33.0f, ui::color::text);
        ui::text(list, type.regular,
                 tr("Capacity as ShadowMount reports it. Select a drive to see its games."),
                 ui::kGutter, top + 72.0f, 17.0f, ui::color::text2);
        const std::vector<LibraryDrive> &drives = state.library_storage.drives;
        if (drives.empty())
        {
            ui::text(list, type.regular,
                     state.library_storage.error.empty()
                         ? (state.have_library_storage ? tr("No drives reported.")
                                                       : tr("Checking your drives\xE2\x80\xA6"))
                         : tr(state.library_storage.error),
                     ui::kGutter, top + 140.0f, 20.0f, ui::color::text2);
        }
        for (int d = 0; d < static_cast<int>(drives.size()); ++d)
        {
            const LibraryDrive &drive = drives[static_cast<std::size_t>(d)];
            const ui::Rect r = rect(Kind::drive, d);
            const bool on = drive.id == drive_id_ || (drive_id_.empty() && d == 0);
            ui::focus_ring(list, r, 18.0f, amount(Kind::drive, d));
            list.bordered_rect(
                r, 18.0f, on ? ui::Color::rgb(0xffffff, 0.12f) : ui::Color::rgb(0xffffff, 0.04f),
                1.0f, on ? ui::Color::rgb(0xffffff, 0.55f) : ui::Color{0, 0, 0, 0});
            ui::text_fit(list, type.regular, i18n::place(drive.label), r.x + 26.0f, r.y + 42.0f, 20.0f,
                         r.w - 52.0f, ui::color::text);
            ui::text_fit(list, type.regular, drive.path, r.x + 26.0f, r.y + 70.0f, 17.0f,
                         r.w - 52.0f, ui::color::text2);
            const float used =
                drive.total > 0.0 ? static_cast<float>(drive.used / drive.total) : 0.0f;
            ui::progress_bar(list, {r.x + 26.0f, r.y + 88.0f, r.w - 52.0f, 5.0f}, used,
                             ui::color::action);
            ui::text(list, type.medium, tr("{size} free", {{"size", format_bytes(drive.free)}}),
                     r.x + 26.0f,
                     r.y + 124.0f, 18.0f, ui::color::text);
            ui::text(list, type.regular,
                     tr("{used} used of {total}",
                        {{"used", format_bytes(drive.used)}, {"total", format_bytes(drive.total)}}),
                     r.x + 26.0f, r.y + 150.0f, 17.0f, ui::color::text2);
        }
        if (!drives.empty())
        {
            const std::vector<const LibraryGame *> games = drive_games();
            const LibraryDrive *drive = &drives.front();
            for (const LibraryDrive &d : drives)
            {
                if (d.id == drive_id_)
                    drive = &d;
            }
            int measured = 0;
            for (const LibraryGame *g : games)
                measured += g->size >= 0.0 ? 1 : 0;
            const float heading =
                rect(Kind::drive, static_cast<int>(drives.size()) - 1).y + 170.0f + 60.0f;
            ui::text(list, type.medium, tr("Games on {drive}", {{"drive", i18n::place(drive->label)}}),
                     ui::kGutter, heading, 22.0f,
                     ui::color::text);
            const std::string sub =
                trn(static_cast<long>(games.size()), "{count} game", "{count} games") +
                " \xC2\xB7 " + trn(measured, "{count} size measured", "{count} sizes measured");
            ui::text(list, type.regular, sub, ui::kGutter, heading + 30.0f, 17.0f,
                     ui::color::text2);
            if (library.can("list_games"))
                ui::button(
                    list, type, rect(Kind::measure, 0),
                    library.has_action && library.action.action == "measure" && library.working()
                        ? tr("Measuring\xE2\x80\xA6")
                        : tr("Measure game sizes"),
                    ui::ButtonKind::secondary, amount(Kind::measure, 0), !library.working(), 18.0f);
            for (int g = 0; g < static_cast<int>(games.size()); ++g)
            {
                const LibraryGame &game = *games[static_cast<std::size_t>(g)];
                const ui::Rect r = rect(Kind::storage_game, g);
                const float focus = amount(Kind::storage_game, g);
                ui::focus_ring(list, r, 12.0f, focus);
                list.rounded_rect(r, 12.0f, ui::Color::rgb(0xffffff, 0.04f + 0.04f * focus));
                ui::text_fit(list, type.medium, game.title, r.x + 20.0f, r.cy() + 7.0f, 19.0f,
                             900.0f, ui::color::text);
                ui::text(list, type.regular,
                         game.size >= 0.0                    ? format_bytes(game.size)
                         : game.size_status == "unavailable" ? tr("Unavailable")
                                                             : tr("Not measured"),
                         r.x + r.w - 20.0f, r.cy() + 7.0f, 17.0f, ui::color::text2,
                         ui::Align::right);
            }
            if (!games.empty())
                ui::paragraph(list, type.regular,
                              tr("Sizes are measured on request. Shared source files can appear under "
                              "more than one title, so these sizes are not a drive total."),
                              ui::kGutter,
                              rect(Kind::storage_game, static_cast<int>(games.size()) - 1).y +
                                  100.0f,
                              15.0f, 1000.0f, 23.0f, ui::color::text3, 2);
        }
    }
    list.pop_transform();
    list.pop_clip();

    // ---- a dropdown's list ----
    if (picker_.in.value > 0.01f)
    {
        int selected = 0;
        const std::vector<std::string> options = picker_options(&selected);
        ui::Rect trigger;
        if (picking_ == Kind::destination)
        {
            for (const Control &c : panel_controls())
            {
                if (c.kind == Kind::destination)
                    trigger = c.rect;
            }
        }
        else
        {
            trigger = rect(picking_, 0);
            trigger.y -= scroll_.value;
        }
        frame.glass = true;
        ui::select_menu(frame.overlay, type, trigger, options, selected, picker_.active,
                        picker_.in.value, frame.glass_texture);
    }

    // ---- the details panel, or the scan dialog ----
    if (panel_in_.value > 0.01f)
    {
        hui::gfx::DrawList &overlay = frame.overlay;
        const float in = panel_in_.value;
        overlay.push_opacity(in);
        overlay.rounded_rect({0.0f, 0.0f, 1920.0f, 1080.0f}, 0.0f,
                             ui::color::night0.with_alpha(0.55f));
        const LibraryGame *game = selected_game();
        const ui::Rect panel{960.0f - kPanelWidth * 0.5f, 150.0f - 16.0f * (1.0f - in), kPanelWidth,
                             scan_open_ ? 420.0f : 820.0f};
        ui::modal(overlay, panel);
        const float left = panel.x + 56.0f;
        if (scan_open_)
        {
            ui::text(overlay, type.light, tr("Scan your drives?"), left, panel.y + 100.0f, 36.0f,
                     ui::color::text);
            ui::paragraph(overlay, type.regular,
                          tr("ShadowMount will search its configured locations for games. It may "
                          "register games on the console or mount detected sources. If a game is "
                          "running, the scan may wait until it is safe."),
                          left, panel.y + 150.0f, 19.0f, panel.w - 112.0f, 30.0f, ui::color::text2,
                          4);
            const float sw = ui::button_width(type, tr("Start scan"), 18.0f);
            ui::button(overlay, type, {left, panel.y + 290.0f, sw, 48.0f}, tr("Start scan"),
                       ui::ButtonKind::primary, scan_button_ == 0 ? 1.0f : 0.0f, !library.working(),
                       18.0f);
            ui::button(overlay, type,
                       {left + sw + 12.0f, panel.y + 290.0f,
                        ui::button_width(type, tr("Back"), 18.0f, ui::ButtonKind::secondary), 48.0f},
                       tr("Back"), ui::ButtonKind::secondary, scan_button_ == 1 ? 1.0f : 0.0f, true,
                       18.0f);
            if (!error_.empty())
                ui::text_fit(overlay, type.regular, tr(error_), left, panel.y + 380.0f, 17.0f,
                             panel.w - 112.0f, ui::color::bad);
        }
        else if (game == nullptr)
        {
            ui::text(overlay, type.light, tr("This game is no longer reported"), left, panel.y + 100.0f,
                     36.0f, ui::color::text);
            ui::text(overlay, type.regular, tr("Refresh the library, then try again."), left,
                     panel.y + 146.0f, 19.0f, ui::color::text2);
        }
        else
        {
            const std::string platform = game->platform == "unknown"
                                             ? std::string(tr("Platform unknown"))
                                             : (game->platform == "ps4" ? "PS4" : "PS5");
            ui::text(overlay, type.semibold, platform + " \xC2\xB7 " + game->title_id, left,
                     panel.y + 64.0f, 14.4f, ui::color::text3);
            ui::text_fit(overlay, type.light, game->title, left, panel.y + 112.0f, 36.0f,
                         panel.w - 112.0f, ui::color::text);
            status_labels(overlay, type, left, panel.y + 138.0f, *game, panel.x + panel.w - 56.0f);
            const std::vector<Control> buttons = panel_controls();
            const auto panel_focus = [&](Kind kind, int index)
            { return kind == panel_kind_ && index == panel_index_ && !picker_.open ? 1.0f : 0.0f; };
            float note_y = panel.y + 200.0f;
            if (library.has_action && library.action.title_id == game->title_id &&
                !library.action.message.empty())
            {
                ui::text_fit(overlay, type.regular, tr(library.action.message), left, note_y + 20.0f,
                             17.0f, panel.w - 112.0f,
                             library.action.state == "error" ? ui::color::bad
                                                             : ui::Color::rgb(0xd0d0d0));
                note_y += 34.0f;
            }
            if (library.stale)
                ui::text(overlay, type.regular,
                         tr("Saved snapshot. Refresh the library before relying on these statuses."),
                         left, note_y + 20.0f, 17.0f, ui::Color::rgb(0xd0d0d0));
            if (mode_ == 0)
            {
                static const char *const kActions[] = {nullptr, /* i18n */ "Copy to drive", /* i18n */ "Move to drive"};
                for (const Control &c : buttons)
                {
                    const char *text = c.index == 0
                                           ? (game->mounted ? tr("Unmount game") : tr("Mount game"))
                                           : tr(kActions[c.index]);
                    ui::button(overlay, type, {c.rect.x, c.rect.y, c.rect.w, c.rect.h}, text,
                               c.index == 0 ? ui::ButtonKind::primary : ui::ButtonKind::secondary,
                               panel_focus(Kind::action, c.index),
                               !library.working() && game->on_drive &&
                                   (c.index == 0 || !game->mounted),
                               18.0f);
                }
                float fy = 400.0f + 80.0f;
                if (game->managed && game->can_manage_source && game->on_drive && game->mounted)
                {
                    ui::text(overlay, type.regular,
                             tr("Unmount this game before copying or moving its source."), left, fy,
                             15.0f, ui::color::text3);
                    fy += 26.0f;
                }
                if (!game->managed)
                {
                    ui::text(overlay, type.regular,
                             tr("This item supports viewing details only. ShadowMount does not manage "
                             "its source."),
                             left, fy, 15.0f, ui::color::text3);
                    fy += 26.0f;
                }
                // .library-facts
                struct Fact
                {
                    const char *label;
                    std::string value;
                };
                std::vector<Fact> facts = {
                    {tr("Installation"), game->installed ? tr("Registered on the console")
                                                     : tr("Not installed on the console")},
                    {tr("Mount status"), game->mounted ? tr("Mounted") : tr("Not mounted")},
                    {tr("Source"), game->on_drive ? tr("Available on drive")
                                              : tr("Unavailable; the drive or source may be missing")},
                    {tr("Format"), game->format},
                    {tr("Location"), i18n::place(game->location)},
                    {tr("Source path"), game->path.empty() ? std::string(tr("Not reported")) : game->path},
                    {tr("Source size"), game->size >= 0.0 ? format_bytes(game->size)
                                    : game->size_status == "unavailable"
                                        ? tr("Unavailable")
                                        : tr("Use Measure game sizes in Storage")},
                    {tr("Size reported by console"), game->installed_size < 0.0
                                                     ? tr("Not reported")
                                                     : format_bytes(game->installed_size)},
                };
                // The label column fits the longest label, which runs long in German.
                float label_w = 220.0f;
                for (const Fact &fact : facts)
                    label_w = std::max(label_w, type.regular.measure(fact.label, 17.0f) + 28.0f);
                label_w = std::min(label_w, 360.0f);
                for (std::size_t i = 0; i < facts.size(); ++i)
                {
                    const float ry = fy + 10.0f + static_cast<float>(i) * 36.0f;
                    ui::text_fit(overlay, type.regular, facts[i].label, left, ry + 20.0f, 17.0f,
                                 label_w - 16.0f, ui::color::text3);
                    ui::text_fit(overlay, type.regular, facts[i].value, left + label_w, ry + 20.0f,
                                 18.0f, panel.w - 112.0f - label_w, ui::color::text);
                    overlay.rounded_rect({left, ry + 34.0f, panel.w - 112.0f, 1.0f}, 0.0f,
                                         ui::color::line);
                }
            }
            else
            {
                // .library-action-confirm
                const ui::Rect box{left - 20.0f, 380.0f, panel.w - 72.0f,
                                   mode_ == 1 ? 260.0f : 450.0f};
                overlay.rounded_rect(box, 16.0f, ui::Color::rgb(0x000000, 0.18f));
                const std::string heading =
                    mode_ == 1   ? std::string(tr("Unmount this game?"))
                    : mode_ == 2 ? tr("Copy {title}", {{"title", game->title}})
                                 : tr("Move {title}", {{"title", game->title}});
                ui::text_fit(overlay, type.medium, heading, left, box.y + 44.0f, 22.0f,
                             box.w - 40.0f, ui::color::text);
                ui::paragraph(
                    overlay, type.regular,
                    mode_ == 1   ? tr("Close the game on your PS5 before unmounting its source.")
                    : mode_ == 3 ? tr("Move the source to another location. The original is removed "
                                   "only after the move succeeds.")
                                 : tr("Create a second copy on the selected drive. The original stays "
                                   "in place."),
                    left, box.y + 80.0f, 18.0f, box.w - 40.0f, 28.0f, ui::color::text2, 2);
                if (mode_ >= 2)
                {
                    ui::text_fit(overlay, type.regular, tr("From {path}", {{"path", game->path}}), left,
                                 box.y + 140.0f,
                                 16.0f, box.w - 40.0f, ui::color::text3);
                    for (const Control &c : buttons)
                    {
                        if (c.kind != Kind::destination)
                            continue;
                        ui::field_label(overlay, type, c.rect, tr("Destination drive"));
                        std::string value = tr("Choose a destination");
                        for (const LibraryDrive *d : destinations())
                        {
                            if (d->id == destination_id_)
                                value = i18n::place(d->label) + " \xC2\xB7 " +
                                        tr("{size} free", {{"size", format_bytes(d->free)}});
                        }
                        const bool fresh = state.have_library_storage &&
                                           state.library_storage.updated_at > 0.0 &&
                                           !state.library_storage.stale;
                        ui::select_field(overlay, type, c.rect, value,
                                         panel_focus(Kind::destination, 0), picker_.open, fresh);
                        float ny = c.rect.y + c.rect.h + 30.0f;
                        for (const LibraryDrive *d : destinations())
                        {
                            if (d->id == destination_id_)
                            {
                                ui::text_fit(overlay, type.regular,
                                             tr("To {path}",
                                                {{"path", d->path + "/" + file_name(game->path)}}),
                                             left,
                                             ny, 16.0f, box.w - 40.0f, ui::color::text3);
                                ny += 24.0f;
                            }
                        }
                        if (!fresh)
                            ui::text_fit(overlay, type.regular,
                                         !state.library_storage.error.empty()
                                             ? tr(state.library_storage.error)
                                             : tr("Checking available destinations\xE2\x80\xA6"),
                                         left, ny, 16.0f, box.w - 40.0f, ui::Color::rgb(0xd0d0d0));
                        else if (destinations().empty())
                            ui::text_fit(
                                overlay, type.regular,
                                tr("Connect another writable drive, or configure another scan "
                                "location in ShadowMount."),
                                left, ny, 16.0f, box.w - 40.0f, ui::Color::rgb(0xd0d0d0));
                    }
                    ui::paragraph(overlay, type.regular,
                                  tr("Orbit checks the source and free space before starting. Pause "
                                  "active and queued downloads first. Existing destination files "
                                  "are not overwritten."),
                                  left, box.y + 316.0f, 15.0f, box.w - 40.0f, 23.0f,
                                  ui::color::text3, 2);
                }
                for (const Control &c : buttons)
                {
                    if (c.kind == Kind::confirm)
                    {
                        const char *text = ctx_.store.library_action().busy ? tr("Starting\xE2\x80\xA6")
                                           : mode_ == 1                     ? tr("Confirm unmount")
                                           : mode_ == 2                     ? tr("Start copy")
                                                                            : tr("Confirm move");
                        ui::button(overlay, type, c.rect, text, ui::ButtonKind::primary,
                                   panel_focus(Kind::confirm, 0),
                                   !library.working() && (mode_ == 1 || !destination_id_.empty()),
                                   18.0f);
                    }
                    else if (c.kind == Kind::back)
                    {
                        ui::button(overlay, type, c.rect, tr("Back"), ui::ButtonKind::secondary,
                                   panel_focus(Kind::back, 0), true, 18.0f);
                    }
                }
            }
            if (!error_.empty())
                ui::text_fit(overlay, type.regular, tr(error_), left, panel.y + panel.h - 70.0f, 17.0f,
                             panel.w - 112.0f, ui::color::bad);
            ui::paragraph(overlay, type.regular,
                          tr("Installed means registered on the console. Mounted means its source is "
                          "currently mounted. On drive means the source is available. These "
                          "statuses do not confirm that a game will launch."),
                          left, panel.y + panel.h - 40.0f, 14.0f, panel.w - 112.0f, 20.0f,
                          ui::color::text3, 2);
        }
        overlay.pop_opacity();
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
        hui::ui::Canvas canvas{overlay, ctx_.fonts, frame.glass_texture, ctx_.clock};
        keyboard_.draw(canvas);
        overlay.pop_transform();
        overlay.pop_opacity();
    }
}

std::vector<hui::ui::Hint> LibraryScreen::hints() const
{
    const LibrarySnapshot &library = ctx_.store.state().library;
    if (keyboard_open_)
        return {{hui::ui::Button::square, tr("Delete")},
                {hui::ui::Button::triangle, tr("Space")},
                {hui::ui::Button::circle, tr("Close")}};
    if (picker_.open)
        return {{hui::ui::Button::cross, tr("Choose")}, {hui::ui::Button::circle, tr("Close")}};
    if (!selected_.empty() || scan_open_)
        return {{hui::ui::Button::cross, tr("Select")},
                {hui::ui::Button::circle, mode_ != 0 ? tr("Back") : tr("Close")}};
    std::vector<hui::ui::Hint> items = {{hui::ui::Button::cross, tr("Select")}};
    if (library.has_storage_job && library.storage_job.active && library.storage_job.cancellable &&
        !library.storage_job.cancel_requested)
        items.push_back({hui::ui::Button::square, tr("Cancel operation")});
    items.push_back({hui::ui::Button::l1, tr("Tabs"), hui::ui::Button::r1});
    return items;
}

} // namespace orbit
