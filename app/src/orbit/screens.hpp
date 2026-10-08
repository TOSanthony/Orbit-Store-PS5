// Orbit Store TV app - The storefront's screens and what they share.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Screens are immediate mode, like the kit: update() owns state and
// animation, draw() is const and only records shapes. A screen never talks to
// the network; it reads the Store's snapshot, asks the ArtCache for images
// and returns an Intent when the app should navigate.

#pragma once

#include "orbit/browser_handoff.hpp"
#include "core/input.hpp"
#include "gfx/backdrop_spec.hpp"
#include "gfx/draw_list.hpp"
#include "orbit/art.hpp"
#include "orbit/store.hpp"
#include "orbit/ui.hpp"
#include "ui/components/keyboard.hpp"
#include "ui/feedback.hpp"
#include "ui/glyphs.hpp"
#include "ui/theme.hpp"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace orbit
{

using hui::Action;
using hui::Direction;
using hui::InputFrame;
using Feedback = hui::ui::Feedback;

// One frame, back to front: backdrop, scene, [glass capture], overlay.
struct Frame
{
    hui::gfx::BackdropSpec backdrop;
    hui::gfx::DrawList scene;
    hui::gfx::DrawList overlay;
    bool glass = false;
    std::uint32_t glass_texture = 0;

    void reset()
    {
        backdrop = {};
        scene.clear();
        overlay.clear();
        glass = false;
    }
};

// Long-lived services every screen receives.
struct Context
{
    Context(const ui::Type &type_, const hui::ui::Fonts &fonts_, Store &store_, ArtCache &art_)
        : type(type_), fonts(fonts_), store(store_), art(art_)
    {
    }

    const ui::Type &type;
    const hui::ui::Fonts &fonts; // the kit's slots, for glyphs and the keyboard
    Store &store;
    ArtCache &art;
    std::uint32_t mark = 0;        // the planet mark, 128 px
    std::uint32_t loader_mark = 0; // the mark with a faint ring, 256 px
    std::string today;             // YYYY-MM-DD, for Latest releases
    std::string app_version;       // this app's contentVersion
    // Opens the browser version of Orbit; empty where it is not possible.
    std::function<bool()> open_browser;
    // Opens the exact option and destination; the browser still asks to proceed.
    std::function<bool(const BrowserSelection &)> open_download;
    float clock = 0.0f; // seconds since the app opened
};

// The tabs, in the browser storefront's order; it opens on Discover.
namespace tab
{
constexpr int discover = 0;
constexpr int browse = 1;
constexpr int library = 2;
constexpr int downloads = 3;
constexpr int count = 4;
} // namespace tab

struct Intent
{
    enum class Kind : std::uint8_t
    {
        none,
        top_bar,      // the focus left the screen upward
        open_game,    // id: a game
        show_job,     // id: a job (Downloads)
        show_tab,     // tab: one of tab::
        show_library, // id: a title ID, detail: its Library source key
        close,        // the game page is done
    } kind = Kind::none;
    std::string id;
    std::string detail;
    int tab = 0;
    std::string toast; // a short confirmation the app shows
};

class Screen
{
  public:
    explicit Screen(Context &context) : ctx_(context)
    {
    }
    virtual ~Screen() = default;
    // The focus arrives on the screen (from the top bar, or after a dialog).
    virtual void focus()
    {
    }
    // focused is false while the top bar has the focus: animate only.
    virtual Intent update(const InputFrame &input, float dt, Feedback &feedback, bool focused) = 0;
    virtual void draw(Frame &frame, bool focused) const = 0;
    // The game whose art fills the background, if any (Discover only, as on the web).
    virtual std::string backdrop_game() const
    {
        return {};
    }
    virtual std::vector<hui::ui::Hint> hints() const
    {
        return {};
    }
    // True while a dialog or keyboard of the screen takes all input.
    virtual bool modal() const
    {
        return false;
    }

  protected:
    Context &ctx_;
};

// A dropdown's controller state (the browser's Select): Cross opens it, up
// and down move, Cross chooses and Circle closes it unchanged.
struct Picker
{
    bool open = false;
    int active = 0;
    ui::Ease in;

    void show(int selected)
    {
        open = true;
        active = selected < 0 ? 0 : selected;
    }
    // While open: the chosen index once it is chosen, else -1.
    int update(const InputFrame &input, int count, Feedback &feedback);
    void animate(float dt);
};

// ---- shared drawing (views.cpp) ----

// A game's cover, fading in when its art arrives; a placeholder before.
void draw_cover(Context &ctx, hui::gfx::DrawList &list, const Game &game, const ui::Rect &r,
                float radius, float opacity = 1.0f);
// A cover tile as the browser's .game-tile: grows and rings when focused;
// current (0..1) outlines the game the hero shows while the focus is elsewhere.
void draw_tile(Context &ctx, hui::gfx::DrawList &list, const Game &game, const ui::Rect &r,
               float focus, float current = 0.0f, float opacity = 1.0f);
// Under a tile: the name (up to two lines), the download state if any, and
// the release date when asked. Returns the baseline after it.
float draw_tile_caption(Context &ctx, hui::gfx::DrawList &list, const Game &game, const char *state,
                        float x, float cover_bottom, float width, float focus, bool date);
// The art behind everything for one game, at an opacity (crossfades).
void draw_backdrop_art(Context &ctx, hui::gfx::DrawList &list, const Game &game, float opacity,
                       float height = 1080.0f);
// The navy veils over the art (the browser's .backdrop::after).
void draw_veils(hui::gfx::DrawList &list, bool discover = false);
// Refusal at an edge: quiet, and silent on a held direction.
void refuse(Feedback &feedback, const InputFrame &input);
// The kit theme the on-screen keyboard is drawn in.
hui::ui::Theme keyboard_theme();

// ---- the screens ----

class DiscoverScreen final : public Screen
{
  public:
    explicit DiscoverScreen(Context &context);
    void focus() override;
    Intent update(const InputFrame &input, float dt, Feedback &feedback, bool focused) override;
    void draw(Frame &frame, bool focused) const override;
    std::string backdrop_game() const override
    {
        return featured_;
    }
    std::vector<hui::ui::Hint> hints() const override;
    // The game whose page was just closed, so the focus returns to its tile.
    void return_to(const std::string &game_id);

  private:
    struct Rail
    {
        std::string title;
        std::string note;
        std::vector<int> games; // indices into the snapshot's games
        bool grid = false;
    };
    void rebuild();
    void rebuild_grid();
    int page_count() const;
    bool on_pager() const;
    const Game *game_at(int row, int column) const;
    float row_top(int row) const;

    std::vector<Rail> rails_;
    std::vector<int> all_games_;
    int page_ = 0;
    int page_button_ = 1; // Previous, Next
    std::uint64_t catalog_serial_ = ~0ull;
    int row_ = 0;              // hero, horizontal rails, All games grid, then page controls
    std::vector<int> columns_; // focused column per rail
    std::string featured_;
    std::string previous_featured_;
    ui::Ease scroll_;
    std::vector<ui::Ease> rail_scroll_;
    ui::Ease hero_in_;
    ui::Ease button_focus_;
    ui::Ease favourite_focus_;
    int hero_action_ = 0;
    std::vector<std::vector<float>> tile_focus_;
};

class BrowseScreen final : public Screen
{
  public:
    explicit BrowseScreen(Context &context);
    void focus() override;
    Intent update(const InputFrame &input, float dt, Feedback &feedback, bool focused) override;
    void draw(Frame &frame, bool focused) const override;
    std::vector<hui::ui::Hint> hints() const override;
    bool modal() const override
    {
        return keyboard_open_ || picker_.open;
    }
    // Opens the keyboard on the search field (the top bar's search button).
    void start_search();
    void return_to(const std::string &game_id);
    // Previews and tests: type a search without pressing keys.
    void set_query(const std::string &query);
    const BrowseFilters &filters() const
    {
        return filters_;
    }

  private:
    enum class Area : std::uint8_t
    {
        field,
        collection, // All games, Favourites
        filters,    // Source, Format, Region, Download size, Sort by, Reset filters
        grid,
    };
    static constexpr int kFilterControls = 6;
    void refilter();
    ui::Rect tile_rect(int index) const;
    ui::Rect filter_rect(int index) const;
    // The choices a filter offers and which is current.
    std::vector<std::string> filter_options(int filter, int *selected) const;
    void choose(int filter, int option);
    std::string filter_value(int filter) const;

    Area area_ = Area::field;
    int column_ = 0; // within the collection chips or the filters row
    BrowseFilters filters_;
    std::vector<int> results_;
    std::uint64_t catalog_serial_ = ~0ull;
    std::uint64_t favourites_serial_ = ~0ull;
    int index_ = 0;
    bool keyboard_open_ = false;
    hui::ui::Keyboard keyboard_;
    Picker picker_;
    int picking_ = 0; // the filter the open picker belongs to
    ui::Ease scroll_;
    ui::Ease field_focus_;
    ui::Ease keyboard_in_;
    float control_focus_[2 + kFilterControls] = {};
    std::vector<float> tile_focus_;
};

// Library: ShadowMount's inventory as Orbit reports it (src/Library.tsx):
// games installed on the console and sources on its drives, their details,
// mount and unmount, copy and move between drives, and drive storage.
class LibraryScreen final : public Screen
{
  public:
    explicit LibraryScreen(Context &context);
    void focus() override;
    void blur();
    Intent update(const InputFrame &input, float dt, Feedback &feedback, bool focused) override;
    void draw(Frame &frame, bool focused) const override;
    std::vector<hui::ui::Hint> hints() const override;
    bool modal() const override
    {
        return keyboard_open_ || picker_.open || !selected_.empty() || scan_open_;
    }
    // Opens a game's details (View in Library from a game page).
    void show_game(const std::string &title_id, const std::string &source_key);
    // Previews and tests.
    bool details_open() const
    {
        return !selected_.empty();
    }

  private:
    enum class Kind : std::uint8_t
    {
        refresh,
        scan,
        section, // index 0 games, 1 storage
        view,    // the status views
        search,
        location,
        format,
        reset, // Reset library filters (no matches)
        card,  // index into rows_
        drive, // index into the storage drives
        measure,
        cancel_job,
        storage_game, // index into the selected drive's games
        // in the details panel
        action, // index: 0 mount/unmount, 1 copy, 2 move
        destination,
        confirm,
        back,
        // in the scan dialog
        scan_start,
        scan_back,
    };
    struct Control
    {
        Kind kind;
        int index = 0;
        int row = 0;
        ui::Rect rect;
    };
    std::vector<Control> page_controls() const;
    std::vector<Control> panel_controls() const;
    void refilter();
    const LibraryGame *selected_game() const;
    std::vector<const LibraryDrive *> destinations() const;
    std::vector<const LibraryGame *> drive_games() const;
    std::vector<std::string> filter_values() const;
    std::vector<std::string> picker_options(int *selected) const;
    float content_height() const;

    // The page.
    int section_ = 0; // 0 games, 1 storage
    int view_ = 0;
    std::string query_;
    std::string location_;
    std::string format_;
    std::vector<int> rows_; // indices into the snapshot's games
    std::uint64_t serial_ = ~0ull;
    Kind focus_kind_ = Kind::refresh;
    int focus_index_ = 0;
    std::string drive_id_; // the drive the Storage section shows
    bool keyboard_open_ = false;
    hui::ui::Keyboard keyboard_;
    ui::Ease keyboard_in_;
    Picker picker_;
    Kind picking_ = Kind::location;
    ui::Ease scroll_;
    std::vector<float> focus_amount_; // parallel to page_controls(), by position
    // The details panel and its confirmation step.
    std::string selected_;     // title ID
    std::string selected_key_; // source key
    int mode_ = 0;             // 0 none, 1 unmount, 2 copy, 3 move
    std::string destination_id_;
    Kind panel_kind_ = Kind::action;
    int panel_index_ = 0;
    ui::Ease panel_in_;
    std::uint64_t action_serial_ = 0;
    std::string error_;
    // The scan dialog.
    bool scan_open_ = false;
    int scan_button_ = 0;
};

class DownloadsScreen final : public Screen
{
  public:
    explicit DownloadsScreen(Context &context);
    void focus() override;
    Intent update(const InputFrame &input, float dt, Feedback &feedback, bool focused) override;
    void draw(Frame &frame, bool focused) const override;
    std::vector<hui::ui::Hint> hints() const override;
    bool modal() const override
    {
        return dialog_open_;
    }
    // Shows one job: switches to its view and moves the focus to it.
    void show_job(const std::string &job_id);

  private:
    struct RowAction
    {
        const char *label;
        const char *action; // API action, or "dialog"
    };
    std::vector<const Job *> visible() const;
    std::vector<RowAction> actions_for(const Job &job) const;
    int count(JobView view) const;

    JobView view_ = JobView::active;
    bool on_views_ = true; // the focus is on the view switcher
    int row_ = 0;
    int button_ = 0;
    std::string pending_job_; // show_job() before the queue arrived
    bool dialog_open_ = false;
    bool dialog_cancelled_ = false;
    bool dialog_forget_ = false;
    bool dialog_pending_ = false;
    std::string dialog_error_;
    int dialog_button_ = 0;
    std::string dialog_job_;
    ui::Ease scroll_;
    ui::Ease dialog_in_;
};

class GamePage final : public Screen
{
  public:
    explicit GamePage(Context &context);
    void open(const std::string &game_id);
    const std::string &game_id() const
    {
        return game_id_;
    }
    Intent update(const InputFrame &input, float dt, Feedback &feedback, bool focused) override;
    void draw(Frame &frame, bool focused) const override;
    std::string backdrop_game() const override
    {
        return game_id_;
    }
    std::vector<hui::ui::Hint> hints() const override;

    bool modal() const override { return sheet_ != Sheet::none; }

  private:
    enum class Sheet : std::uint8_t { none, downloads, source, delivery, storage, about };
    enum class Slot : std::uint8_t { primary, favourite, about, source, delivery, drive,
                                     choice, again, related, reading, back, close };
    struct Control
    {
        Slot slot;
        int index;
        ui::Rect rect;
        bool enabled;
        bool fixed = false;
    };
    struct Layout
    {
        std::vector<Control> controls;
        float end = 0.0f;
    };
    struct Offer
    {
        CollectionState collection;
        bool show_download = false;
        const Drive *drive = nullptr;
        SpacePlan plan;
    };
    Offer offer(const Release &release) const;
    bool primary_enabled(const Release &release, const Offer &offer) const;
    Layout compose(hui::gfx::DrawList *list, Sheet sheet, bool focused) const;
    void show_sheet(Sheet sheet, Slot preferred = Slot::primary, int index = 0);
    void back_sheet();
    Intent activate(const Control &control, const Game &game, const Release &release,
                    const Offer &offer, Feedback &feedback);
    const Drive *drive() const;
    std::string delivery_note(const Release &release) const;
    std::string game_id_;
    int option_ = 0;
    int focus_ = 0;
    Sheet sheet_ = Sheet::none;
    std::string drive_id_;
    bool download_again_ = false;
    bool waiting_ = false;
    bool torbox_ = false;
    std::uint64_t action_serial_ = 0;
    std::uint64_t favourite_serial_ = 0;
    std::string error_;
    ui::Ease page_scroll_;
    ui::Ease sheet_scroll_;

};

// App settings (the browser's Sources, Pairing, Storage, Game catalogue,
// Update / reinstall and TV app panels) as one page: sections on the left,
// the chosen one on the right. Updates keep the two programs apart: this TV
// app, and the download service (Orbit, which keeps running after it closes).
class SettingsScreen final : public Screen
{
  public:
    enum class Section : std::uint8_t
    {
        sources,
        storage,
        pairing,
        catalogue,
        updates,
        more,
        count,
    };
    explicit SettingsScreen(Context &context);
    // Opens on a section; content puts the focus inside it at once.
    void open(Section section, bool content = false);
    void close();
    Section section() const
    {
        return section_;
    }
    Intent update(const InputFrame &input, float dt, Feedback &feedback, bool focused) override;
    void draw(Frame &frame, bool focused) const override;
    std::vector<hui::ui::Hint> hints() const override;
    // Previews and tests.
    void confirm_restart()
    {
        confirm_restart_ = true;
    }

  private:
    enum class Kind : std::uint8_t
    {
        source,      // index into the source options
        acknowledge, // the download notice
        save_sources,
        drive, // 0 chooses automatically; then one per drive
        refresh_catalogue,
        tv_check,
        tv_install,
        tv_replace, // install over a copy the owner placed, after confirming
        tv_keep,
        service_check,
        service_install,
        service_restart,
        restart_confirm,
        restart_cancel,
        restart_retry,
        open_browser,
    };
    struct Control
    {
        Kind kind;
        int index = 0;
        int row = 0;
        ui::Rect rect;
        bool enabled = true;
    };
    struct Pane; // lays out a section and, when drawing, draws it
    void build(Pane &pane) const;
    void build_sources(Pane &pane) const;
    void build_storage(Pane &pane) const;
    void build_pairing(Pane &pane) const;
    void build_catalogue(Pane &pane) const;
    void build_updates(Pane &pane) const;
    void build_more(Pane &pane) const;
    std::vector<Control> controls(float *height = nullptr) const;
    void activate(const Control &control, Intent &intent, Feedback &feedback);
    void load_draft();
    bool update_available() const;

    Section section_ = Section::sources;
    bool in_content_ = false;
    Kind focus_kind_ = Kind::source;
    int focus_index_ = 0;
    int last_row_ = 0; // where the focus was, if its control goes away
    float last_x_ = 0.0f;
    // Sources: the choice being made, until it is saved.
    std::vector<std::string> draft_;
    bool draft_acknowledged_ = false;
    bool first_setup_ = false; // saving finishes setting up Orbit
    // Updates: the confirmation steps.
    bool confirm_restart_ = false;
    bool confirm_replace_ = false;
    std::uint64_t setting_serial_ = 0;
    std::string error_;
    Setting error_setting_ = Setting::none;
    float age_ = 0.0f;
    ui::Ease scroll_;
    std::vector<float> focus_amount_; // parallel to controls(), by position
    float nav_focus_[static_cast<int>(Section::count)] = {};
};

// The whole app: top bar, tabs, the game page, status screens and dialogs.
class App
{
  public:
    explicit App(Context &context);
    void update(const InputFrame &input, float dt, Feedback &feedback);
    void draw(Frame &frame) const;
    // Preview and tests: jump straight to a place.
    void show_tab(int tab);
    void open_game(const std::string &game_id);
    void open_settings(SettingsScreen::Section section = SettingsScreen::Section::sources,
                       bool content = false);
    SettingsScreen &settings()
    {
        return settings_;
    }
    BrowseScreen &browse()
    {
        return browse_;
    }
    LibraryScreen &library()
    {
        return library_;
    }
    int current_tab() const
    {
        return tab_;
    }
    DownloadsScreen &downloads()
    {
        return downloads_;
    }

  private:
    enum class Area : std::uint8_t
    {
        top,
        content,
    };
    Screen &screen();
    const Screen &screen() const;
    void draw_top_bar(hui::gfx::DrawList &list) const;
    void draw_game_header(hui::gfx::DrawList &list) const;
    bool status_offline(const Snapshot &state) const;
    void draw_status(Frame &frame) const;
    bool update_badge() const;    // an update for the TV app or the download service
    bool content_blocked() const; // setup or an empty catalogue on Discover/Browse
    void draw_blocked(hui::gfx::DrawList &list) const;
    void handle_intent(const Intent &intent, Feedback &feedback);

    Context &ctx_;
    DiscoverScreen discover_;
    BrowseScreen browse_;
    LibraryScreen library_;
    DownloadsScreen downloads_;
    GamePage game_;
    SettingsScreen settings_;
    int tab_ = tab::discover;
    int last_tab_ = tab::discover; // to tell the Library when it is left
    Area area_ = Area::content;
    int top_index_ = 0; // the tabs, then search and settings
    bool game_open_ = false;
    bool settings_open_ = false;
    bool update_told_ = false; // the update toast shows once per launch
    void show_toast(std::string text, float seconds = 2.8f, bool update = false);
    std::string toast_;
    float toast_time_ = 0.0f;
    float toast_length_ = 2.8f;
    bool toast_update_ = false; // an update notice, not a confirmation
    std::string backdrop_;
    std::string previous_backdrop_;
    std::string settling_; // the art Discover wants, while the focus rests
    float settled_for_ = 0.0f;
    ui::Ease backdrop_in_;
    ui::Ease game_in_;
    std::vector<ui::Ease> top_focus_;
    float status_age_ = 0.0f;
};

} // namespace orbit
