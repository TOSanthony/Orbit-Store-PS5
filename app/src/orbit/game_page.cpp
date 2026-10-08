// Orbit Store TV app - Artwork-led game hub and controller download sheets.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "orbit/screens.hpp"
#include "orbit/i18n.hpp"
#include <algorithm>
#include <cmath>
#include <limits>

namespace orbit
{
namespace
{
constexpr float kBodyTop = 430.0f;
constexpr float kBodyBottom = 978.0f;
constexpr float kLeft = 70.0f;
const char *primary_label(const Release &release, bool again, bool waiting,
                         const CollectionState &collection, const Snapshot &state, bool torbox)
{
    if (!state.paired) return tr("Opening console session\xE2\x80\xA6");
    if (waiting) return tr("Please wait\xE2\x80\xA6");
    if (!again && collection.job != nullptr) return tr("View download");
    if (!again && collection.kind == CollectionState::Kind::library && collection.library != nullptr)
        return tr("View in Library");
    if (torbox) return tr("Download via TorBox");
    if (release.browser_only()) return tr("Open browser version");
    return collection.job != nullptr || again ? tr("Download again") : tr("Download to PS5");
}
std::string heading(std::string value)
{
    // Preserve UTF-8; uppercase ASCII just as the browser's title treatment.
    for (char &c : value) if (c >= 'a' && c <= 'z') c -= 'a' - 'A';
    return value;
}
}

GamePage::GamePage(Context &context) : Screen(context) {}

void GamePage::open(const std::string &game_id)
{
    game_id_ = game_id;
    option_ = focus_ = 0;
    sheet_ = Sheet::none;
    download_again_ = waiting_ = torbox_ = false;
    error_.clear();
    page_scroll_.snap(0);
    sheet_scroll_.snap(0);
    action_serial_ = ctx_.store.action().serial;
    favourite_serial_ = ctx_.store.favourite_action().serial;
    const auto &state = ctx_.store.state();
    const int at = default_drive(state.drives, state.system.preferred_storage);
    drive_id_ = at >= 0 ? state.drives[static_cast<std::size_t>(at)].id : std::string();
}

const Drive *GamePage::drive() const
{
    const auto &state = ctx_.store.state();
    for (const auto &d : state.drives) if (d.id == drive_id_) return &d;
    // A disconnected destination must be chosen explicitly, never substituted.
    if (!drive_id_.empty()) return nullptr;
    const int at = default_drive(state.drives, state.system.preferred_storage);
    return at >= 0 ? &state.drives[static_cast<std::size_t>(at)] : nullptr;
}

GamePage::Offer GamePage::offer(const Release &release) const
{
    const auto &state = ctx_.store.state();
    Offer out;
    out.collection = release_state(release, state.jobs, state.library_games());
    out.show_download = download_again_ || out.collection.kind == CollectionState::Kind::none ||
                        out.collection.kind == CollectionState::Kind::related;
    out.drive = drive();
    if (out.drive != nullptr) out.plan = plan_space(release, *out.drive, state.jobs);
    return out;
}

bool GamePage::primary_enabled(const Release &release, const Offer &now) const
{
    const auto &state = ctx_.store.state();
    if (!state.paired || waiting_) return false;
    if (!now.show_download) return true;
    if (now.drive == nullptr || !now.plan.enough || state.system.platform == "desktop") return false;
    if (torbox_) return state.debrid.supports(release);
    return !release.browser_only() || static_cast<bool>(ctx_.open_download);
}

std::string GamePage::delivery_note(const Release &release) const
{
    if (torbox_)
        return ctx_.store.state().debrid.supports(release)
            ? tr("Uses your TorBox account. Follow preparation in Downloads.")
            : tr("TorBox is unavailable for this option. Choose the original source or check App settings.");
    return tr("Choose TorBox to use your connected account for this download.");
}

// One measured layout supplies drawing, focus and scroll bounds. No notes are truncated.
GamePage::Layout GamePage::compose(hui::gfx::DrawList *list, Sheet sheet, bool focused) const
{
    Layout out;
    const auto &state = ctx_.store.state();
    const auto &type = ctx_.type;
    const Game *game = state.game(game_id_);
    if (game == nullptr || game->releases.empty()) return out;
    const int selected = std::clamp(option_, 0, static_cast<int>(game->releases.size()) - 1);
    const auto &release = game->releases[static_cast<std::size_t>(selected)];
    const Offer now = offer(release);
    const auto paragraph = [&](const std::string &text, float x, float &y, float width,
                               float size = 22.0f, ui::Color ink = ui::color::text2)
    {
        if (text.empty()) return;
        const auto lines = type.regular.font->wrap(text, size, width);
        if (list) ui::paragraph(*list, type.regular, text, x, y + size, size, width, size * 1.45f,
                               ink, static_cast<int>(lines.size()));
        y += size * 1.45f * static_cast<float>(lines.size());
    };
    const auto control = [&](Slot slot, int index, ui::Rect r, const std::string &label,
                             bool enabled = true, bool fixed = false, bool primary = false)
    {
        const float focus = focused && static_cast<int>(out.controls.size()) == focus_ ? 1.0f : 0.0f;
        out.controls.push_back({slot, index, r, enabled, fixed});
        if (list && !fixed)
        {
            ui::button(*list, type, r, label, primary ? ui::ButtonKind::light : ui::ButtonKind::secondary,
                       focus, enabled, 23);
        }
        return focus;
    };
    const auto card = [&](Slot slot, int index, float x, float y, float width,
                          const std::string &title, const std::string &detail,
                          bool checked, bool enabled = true)
    {
        const float text_width = width - 110;
        const float title_height = 27 * 1.3f * static_cast<float>(type.medium.font->wrap(title, 27, text_width).size());
        const float detail_height = detail.empty() ? 0 : 21 * 1.45f * static_cast<float>(type.regular.font->wrap(detail, 21, text_width).size());
        const float height = std::max(112.0f, 38 + title_height + detail_height);
        const ui::Rect r{x, y, width, height};
        const float focus = focused && static_cast<int>(out.controls.size()) == focus_ ? 1.0f : 0.0f;
        out.controls.push_back({slot, index, r, enabled, false});
        if (list)
        {
            ui::focus_ring(*list, r, 20, focus);
            ui::surface(*list, r, 20, checked ? ui::color::field_open : ui::color::field);
            list->push_opacity(enabled ? 1 : 0.4f);
            if (checked)
            {
                list->circle(x + 34, y + 44, 15, ui::color::text);
                ui::icon_check(*list, x + 34, y + 44, 18, ui::color::night1);
            }
            else list->ring(x + 34, y + 44, 14, 1, ui::color::text3);
            ui::paragraph(*list, type.medium, title, x + 68, y + 47, 27, text_width, 35, ui::color::text, 999);
            if (!detail.empty())
                ui::paragraph(*list, type.regular, detail, x + 68, y + 35 + title_height, 21,
                              text_width, 30.45f, ui::color::text2, 999);
            list->pop_opacity();
        }
        return height;
    };
    const auto budget = [&](float x, float &y, float width)
    {
        if (now.drive == nullptr) return;
        const std::pair<std::string, std::string> cells[] = {
            {tr("Free now"), format_bytes(now.drive->free_bytes)},
            {tr("Unfinished downloads"), format_bytes(now.drive->pending_bytes)},
            {now.plan.planned ? tr("After your queue") : tr("After queue + this download"),
             now.plan.after < 0 ? tr("{size} short", {{"size",format_bytes(-now.plan.after)}}) : format_bytes(now.plan.after)}};
        for (const auto &cell : cells)
        {
            if (list)
            {
                ui::text_fit(*list, type.regular, cell.first, x, y + 22, 20, width - 230, ui::color::text2);
                ui::text(*list, type.medium, cell.second, x + width, y + 22, 21, ui::color::text, ui::Align::right);
            }
            y += 40;
        }
    };
    if (sheet == Sheet::none)
    {
        const std::string title = heading(game->title);
        const float size = game->title.size() > 34 ? 68.0f : 92.0f;
        const float width = 960;
        float y = 185;
        const auto lines = type.medium.font->wrap(title, size, width);
        if (list) ui::paragraph(*list, type.medium, title, kLeft, y + size * 0.85f, size,
                               width, size * 1.04f, ui::color::text, static_cast<int>(lines.size()));
        y += static_cast<float>(lines.size()) * size * 1.04f + 25;
        paragraph(game->tagline, kLeft, y, width, 25, ui::color::text);
        if (!game->tagline.empty()) y += 15;
        paragraph(game->description, kLeft, y, std::min(width, type.regular.measure("0", 22) * 54), 22);
        y += 24;
        std::string meta = release.provider + " \xC2\xB7 " + release.format + " \xC2\xB7 " +
                           format_bytes(release.size_bytes) + " \xC2\xB7 PS5";
        if (!game->genre.empty()) meta += " \xC2\xB7 " + i18n::genre(game->genre);
        paragraph(meta, kLeft, y, width, 22);
        y = std::max(830.0f, y + 60);
        const char *label = primary_label(release, download_again_, waiting_, now.collection, state, torbox_);
        const float button_width = std::clamp(type.medium.measure(label, 23) + 76, 460.0f, 800.0f);
        control(Slot::primary, 0, {kLeft,y,button_width,88}, label, !waiting_, false, true);
        for (int i=0;i<2;++i)
        {
            const ui::Rect r{kLeft + button_width + 24 + i * 108.0f,y,88,88};
            const Slot slot = i == 0 ? Slot::favourite : Slot::about;
            const float focus = control(slot,0,r,"",!waiting_);
            if (list)
            {
                list->bordered_rect(r,44,ui::Color{0,0,0,0},1,ui::color::text.with_alpha(0.35f));
                if (i == 0) ui::icon_heart(*list,r.cx(),r.cy(),36,ui::color::text);
                else for (int d=-1;d<=1;++d) list->circle(r.cx()+d*12,r.cy(),3,ui::color::text);
                if (focus > 0)
                    ui::text(*list,type.regular,i == 0 ? (state.favourite(game->id) ? tr("Saved to favourites") : tr("Add to favourites")) : tr("About this game"),
                             r.cx(),y+123,18,ui::color::text2,ui::Align::center);
                if (i==0 && state.favourite(game->id)) list->circle(r.cx()+26,r.cy()-26,5,ui::color::text);
            }
        }
        y += 140;
        paragraph(error_,kLeft,y,1000,21);
        out.end = y;
        return out;
    }
    // Header/footer controls are fixed; all body content can scroll independently.
    control(Slot::back,0,{116,350,120,50},tr("Back"),true,true);
    control(Slot::close,0,{1730,350,66,50},tr("Close"),true,true);
    float y = 0;
    if (sheet == Sheet::downloads)
    {
        const float width = 554;
        const auto setting = [&](Slot slot,float x,const std::string &label,const std::string &value,const std::string &note)
        {
            const ui::Rect r{x,0,width,128};
            const float f = focused && static_cast<int>(out.controls.size()) == focus_ ? 1 : 0;
            out.controls.push_back({slot,0,r,!waiting_,false});
            if (list)
            {
                ui::focus_ring(*list,r,20,f);ui::surface(*list,r,20,ui::color::field);
                ui::text(*list,type.regular,label,x+26,36,21,ui::color::text2);
                ui::text_fit(*list,type.medium,value,x+26,76,26,width-65,ui::color::text);
                ui::text_fit(*list,type.regular,note,x+26,108,20,width-85,ui::color::text2);
                const float cx = x + width - 30;
                list->line(cx-4,57,cx+3,64,2,ui::color::text2);
                list->line(cx+3,64,cx-4,71,2,ui::color::text2);
            }
        };
        setting(Slot::source,116,tr("Source"),release.provider + " \xC2\xB7 " + release.format,
                release.title_id + (release.version.empty()?"":" \xC2\xB7 v"+release.version) + " \xC2\xB7 " + format_bytes(release.size_bytes));
        if (now.show_download)
        {
            setting(Slot::delivery,684,tr("Download using"),torbox_ ? "TorBox" : tr("{provider} directly",{{"provider",release.provider}}),
                    state.debrid.supports(release) && !torbox_ ? tr("TorBox available") : "");
            setting(Slot::drive,1252,tr("Save to"),now.drive ? i18n::place(now.drive->label) : tr("Select storage"),
                    now.drive ? tr("{size} free",{{"size",format_bytes(now.drive->free_bytes)}}) : "");
        }
        y=162;
        const float width_notes=1040;
        const float notes_top=y;
        if (now.show_download && release.browser_only() && !torbox_)
        {
            paragraph(tr("Vikingfile: start here"),116,y,width_notes,25,ui::color::text);y+=16;
            paragraph(tr("This option downloads through the PS5 web browser. Open the browser version of Orbit to use it."),116,y,width_notes,20);y+=12;
            paragraph(tr("1. Select Open browser version. Your game, source and save location will already be selected."),116,y,width_notes,20);y+=12;
            paragraph(tr("2. Check your drive and select Open download page on PS5. On Vikingfile, complete any verification and select the site's Download button."),116,y,width_notes,20);y+=12;
            paragraph(tr("3. Return to Orbit Store. Open Downloads to follow progress once Orbit has checked the file."),116,y,width_notes,20);y+=16;
        }
        if(now.show_download && release.browser_only() && !torbox_)
            out.controls.push_back({Slot::reading,0,{116,notes_top,width_notes,y-notes_top},true,false});
        if (now.show_download && (torbox_ || (state.debrid.connected && !state.debrid.supports(release))))
        { paragraph(delivery_note(release),116,y,width_notes,21);y+=18; }
        if (now.collection.library)
        {
            paragraph(now.collection.kind == CollectionState::Kind::related
                ? tr("A related copy is in Library. Its format, filename or version does not confirm a match for this option.")
                : tr("In your Library on {location}.",{{"location",i18n::place(now.collection.library->location)}}),116,y,width_notes);
            y+=16;
            if (now.collection.kind == CollectionState::Kind::related)
            { control(Slot::related,0,{116,y,450,58},tr("View related copy"));y+=80; }
        }
        if (!download_again_ && (now.collection.kind == CollectionState::Kind::library ||
            (now.collection.job && now.collection.job->status == "complete")))
        { control(Slot::again,0,{116,y,450,58},tr("Download another copy"));y+=80; }
        if (now.collection.job && now.collection.job->status == "complete")
        { paragraph(tr("Download history records a completed transfer. Find it in Library to check its current location."),116,y,width_notes,20);y+=16; }
        if (now.show_download && (!now.drive || !now.plan.enough))
        {
            paragraph(!now.drive ? tr("Connect a writable drive to your PS5.") :
                tr("Not enough space after unfinished downloads. Free space or cancel an item in Downloads."),116,y,width_notes);
            y+=16;
        }
        paragraph(error_,116,y,width_notes);
        control(Slot::primary,0,{1252,842,554,84},"",primary_enabled(release,now),true,true);
    }
    else if (sheet == Sheet::source)
    {
        paragraph(game->releases.size()>1 ? tr("Choose a source and format for this game.") : tr("Available from your enabled sources."),116,y,1650);y+=22;
        for (int i=0;i<static_cast<int>(game->releases.size());++i)
        {
            const auto &r=game->releases[static_cast<std::size_t>(i)];
            const auto presence=release_state(r,state.jobs,state.library_games());
            std::string detail=r.title_id + (r.version.empty()?"":" \xC2\xB7 v"+r.version) + " \xC2\xB7 " + format_bytes(r.size_bytes);
            if (!r.region.empty()) detail += " \xC2\xB7 " + r.region;
            if (presence.label) detail += " \xC2\xB7 " + std::string(presence.label);
            y+=card(Slot::choice,i,116,y,1690,r.provider+" \xC2\xB7 "+r.format,detail,i==selected,!waiting_)+18;
        }
    }
    else if (sheet == Sheet::delivery)
    {
        y+=card(Slot::choice,0,116,y,1690,tr("{provider} directly",{{"provider",release.provider}}),"",!torbox_,!waiting_)+18;
        const bool connected=state.debrid.connected || torbox_;
        y+=card(Slot::choice,1,116,y,1690,connected ? "TorBox" : tr("Set up TorBox"),
                connected ? delivery_note(release) : tr("Connect your TorBox account in App settings."),torbox_,
                !waiting_ && (connected ? state.debrid.supports(release) : static_cast<bool>(ctx_.open_download) || static_cast<bool>(ctx_.open_browser)))+18;
    }
    else if (sheet == Sheet::storage)
    {
        for(int i=0;i<static_cast<int>(state.drives.size());++i)
        {
            const auto &d=state.drives[static_cast<std::size_t>(i)];
            y+=card(Slot::choice,i,116,y,1690,i18n::place(d.label),tr("{size} free",{{"size",format_bytes(d.free_bytes)}})+"\n"+
                    tr("Saves to {path}",{{"path",d.path}}),now.drive && d.id==now.drive->id,!waiting_)+18;
        }
        if (state.drives.empty()) paragraph(tr("Connect a writable drive to your PS5."),116,y,1690);
        y+=12;budget(116,y,1690);
        if (now.drive && list)
            ui::progress_bar(*list,{116,y,1690,10},now.drive->total_bytes>0 ? static_cast<float>(1-now.drive->free_bytes/now.drive->total_bytes) : 0,ui::color::text);
        y+=34;
        paragraph(tr("Estimates include paused and failed downloads. Other apps can change free space."),116,y,1690,20);
    }
    else if (sheet == Sheet::about)
    {
        paragraph(game->description,116,y,1650,25);y+=30;
        std::vector<std::pair<std::string,std::string>> facts;
        if(!game->publisher.empty())facts.emplace_back(tr("Publisher"),game->publisher);
        if(!game->release_date.empty())facts.emplace_back(tr("PS5 release"),format_date(game->release_date));
        if(!game->genre.empty())facts.emplace_back(tr("Genre"),i18n::genre(game->genre));
        facts.emplace_back(tr("Format"),release.format);
        facts.emplace_back(tr("Title ID"),release.title_id);
        facts.emplace_back(tr("Source"),release.provider);
        if(!release.version.empty())facts.emplace_back(tr("Version"),release.version);
        if(!release.region.empty())facts.emplace_back(tr("Region"),release.region);
        facts.emplace_back(tr("Download size"),format_bytes(release.size_bytes));
        for(std::size_t first=0;first<facts.size();first+=3)
        {
            float bottom=y;
            for(std::size_t i=first;i<std::min(first+3,facts.size());++i)
            {
                const float x=116+static_cast<float>(i-first)*568;
                float fy=y;
                paragraph(facts[i].first,x,fy,520,19,ui::color::text3);fy+=10;
                paragraph(facts[i].second,x,fy,520,24,ui::color::text);
                bottom=std::max(bottom,fy);
            }
            y=bottom+35;
        }
        paragraph(tr("Saves a file to your console. Installation and launching are separate."),116,y,1690,20);
    }
    out.end=y+20;
    return out;
}

void GamePage::show_sheet(Sheet sheet, Slot preferred, int index)
{
    sheet_=sheet;
    sheet_scroll_.snap(0);
    const auto where=compose(nullptr,sheet,false);
    focus_=0;
    for(std::size_t i=0;i<where.controls.size();++i)
        if(where.controls[i].slot==preferred && where.controls[i].index==index && where.controls[i].enabled)
        {focus_=static_cast<int>(i);return;}
    for(std::size_t i=0;i<where.controls.size();++i)
        if(where.controls[i].enabled && where.controls[i].slot!=Slot::back && where.controls[i].slot!=Slot::close)
        {focus_=static_cast<int>(i);return;}
}
void GamePage::back_sheet()
{
    const auto previous=sheet_;
    if(previous==Sheet::source)show_sheet(Sheet::downloads,Slot::source);
    else if(previous==Sheet::delivery)show_sheet(Sheet::downloads,Slot::delivery);
    else if(previous==Sheet::storage)show_sheet(Sheet::downloads,Slot::drive);
    else show_sheet(Sheet::none,previous==Sheet::about?Slot::about:Slot::primary);
}

Intent GamePage::activate(const Control &control,const Game &game,const Release &release,
                          const Offer &now,Feedback &feedback)
{
    Intent intent;
    const auto &state=ctx_.store.state();
    if(!control.enabled)return intent;
    feedback.play(hui::audio::Cue::select);
    switch(control.slot)
    {
    case Slot::reading: break;
    case Slot::back: back_sheet();break;
    case Slot::close: show_sheet(Sheet::none,sheet_==Sheet::about?Slot::about:Slot::primary);break;
    case Slot::favourite:
        if(state.paired && !ctx_.store.favourite_action().busy)ctx_.store.set_favourite(game.id,!state.favourite(game.id));
        break;
    case Slot::about: show_sheet(Sheet::about,Slot::back);break;
    case Slot::source: show_sheet(Sheet::source,Slot::choice,option_);break;
    case Slot::delivery: show_sheet(Sheet::delivery,Slot::choice,torbox_?1:0);break;
    case Slot::drive:
    {
        int at=0;
        for(std::size_t i=0;i<state.drives.size();++i)if(now.drive && now.drive->id==state.drives[i].id)at=static_cast<int>(i);
        show_sheet(Sheet::storage,Slot::choice,at);break;
    }
    case Slot::choice:
        if(sheet_==Sheet::source)
        {
            if(option_!=control.index){option_=control.index;torbox_=false;download_again_=false;}
        }
        else if(sheet_==Sheet::storage)drive_id_=state.drives[static_cast<std::size_t>(control.index)].id;
        else if(sheet_==Sheet::delivery)
        {
            if(control.index==1 && !state.debrid.connected && !torbox_)
            {
                // Keep this view and all selections while the user connects in the browser.
                if(now.drive && ctx_.open_download)ctx_.open_download({game.id,release.id,now.drive->id});
                else if(ctx_.open_browser)ctx_.open_browser();
                return intent;
            }
            torbox_=control.index==1;
        }
        error_.clear();back_sheet();break;
    case Slot::again: download_again_=true;error_.clear();show_sheet(Sheet::downloads);break;
    case Slot::related:
        if(now.collection.library)
        {intent.kind=Intent::Kind::show_library;intent.id=now.collection.library->title_id;intent.detail=now.collection.library->source_key;}
        break;
    case Slot::primary:
        if(sheet_==Sheet::none){show_sheet(Sheet::downloads);break;}
        // Opening/reviewing a sheet never executes these actions.
        if(!primary_enabled(release,now))break;
        if(!download_again_ && now.collection.job)
        {intent.kind=Intent::Kind::show_job;intent.id=now.collection.job->id;}
        else if(!download_again_ && now.collection.kind==CollectionState::Kind::library && now.collection.library)
        {intent.kind=Intent::Kind::show_library;intent.id=now.collection.library->title_id;intent.detail=now.collection.library->source_key;}
        else if(release.browser_only() && !torbox_)
        {
            if(!ctx_.open_download({release.game_id,release.id,now.drive->id}))
                error_=tr("Open browser version");
        }
        else
        {
            error_.clear();waiting_=true;action_serial_=ctx_.store.action().serial;
            ctx_.store.download(release.id,now.drive->id,torbox_);
        }
        break;
    }
    return intent;
}

Intent GamePage::update(const InputFrame &input,float dt,Feedback &feedback,bool focused)
{
    Intent intent;
    const auto &state=ctx_.store.state();
    const Game *game=state.game(game_id_);
    if(!game || game->releases.empty())
    {
        if(focused && (input.is_pressed(Action::back)||input.is_pressed(Action::confirm)))intent.kind=Intent::Kind::close;
        return intent;
    }
    option_=std::clamp(option_,0,static_cast<int>(game->releases.size())-1);
    if(waiting_ && ctx_.store.action().serial!=action_serial_ && !ctx_.store.action().busy)
    {
        waiting_=false;action_serial_=ctx_.store.action().serial;
        if(ctx_.store.action().error.empty())
        {
            intent.kind=Intent::Kind::show_job;intent.id=ctx_.store.action().created_job;
            intent.toast=tr("Added to your downloads");return intent;
        }
        error_=ctx_.store.action().error;
    }
    if(ctx_.store.favourite_action().serial!=favourite_serial_ && !ctx_.store.favourite_action().busy)
    {
        favourite_serial_=ctx_.store.favourite_action().serial;
        if(!ctx_.store.favourite_action().error.empty())error_=ctx_.store.favourite_action().error;
    }
    if (focused && sheet_ == Sheet::none && !waiting_)
    {
        if (input.is_pressed(Action::north) || input.is_pressed(Action::west))
        {
            const auto &release = game->releases[static_cast<std::size_t>(option_)];
            const Slot slot = input.is_pressed(Action::north) ? Slot::favourite : Slot::about;
            return activate({slot, 0, {}, true, false}, *game, release, offer(release), feedback);
        }
    }
    auto where=compose(nullptr,sheet_,false);
    if(where.controls.empty())return intent;
    focus_=std::clamp(focus_,0,static_cast<int>(where.controls.size())-1);
    if(focused && input.is_pressed(Action::back))
    {
        if(sheet_==Sheet::none)intent.kind=Intent::Kind::close;
        else back_sheet();
        feedback.play(hui::audio::Cue::back);return intent;
    }
    auto &scroll=sheet_==Sheet::none?page_scroll_:sheet_scroll_;
    const float visible=sheet_==Sheet::none?980:kBodyBottom-kBodyTop;
    bool moved=false;
    bool read_scroll=false;
    if(focused && input.nav!=Direction::none)
    {
        const auto &current_control=where.controls[static_cast<std::size_t>(focus_)];
        if(current_control.slot==Slot::reading && (input.nav==Direction::up || input.nav==Direction::down))
        {
            const float first=std::max(0.0f,current_control.rect.y-14);
            const float last=std::max(first,current_control.rect.y+current_control.rect.h-visible+14);
            const float next=std::clamp(scroll.target+(input.nav==Direction::down?130.0f:-130.0f),first,last);
            read_scroll=next!=scroll.target;
            scroll.target=next;
        }
        if(read_scroll) {}
        else if((sheet_==Sheet::none || sheet_==Sheet::about) && (input.nav==Direction::up || input.nav==Direction::down))
            scroll.target=std::clamp(scroll.target+(input.nav==Direction::down?150.0f:-150.0f),0.0f,std::max(0.0f,where.end-visible));
        else
        {
            const auto screen_rect=[&](const Control &c){auto r=c.rect;if(!c.fixed)r.y+=(sheet_==Sheet::none?0:kBodyTop)-scroll.target;return r;};
            const auto current=screen_rect(where.controls[static_cast<std::size_t>(focus_)]);
            float best=std::numeric_limits<float>::max();int next=-1;
            for(std::size_t i=0;i<where.controls.size();++i)
            {
                if(static_cast<int>(i)==focus_ || !where.controls[i].enabled)continue;
                const auto r=screen_rect(where.controls[i]);
                float dx=r.cx()-current.cx(),dy=r.cy()-current.cy();
                const bool vertical=input.nav==Direction::up || input.nav==Direction::down;
                const float along=vertical?dy:dx,cross=vertical?dx:dy;
                if((input.nav==Direction::up || input.nav==Direction::left)?along>=-1:along<=1)continue;
                const float score=std::abs(along)+std::abs(cross)*3;
                if(score<best){best=score;next=static_cast<int>(i);}
            }
            if(next>=0){focus_=next;moved=true;feedback.play(hui::audio::Cue::focus);}
            else scroll.target=std::clamp(scroll.target+(input.nav==Direction::down?130.0f:input.nav==Direction::up?-130.0f:0),0.0f,std::max(0.0f,where.end-visible));
        }
    }
    if(moved)
    {
        const auto &c=where.controls[static_cast<std::size_t>(focus_)];
        if(!c.fixed)
        {
            if(c.rect.y-scroll.target<14)scroll.target=std::max(0.0f,c.rect.y-14);
            if(c.rect.y+c.rect.h-scroll.target>visible-14)scroll.target=c.rect.y+c.rect.h-visible+14;
        }
    }
    scroll.target=std::clamp(scroll.target,0.0f,std::max(0.0f,where.end-visible));
    scroll.update(dt,18);
    if(focused && input.is_pressed(Action::confirm))
    {
        const auto &c=where.controls[static_cast<std::size_t>(focus_)];
        if(!c.enabled)refuse(feedback,input);
        else intent=activate(c,*game,game->releases[static_cast<std::size_t>(option_)],offer(game->releases[static_cast<std::size_t>(option_)]),feedback);
    }
    return intent;
}

void GamePage::draw(Frame &frame,bool focused) const
{
    const auto &state=ctx_.store.state();
    const auto &type=ctx_.type;
    const Game *game=state.game(game_id_);
    auto &scene=frame.scene;
    if(!game || game->releases.empty())
    {
        ui::text(scene,type.light,tr("This game is no longer available"),70,420,52,ui::color::text);
        ui::text(scene,type.regular,tr("Its download source was turned off. Press Circle to go back."),70,470,22,ui::color::text2);return;
    }
    scene.push_clip({0,132,1920,865});
    scene.push_transform(1,0,0,0,-page_scroll_.value);
    compose(&scene,Sheet::none,focused && sheet_==Sheet::none);
    scene.pop_transform();scene.pop_clip();
    if(sheet_==Sheet::none)return;
    auto &list=frame.overlay;
    list.rounded_rect({0,0,1920,1080},0,ui::Color::rgb(0x000000,0.35f));
    const ui::Rect panel{70,320,1780,710};
    list.shadow(panel,30,50,ui::Color::rgb(0x000000,0.5f));
    list.gradient_rect(panel,30,ui::Color::rgb(0x242424,0.98f),ui::Color::rgb(0x101010,0.99f));
    list.bordered_rect(panel,30,ui::Color{0,0,0,0},1,ui::color::line);
    const auto &release=game->releases[static_cast<std::size_t>(std::clamp(option_,0,static_cast<int>(game->releases.size())-1))];
    const auto now=offer(release);
    const char *title=sheet_==Sheet::downloads?tr("Download options"):sheet_==Sheet::source?tr("Source"):
        sheet_==Sheet::delivery?tr("Download using"):sheet_==Sheet::storage?tr("Save to"):tr("About this game");
    ui::text_fit(list,type.medium,title,270,386,34,1380,ui::color::text);
    list.push_clip({100,kBodyTop-8,1720,kBodyBottom-kBodyTop+8});
    list.push_transform(1,0,0,0,kBodyTop-sheet_scroll_.value);
    const auto where=compose(&list,sheet_,focused);
    list.pop_transform();list.pop_clip();
    if(sheet_==Sheet::downloads)
    {
        list.rounded_rect({1226,738,596,253},22,ui::Color::rgb(0x111111,0.98f));
        if(now.show_download && now.drive)
        {
            list.rounded_rect({1252,765,554,1},0,ui::color::line);
            ui::text_fit(list,type.regular,now.plan.planned?tr("After your queue"):tr("After queue + this download"),1252,808,21,370,ui::color::text2);
            ui::text(list,type.medium,now.plan.after<0?tr("{size} short",{{"size",format_bytes(-now.plan.after)}}):format_bytes(now.plan.after),1806,808,22,ui::color::text,ui::Align::right);
        }
        ui::paragraph(list,type.regular,tr("Saves a file to your console. Installation and launching are separate."),1252,958,17,554,24,ui::color::text3,3);
    }
    for(std::size_t i=0;i<where.controls.size();++i)
    {
        const auto &c=where.controls[i];if(!c.fixed)continue;
        const float focus=focused && static_cast<int>(i)==focus_?1:0;
        if(c.slot==Slot::primary)
        {
            const char *label=primary_label(release,download_again_,waiting_,now.collection,state,torbox_);
            const float size=ui::fit_size(type.medium,label,25,18,c.rect.w-45);
            ui::button(list,type,c.rect,label,ui::ButtonKind::light,focus,c.enabled,size);
        }
        else if(c.slot==Slot::back)ui::button(list,type,c.rect,tr("Back"),ui::ButtonKind::secondary,focus,true,22);
        else
        {
            ui::focus_ring(list,c.rect,c.rect.h/2,focus);
            const float x=c.rect.cx(),y=c.rect.cy();
            list.line(x-9,y-9,x+9,y+9,2,ui::color::text);list.line(x-9,y+9,x+9,y-9,2,ui::color::text);
        }
    }
}
std::vector<hui::ui::Hint> GamePage::hints() const
{
    const auto where=compose(nullptr,sheet_,false);
    const bool reading=focus_>=0 && focus_<static_cast<int>(where.controls.size()) && where.controls[static_cast<std::size_t>(focus_)].slot==Slot::reading;
    if(sheet_==Sheet::about || reading)return {{hui::ui::Button::dpad,tr("Scroll")},{hui::ui::Button::circle,tr("Back")}};
    if (sheet_ == Sheet::none)
        return {{hui::ui::Button::cross,tr("Select")},{hui::ui::Button::circle,tr("Back")},
                {hui::ui::Button::triangle,tr("Favourite")},{hui::ui::Button::square,tr("Details")}};
    return {{hui::ui::Button::cross,tr("Select")},{hui::ui::Button::circle,tr("Back")}};
}
} // namespace orbit
