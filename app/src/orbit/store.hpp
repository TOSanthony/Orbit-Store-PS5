// Orbit Store TV app - The app's view of Orbit, kept in sync on a worker thread.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// One worker talks to the backend: it polls status, sources, the catalogue
// (only when its revision changes), the queue and storage, and carries out the
// player's actions in order. Results reach the frame loop through an inbox,
// so screens read a plain snapshot and never wait on the network.

#pragma once

#include "orbit/api.hpp"
#include "orbit/model.hpp"
#include "orbit/starter.hpp"
#include "orbit/thread.hpp"

#include <atomic>
#include <cstdint>
#include <deque>
#include <string>
#include <vector>

namespace orbit
{

enum class Link : std::uint8_t
{
    connecting, // no answer yet
    online,
    offline, // Orbit is not answering
};

// Starting Orbit from the app, when it isn't running (orbit/starter.hpp).
enum class Starting : std::uint8_t
{
    idle,
    starting,   // sending Orbit to the ELF loader
    waiting,    // sent; waiting for Orbit to answer
    no_loader,  // no ELF loader on the console
    no_payload, // no copy of Orbit to send
    failed,     // sending broke off, or Orbit never answered
};

// Restarting the download service from Settings: Orbit stops (pausing its
// downloads), then the app starts the saved copy through the ELF loader.
enum class Restart : std::uint8_t
{
    idle,
    stopping, // Orbit was asked to stop and is still answering
    starting, // Orbit stopped; the app is starting it and waiting for an answer
    done,     // Orbit answers again (restart_detail: its version)
    manual,   // Orbit stopped and the app can't start it: use a payload manager
    failed,   // Orbit didn't stop, or didn't come back after starting
};

struct Snapshot
{
    Link link = Link::connecting;
    std::string link_error; // why the last poll failed
    double offline_since = 0.0;
    Starting starting = Starting::idle;
    std::string start_detail; // what the last start attempt did

    bool have_system = false;
    System system;
    bool have_sources = false;
    Sources sources;
    Debrid debrid;
    bool have_catalog = false;
    std::vector<Game> games;
    std::uint64_t catalog_serial = 0; // changes when games are replaced

    bool paired = false; // the console session is open
    std::string token;
    bool have_queue = false;
    std::vector<Job> jobs;
    std::vector<Drive> drives;

    bool have_favourites = false;
    std::vector<std::string> favourites; // game IDs, shared with paired clients
    bool have_library = false;
    LibrarySnapshot library;
    std::string library_error; // why the last Library read failed
    bool have_library_storage = false;
    LibraryStorage library_storage;

    // Settings: the console session's pairing code and address, the catalogue,
    // and updates for the download service (Orbit) and this app.
    std::string pair_code;
    std::string address;
    bool have_catalogue_status = false;
    CatalogueStatus catalogue;
    bool have_service_update = false;
    ServiceUpdate service;
    bool have_tv_app = false;
    TvAppStatus tv_app;
    Restart restart = Restart::idle;
    std::string restart_detail;

    std::uint64_t serial = 0; // changes with any update

    bool setup_needed() const
    {
        return have_sources && (!sources.acknowledged || sources.enabled.empty());
    }
    const Game *game(std::string_view id) const;
    bool favourite(std::string_view game_id) const;
    // The Library's games, once a read has succeeded.
    const std::vector<LibraryGame> &library_games() const;
};

// What a settings command changed, so its screen can show the outcome there.
enum class Setting : std::uint8_t
{
    none,
    sources,
    storage,
    catalogue,
    service_check,
    service_install,
    service_restart,
    tv_check,
    tv_install,
};

struct SettingResult
{
    bool busy = false;
    Setting setting = Setting::none; // the last one sent
    std::string error;
    std::uint64_t serial = 0; // changes when a command finishes
};

// The outcome of the last player action (Download to PS5, Pause, ...).
struct ActionState
{
    bool busy = false;
    std::string error;
    std::string created_job;  // the job a download created
    std::uint64_t serial = 0; // changes when an action finishes
};

class Store
{
  public:
    // The transport (and starter, if any) must outlive the store. start()
    // launches the worker; tests may instead call poll_once() and
    // run_next_action() directly. Without a starter the app only waits for
    // Orbit; with one it starts Orbit once when nothing answers on its port,
    // and again whenever the player asks.
    explicit Store(http::Transport &transport, starter::Starter *starter = nullptr);
    ~Store();
    Store(const Store &) = delete;
    Store &operator=(const Store &) = delete;

    void start();
    void stop();

    // Frame thread: applies whatever the worker finished.
    void tick();
    const Snapshot &state() const
    {
        return state_;
    }
    const ActionState &action() const
    {
        return action_;
    }

