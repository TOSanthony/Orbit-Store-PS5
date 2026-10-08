// Orbit Store TV app - Downloads: the console's queue, with progress and actions.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Views, labels and actions follow the browser storefront (src/Downloads.tsx):
// Active / Finished / Failed / Cancelled; Pause, Resume, Retry download;
// Cancel asks whether to keep or delete the partial file.

#include "orbit/screens.hpp"
#include "orbit/i18n.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace orbit
{

namespace
{

// The browser's Downloads page at 1920 x 1080: .page-heading, the
// .download-tabs chips, then .queue rows (70 rem at most, 0.4 rem apart).
constexpr float kTitleBaseline = 192.0f;
constexpr float kChipsY = 236.0f;
constexpr float kChipHeight = 42.0f;
constexpr float kListTop = 302.0f;
constexpr float kRowHeight = 210.0f;
constexpr float kRowGap = 8.0f;
constexpr float kRowWidth = 1400.0f;
constexpr float kRowPad = 22.0f;
constexpr float kCover = 130.0f;
constexpr const char *kDialogButtons[] = {/* i18n */ "Keep partial file", /* i18n */ "Delete partial file", /* i18n */ "Go back"};
constexpr const char *kDeleteButtons[] = {/* i18n */ "Remove from history", /* i18n */ "Delete download", /* i18n */ "Go back"};
constexpr const char *kForgetButtons[] = {/* i18n */ "Go back", /* i18n */ "Remove from history"};

const Game *game_for(const Snapshot &state, const Job &job)
{
    if (const Game *game = state.game(job.game_id))
        return game;
    for (const Game &game : state.games)
    {
        for (const Release &release : game.releases)
        {
            if (release.id == job.release_id)
                return &game;
        }
    }
    return nullptr;
}

const Release *release_for(const Game *game, const Job &job)
{
    if (game == nullptr)
        return nullptr;
    for (const Release &release : game->releases)
    {
        if (release.id == job.release_id)
            return &release;
    }
    return nullptr;
}

} // namespace

DownloadsScreen::DownloadsScreen(Context &context) : Screen(context)
{
}

int DownloadsScreen::count(JobView view) const
{
    int n = 0;
    for (const Job &job : ctx_.store.state().jobs)
    {
        if (job_view(job) == view)
            ++n;
    }
    return n;
}

std::vector<const Job *> DownloadsScreen::visible() const
{
    std::vector<const Job *> jobs;
    for (const Job &job : ctx_.store.state().jobs)
    {
        if (job_view(job) == view_)
            jobs.push_back(&job);
    }
    return jobs;
}

std::vector<DownloadsScreen::RowAction> DownloadsScreen::actions_for(const Job &job) const
{
    if (job.status == "complete")
        return {{tr("Remove from history"), "remove"}};
    if (job.status == "cancelled")
    {
        return {{job.received > 0.0 ? tr("Resume") : tr("Start again"), "resume"},
                {tr("Delete download"), "dialog"}};
    }
    const char *primary = job_active(job)         ? tr("Pause")
                          : job.status == "error" ? tr("Retry download")
                                                  : tr("Resume");
    const char *action = job_active(job) ? "pause" : job.status == "error" ? "retry" : "resume";
    return {{primary, action}, {tr("Cancel"), "dialog"}};
}

void DownloadsScreen::focus()
{
    on_views_ = visible().empty();
}

void DownloadsScreen::show_job(const std::string &job_id)
{
    pending_job_ = job_id;
    for (const Job &job : ctx_.store.state().jobs)
    {
        if (job.id != job_id)
            continue;
        view_ = job_view(job);
        const std::vector<const Job *> jobs = visible();
        for (int i = 0; i < static_cast<int>(jobs.size()); ++i)
        {
            if (jobs[static_cast<std::size_t>(i)]->id == job_id)
            {
                row_ = i;
                button_ = 0;
                on_views_ = false;
                pending_job_.clear();
            }
        }
    }
}

Intent DownloadsScreen::update(const InputFrame &input, float dt, Feedback &feedback, bool focused)
{
    Intent intent;
    // A job created a moment ago appears with the next queue refresh.
    if (!pending_job_.empty())
        show_job(pending_job_);

    std::vector<const Job *> jobs = visible();
    row_ = std::clamp(row_, 0, std::max(0, static_cast<int>(jobs.size()) - 1));
    if (jobs.empty())
        on_views_ = true;
    const bool busy = ctx_.store.action().busy;
    if (dialog_pending_ && !busy)
    {
        dialog_pending_ = false;
        dialog_error_ = ctx_.store.action().error;
        if (dialog_error_.empty()) dialog_open_ = false;
    }

    if (focused && dialog_open_)
    {
        if (input.is_pressed(Action::back) && !busy)
        {
            if (dialog_forget_)
            {
                dialog_forget_ = false;
                dialog_button_ = 2;
            }
            else dialog_open_ = false;
            feedback.play(hui::audio::Cue::modal_close);
        }
        else if (input.nav == Direction::left || input.nav == Direction::right)
        {
            const int next = dialog_button_ + (input.nav == Direction::right ? 1 : -1);
            if (next >= 0 && next < (dialog_forget_ ? 2 : 3))
            {
                dialog_button_ = next;
                feedback.play(hui::audio::Cue::focus);
            }
            else
            {
                refuse(feedback, input);
            }
        }
        else if (input.is_pressed(Action::confirm) && !busy)
        {
            if (dialog_button_ < 2)
            {
                if (dialog_cancelled_ && dialog_button_ == 0)
                {
                    dialog_forget_ = !dialog_forget_;
                    dialog_button_ = 0;
                    dialog_error_.clear();
                }
                else
                {
                    ctx_.store.job_action(dialog_job_, dialog_cancelled_
                        ? (dialog_forget_ ? "forget" : "delete") : "cancel", !dialog_cancelled_ && dialog_button_ == 1);
                    dialog_pending_ = true;
                    dialog_error_.clear();
                }
                feedback.play(dialog_button_ == 1 ? hui::audio::Cue::erase
                                                  : hui::audio::Cue::select);
            }
            else
            {
                dialog_open_ = false;
                feedback.play(hui::audio::Cue::modal_close);
            }
        }
    }
    else if (focused)
    {
        if (on_views_)
        {
            if (input.nav == Direction::up)
            {
                intent.kind = Intent::Kind::top_bar;
                return intent;
            }
            if (input.nav == Direction::left || input.nav == Direction::right)
            {
                const int next = static_cast<int>(view_) + (input.nav == Direction::right ? 1 : -1);
                if (next >= 0 && next < static_cast<int>(JobView::count))
                {
                    view_ = static_cast<JobView>(next);
                    row_ = 0;
                    button_ = 0;
                    feedback.play(hui::audio::Cue::tab);
                }
                else
                {
                    refuse(feedback, input);
                }
            }
            else if (input.nav == Direction::down)
            {
                if (!visible().empty())
                {
                    on_views_ = false;
                    row_ = 0;
                    button_ = 0;
                    feedback.play(hui::audio::Cue::focus);
                }
                else
                {
                    refuse(feedback, input);
                }
            }
            if (input.is_pressed(Action::confirm) && visible().empty())
            {
                // The empty state's button.
                feedback.play(hui::audio::Cue::select);
                intent.kind = Intent::Kind::show_tab;
                intent.tab = tab::browse;
                return intent;
            }
        }
        else if (!jobs.empty())
        {
            const Job &job = *jobs[static_cast<std::size_t>(row_)];
            const std::vector<RowAction> actions = actions_for(job);
            button_ = std::clamp(button_, 0, static_cast<int>(actions.size()) - 1);
            if (input.nav == Direction::up)
            {
                if (row_ == 0)
                    on_views_ = true;
                else
                    --row_;
                button_ = 0;
                feedback.play(hui::audio::Cue::focus);
            }
            else if (input.nav == Direction::down)
            {
                if (row_ + 1 < static_cast<int>(jobs.size()))
                {
                    ++row_;
                    button_ = 0;
                    feedback.play(hui::audio::Cue::focus);
                }
                else
                {
                    refuse(feedback, input);
                }
            }
            else if (input.nav == Direction::left || input.nav == Direction::right)
            {
                const int next = button_ + (input.nav == Direction::right ? 1 : -1);
                if (next >= 0 && next < static_cast<int>(actions.size()))
                {
                    button_ = next;
                    feedback.play(hui::audio::Cue::focus);
                }
                else
                {
                    refuse(feedback, input);
                }
            }
            if (input.is_pressed(Action::confirm))
            {
                const RowAction &action = actions[static_cast<std::size_t>(button_)];
                if (busy)
                {
                    refuse(feedback, input);
                }
                else if (std::string_view(action.action) == "dialog")
                {
                    dialog_open_ = true;
                    dialog_cancelled_ = job.status == "cancelled";
                    dialog_forget_ = false;
                    dialog_pending_ = false;
                    dialog_error_.clear();
                    dialog_button_ = dialog_cancelled_ ? 2 : 0;
                    dialog_job_ = job.id;
                    dialog_in_.snap(0.0f);
                    feedback.play(hui::audio::Cue::modal_open);
                }
                else
                {
                    ctx_.store.job_action(job.id, action.action, false);
                    feedback.play(hui::audio::Cue::select);
                }
            }
        }
    }

    // Live progress while this screen is visible.
    ctx_.store.set_fast(true);
    scroll_.target =
        on_views_ ? 0.0f : std::max(0.0f, static_cast<float>(row_ - 2) * (kRowHeight + kRowGap));
    scroll_.update(dt, 11.0f);
    dialog_in_.target = dialog_open_ ? 1.0f : 0.0f;
    dialog_in_.update(dt, 16.0f);
    return intent;
}

void DownloadsScreen::draw(Frame &frame, bool focused) const
{
    hui::gfx::DrawList &list = frame.scene;
    const Snapshot &state = ctx_.store.state();
    const ui::Type &type = ctx_.type;

    const float title = ui::text(list, type.light, tr("Downloads"), ui::kGutter, kTitleBaseline, 56.0f,
                                 ui::color::text);
    int active = 0;
    int attention = 0;
    for (const Job &job : state.jobs)
    {
        if (job_view(job) == JobView::active)
            ++active;
        if (job.status == "error")
            ++attention;
    }
    const std::string summary =
        trn(active, "{count} active", "{count} active") + " \xC2\xB7 " +
        trn(attention, "{count} needs attention", "{count} need attention");
    ui::text(list, type.regular, summary, ui::kGutter + title + 24.0f, kTitleBaseline, 20.0f,
             ui::color::text3);

    // ---- view switcher: .download-tabs ----
    float x = ui::kGutter;
    for (int v = 0; v < static_cast<int>(JobView::count); ++v)
    {
        const JobView view = static_cast<JobView>(v);
        const char *label = job_view_label(view);
        char number[16];
        std::snprintf(number, sizeof(number), "%d", count(view));
        const float label_width = type.medium.measure(label, 18.0f);
        const float width = label_width + 7.0f + type.medium.measure(number, 18.0f) + 2.0f * 18.0f;
        const ui::Rect pill{x, kChipsY, width, kChipHeight};
        const bool current = view == view_;
        ui::focus_ring(list, pill, pill.h * 0.5f,
                       focused && on_views_ && current && !dialog_open_ ? 1.0f : 0.0f);
        if (current)
            list.rounded_rect(pill, pill.h * 0.5f, ui::Color::rgb(0xf4f4f4));
        else
            list.bordered_rect(pill, pill.h * 0.5f, ui::Color::rgb(0xffffff, 0.05f), 1.0f,
                               ui::Color::rgb(0xffffff, 0.14f));
        const ui::Color ink = current ? ui::Color::rgb(0x080808) : ui::color::text;
        ui::text(list, type.medium, label, pill.x + 18.0f, pill.cy() + 6.5f, 18.0f, ink);
        ui::text(list, type.medium, number, pill.x + 18.0f + label_width + 7.0f, pill.cy() + 6.5f,
                 18.0f, ink.with_alpha(0.65f));
        x += width + 8.0f;
    }

    // ---- rows ----
    const std::vector<const Job *> jobs = visible();
    if (jobs.empty())
    {
        // .empty: centred icon, heading, note and the way out.
        static const char *const kEmptyTitle[] = {/* i18n */ "Nothing in progress", /* i18n */ "No finished downloads",
                                                  /* i18n */ "No failed downloads", /* i18n */ "No cancelled downloads"};
        static const char *const kEmptyBody[] = {
            /* i18n */ "Choose a game to add it to your queue.",
            /* i18n */ "Successfully completed downloads appear here.",
            /* i18n */ "Downloads that need your attention appear here.",
            /* i18n */ "Cancelled downloads appear here. Kept partial files can be resumed."};
        const int v = static_cast<int>(view_);
        ui::icon_download(list, 960.0f, 375.0f, 52.0f, ui::color::text2);
        ui::text(list, type.light, tr(kEmptyTitle[v]), 960.0f, 462.0f, 32.0f, ui::color::text,
                 ui::Align::center);
        ui::text(list, type.regular, tr(kEmptyBody[v]), 960.0f, 508.0f, 20.0f, ui::color::text2,
                 ui::Align::center);
        if (view_ == JobView::active)
        {
            const float width = ui::button_width(type, tr("Browse games"));
            ui::button(list, type, {960.0f - width * 0.5f, 550.0f, width, ui::kButtonHeight},
                       tr("Browse games"), ui::ButtonKind::primary, 0.0f);
        }
        return;
    }

    // Rows scrolling up disappear under the view switcher, not over it.
    list.push_clip({0.0f, kListTop - 6.0f, 1920.0f, 1080.0f - kListTop - 70.0f});
    const double now = unix_seconds();
    for (int i = 0; i < static_cast<int>(jobs.size()); ++i)
    {
        const Job &job = *jobs[static_cast<std::size_t>(i)];
        const ui::Rect row{
            ui::kGutter, kListTop + static_cast<float>(i) * (kRowHeight + kRowGap) - scroll_.value,
            kRowWidth, kRowHeight};
        if (row.y > 1080.0f || row.y + row.h < 0.0f)
            continue;
        const bool is_focused = focused && !on_views_ && i == row_ && !dialog_open_;
        if (is_focused)
            list.rounded_rect(row, 18.0f, ui::Color::rgb(0xffffff, 0.06f));

        const Game *game = game_for(state, job);
        const Release *release = release_for(game, job);
        const ui::Rect cover{row.x + kRowPad, row.y + kRowPad, kCover, kCover};
        if (game != nullptr)
            draw_cover(ctx_, list, *game, cover, 12.0f);
        else
            ui::cover_placeholder(list, type, cover, job.title.empty() ? job.filename : job.title,
                                  12.0f);

        // .queue-info: the name and status, where it comes from and goes,
        // the progress, the transfer, then the actions.
        const float tx = cover.x + cover.w + 28.0f;
        const float right_edge = row.x + row.w - kRowPad;
        const float info_width = right_edge - tx;
        const std::string name = game != nullptr     ? game->title
                                 : job.title.empty() ? job.filename
                                                     : job.title;
        const char *status = job.status == "complete" ? tr("Complete") : status_label(job.status);
        if (job.delivery == "torbox" && job.status == "queued" && !job.delivery_phase.empty())
            status = tr(job.delivery_phase.c_str());
        const ui::Color status_ink = job.status == "error"      ? ui::color::bad
                                     : job.status == "complete" ? ui::color::ok
                                     : job.status == "downloading" || job.status == "verifying"
                                         ? ui::Color::rgb(0xd0d0d0)
                                         : ui::color::text2;
        const float status_width = type.regular.measure(status, 16.0f);
        ui::text(list, type.regular, status, right_edge, row.y + kRowPad + 21.0f, 16.0f, status_ink,
                 ui::Align::right);
        ui::text_fit(list, type.medium, name, tx, row.y + kRowPad + 22.0f, 23.0f,
                     info_width - status_width - 24.0f, ui::color::text);

        const Drive *drive = nullptr;
        for (const Drive &d : state.drives)
        {
            if (d.id == job.storage_id)
                drive = &d;
        }
        std::string meta;
        if (release != nullptr)
            meta = release->provider + " \xC2\xB7 " + release->format + " \xC2\xB7 ";
        meta += drive != nullptr ? i18n::place(drive->label) : job.storage_id;
        if (job.delivery == "torbox")
            meta += std::string(" \xC2\xB7 ") + tr("via TorBox");
        if (drive == nullptr && job.status != "complete")
            meta += std::string(" \xC2\xB7 ") + tr("Reconnect this drive");
        ui::text_fit(list, type.regular, meta, tx, row.y + kRowPad + 51.0f, 16.0f, info_width,
                     ui::color::text3);

        const float progress =
            job.total > 0.0 ? static_cast<float>(std::clamp(job.received / job.total, 0.0, 1.0))
                            : 0.0f;
        const ui::Rect bar{tx, row.y + kRowPad + 74.0f, info_width, 6.0f};
        ui::progress_bar(list, bar, progress,
                         job.status == "error"      ? ui::color::bad
                         : job.status == "complete" ? ui::color::ok
                                                    : ui::color::action);
        const std::string left = format_bytes(job.received) + " / " + format_bytes(job.total) +
                                 " \xC2\xB7 " + i18n::percent(progress * 100.0);
        const float transfer_y = row.y + kRowPad + 104.0f;
        ui::text(list, type.regular, left, tx, transfer_y, 16.0f, ui::color::text3);
        std::string right;
        if (!job.error.empty())
        {
            ui::text_fit(list, type.regular, tr(job.error), right_edge, transfer_y, 16.0f, 620.0f,
                         ui::color::bad, ui::Align::right);
        }
        else
        {
            if (job.status == "retrying")
            {
                const int seconds = static_cast<int>(std::max(0.0, std::ceil(job.retry_at - now)));
                right = seconds > 0 ? tr("Retrying in {seconds} s",
                                         {{"seconds", std::to_string(seconds)}})
                                    : std::string(tr("Waiting to retry\xE2\x80\xA6"));
            }
            else if (job.speed > 0.0)
            {
                const double minutes = std::ceil((job.total - job.received) / job.speed / 60.0);
                right = tr("{speed}/s \xC2\xB7 {minutes} min remaining",
                           {{"speed", format_bytes(job.speed)},
                            {"minutes", i18n::count(static_cast<long>(minutes))}});
            }
            else if (job.status == "complete")
            {
                right = job.verification == "sha256" ? tr("SHA-256 verified") : tr("Size verified");
            }
            else
            {
                right = tr("{size} remaining",
                           {{"size", format_bytes(std::max(0.0, job.total - job.received))}});
            }
            ui::text(list, type.regular, right, right_edge, transfer_y, 16.0f, ui::color::text3,
                     ui::Align::right);
        }

        // .queue-actions: small buttons, always shown; the focused one rings.
        const std::vector<RowAction> actions = actions_for(job);
        float ax = tx;
        for (int a = 0; a < static_cast<int>(actions.size()); ++a)
        {
            const char *label = actions[static_cast<std::size_t>(a)].label;
            const float width = type.medium.measure(label, 16.0f) + 2.0f * 22.0f;
            const bool on = is_focused && a == button_;
            ui::button(list, type, {ax, row.y + kRowPad + 128.0f, width, 38.0f}, label,
                       a == 0 ? ui::ButtonKind::primary : ui::ButtonKind::secondary,
                       on ? 1.0f : 0.0f, !ctx_.store.action().busy, 16.0f);
            ax += width + 12.0f;
        }
    }
    list.pop_clip();

    if (!ctx_.store.action().error.empty())
        ui::text_fit(list, type.regular, tr(ctx_.store.action().error), ui::kGutter + 760.0f,
                     kChipsY + 28.0f, 17.0f, 1060.0f - ui::kGutter, ui::color::bad);

    // ---- the cancel dialog ----
    if (dialog_in_.value > 0.01f)
    {
        hui::gfx::DrawList &overlay = frame.overlay;
        const float in = dialog_in_.value;
        overlay.push_opacity(in);
        overlay.rounded_rect({0.0f, 0.0f, 1920.0f, 1080.0f}, 0.0f,
                             ui::color::night0.with_alpha(0.55f));
        const ui::Rect panel{360.0f, 290.0f - 16.0f * (1.0f - in), 1200.0f, 500.0f};
        ui::modal(overlay, panel);
        ui::text_fit(overlay, type.light,
                 dialog_cancelled_ ? (dialog_forget_ ? tr("Remove from history?") : tr("Delete this cancelled download?")) : tr("Cancel this download?"), panel.x + 56.0f,
                 panel.y + 100.0f, 46.0f, panel.w - 112.0f, ui::color::text);
        ui::paragraph(overlay, type.regular,
                      dialog_cancelled_ ? (dialog_forget_
                      ? tr("Only the history entry will be removed. Any partial file stays on its drive and must be deleted before downloading there again.")
                      : tr("Delete the partial file and remove this entry. If the file is already gone, the entry will still be removed. Completed files are kept."))
                      : tr("Keep the partial file to resume later, or delete only the partial file. "
                      "Downloaded files are never deleted through history cleanup."),
                      panel.x + 56.0f, panel.y + 160.0f, 24.0f, panel.w - 112.0f, 36.0f,
                      ui::color::text2, 3);
        if (!dialog_error_.empty())
            ui::paragraph(overlay, type.regular, tr(dialog_error_), panel.x + 56.0f,
                          panel.y + 284.0f, 20.0f, panel.w - 112.0f, 28.0f, ui::color::bad, 3);
        const char *const *buttons = dialog_forget_ ? kForgetButtons : dialog_cancelled_ ? kDeleteButtons : kDialogButtons;
        float bx = panel.x + 56.0f;
        for (int b = 0; b < (dialog_forget_ ? 2 : 3); ++b)
        {
            const ui::ButtonKind kind = b == 1 ? ui::ButtonKind::danger : ui::ButtonKind::secondary;
            const float width = ui::button_width(ctx_.type, tr(buttons[b]), 18.0f, kind);
            ui::button(overlay, type, {bx, panel.y + panel.h - 104.0f, width, 48.0f},
                       tr(buttons[b]), kind, b == dialog_button_ ? 1.0f : 0.0f, !ctx_.store.action().busy, 18.0f);
            bx += width + 16.0f;
        }
        overlay.pop_opacity();
    }
}

std::vector<hui::ui::Hint> DownloadsScreen::hints() const
{
    if (dialog_open_)
        return {{hui::ui::Button::cross, tr("Select")}, {hui::ui::Button::circle, tr("Go back")}};
    if (on_views_)
        return {{hui::ui::Button::dpad, tr("Switch view")},
                {hui::ui::Button::l1, tr("Tabs"), hui::ui::Button::r1}};
    return {{hui::ui::Button::cross, tr("Select")},
            {hui::ui::Button::dpad, tr("Actions")},
            {hui::ui::Button::l1, tr("Tabs"), hui::ui::Button::r1}};
}

} // namespace orbit