    // Poll every second while a screen shows live progress, else every three.
    void set_fast(bool fast)
    {
        fast_.store(fast);
    }
    void refresh_now();
    bool can_start() const
    {
        return starter_ != nullptr;
    }
    void start_orbit();

    void download(const std::string &release_id, const std::string &storage_id,
                  bool torbox = false);
    void job_action(const std::string &job_id, const std::string &action, bool delete_partial);

    // Favourites and the Library each report their own outcome, so an error
    // shows where it happened.
    const ActionState &favourite_action() const
    {
        return favourite_;
    }
    const ActionState &library_action() const
    {
        return library_;
    }
    void set_favourite(const std::string &game_id, bool favourite);
    void refresh_library();
    // A request_id is filled in here.
    void run_library_action(api::Client::LibraryRequest request);
    // Library storage (drives and copy/move destinations) is read only while
    // a screen needs it; refresh asks for a read sooner.
    void want_library_storage(bool wanted, bool refresh = false);

    // ---- settings: each command reports to setting_result() ----
    const SettingResult &setting_result() const
    {
        return setting_;
    }
    void save_sources(std::vector<std::string> enabled, int notice_version);
    // Empty lets the app choose (the first external drive).
    void set_preferred_storage(const std::string &storage_id);
    void refresh_catalogue();
    void check_service_update();
    void install_service_update(const std::string &version, const std::string &checksum);
    // Stops Orbit, which pauses its downloads, then starts it again.
    void restart_service();
    void check_tv_app();
    void install_tv_app(const std::string &version, const std::string &checksum, bool replace);
    // While Settings is open the catalogue and update statuses are read on
    // every poll; otherwise about once a minute, for the update badge.
    void want_settings(bool wanted);
    // Reads the console session again (a fresh pairing code after a restart).
    void refresh_session();
    // The frame has shown a finished restart.
    void dismiss_restart()
    {
        if (state_.restart == Restart::done)
            state_.restart = Restart::idle;
    }

    // Tests: how long a restart waits after Orbit's port closes before
    // starting it again (four seconds).
    void set_restart_grace(double seconds)
    {
        restart_grace_ = seconds;
    }

    // ---- worker steps (public for tests) ----
    void poll_once();
    bool run_next_action();

  private:
    struct Command
    {
        enum class Kind : std::uint8_t
        {
            download,
            job_action,
            favourite,
            library_refresh,
            library_action,
            setting,
        } kind = Kind::download;
        Setting setting = Setting::none;
        std::string a; // release, job or game ID
        std::string b; // storage ID or action name
        bool flag = false;
        api::Client::LibraryRequest library;
    };
    void run_aside(Command &command);
    void run_setting(Command &command);
    void send_setting(Setting setting, std::string a = {}, std::string b = {}, bool flag = false);
    void poll_settings();
    void post_restart(Restart restart, std::string detail);
    void continue_restart(bool unreachable);

    void worker();
    void post_queue(std::vector<Job> jobs, std::vector<Drive> drives, bool have_drives);
    void post_failure(const std::string &error, bool unreachable);
    void post_start(Starting starting, std::string detail);
    void start_if_needed(bool refused);

    api::Client client_;
    starter::Starter *starter_;
    Inbox inbox_;
    Snapshot state_;
    ActionState action_;
    ActionState favourite_;
    ActionState library_;
    SettingResult setting_;
    int pending_actions_ = 0; // frame thread: queued actions without a result yet
    std::uint64_t request_count_ = 0;

    // Shared with the worker.
    Mutex mutex_;
    Signal signal_;
    std::deque<Command> commands_;
    bool poll_requested_ = true;
    bool start_requested_ = false;
    bool stopping_ = false;
    std::atomic<bool> fast_{false};
    std::atomic<bool> want_storage_{false};
    std::atomic<bool> storage_refresh_{false};
    std::atomic<bool> want_settings_{false};
    std::atomic<bool> session_refresh_{false};
    Thread thread_;

    // Worker-only.
    std::string catalog_key_;
    std::string token_;
    double next_session_ = 0.0;
    bool auto_start_used_ = false;
    bool start_waiting_ = false;
    double start_deadline_ = 0.0;
    Starting start_state_ = Starting::idle;
    bool automatic_checks_sent_ = false;
    int settings_countdown_ = 0; // polls until the statuses are read again
    bool settings_busy_ = false; // a check, install or refresh is running
    Link worker_link_ = Link::connecting;
    Restart restart_ = Restart::idle;
    double restart_grace_ = 4.0;
    double restart_since_ = 0.0;   // when the current restart step began
    double restart_send_at_ = 0.0; // when to send Orbit to the loader
    double restart_deadline_ = 0.0;
    int restart_attempts_ = 0;
    bool restart_sent_ = false;
};

} // namespace orbit
