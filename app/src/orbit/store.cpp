// Orbit Store TV app - The app's view of Orbit, kept in sync on a worker thread.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "orbit/store.hpp"
#include "orbit/i18n.hpp"

#include <cstdio>
#include <cstdlib>
#include <utility>

namespace orbit
{

namespace
{

// How long Orbit may take to answer after the loader has it: it loads its
// catalogue and saves its runtime copy before it serves.
constexpr double kStartWait = 30.0;
// Restarting: Orbit closes its port first, then stops its workers and
// releases its instance lock; a copy started before that exits at once. So
// the app waits a little after the port closes (restart_grace_), and sends
// Orbit again once if it doesn't answer.
constexpr double kRestartAnswer = 15.0;
constexpr double kRestartStop = 30.0; // Orbit should have stopped by then
constexpr int kRestartAttempts = 2;
// Without Settings open, statuses are read every twentieth poll (a minute).
constexpr int kSettingsEvery = 20;

// A catalogue fetch is needed when anything that changes the list changes:
// the server, its catalogue revision, or the source choices.
std::string catalog_key(const System &system, const Sources &sources)
{
    std::string key = system.version;
    key += '|';
    key += std::to_string(system.catalogue_revision);
    key += '|';
    key += sources.acknowledged ? '1' : '0';
    key += '|';
    key += std::to_string(sources.notice_version);
    for (const std::string &id : sources.enabled)
    {
        key += '|';
        key += id;
    }
    for (const SourceOption &option : sources.options)
    {
        key += '|';
        key += option.id;
        key += ':';
        key += std::to_string(option.release_count);
    }
    return key;
}

} // namespace

const Game *Snapshot::game(std::string_view id) const
{
    for (const Game &game : games)
    {
        if (game.id == id)
            return &game;
    }
    return nullptr;
}

bool Snapshot::favourite(std::string_view game_id) const
{
    return contains(favourites, game_id);
}

const std::vector<LibraryGame> &Snapshot::library_games() const
{
    static const std::vector<LibraryGame> kNone;
    return have_library && library.ready() ? library.games : kNone;
}

Store::Store(http::Transport &transport, starter::Starter *starter)
    : client_(transport), starter_(starter)
{
}

Store::~Store()
{
    stop();
}

void Store::start()
{
    thread_.start([this] { worker(); });
}

void Store::stop()
{
    {
        Lock lock(mutex_);
        stopping_ = true;
    }
    signal_.notify();
    thread_.join();
}

void Store::tick()
{
    inbox_.drain();
}

void Store::refresh_now()
{
    {
        Lock lock(mutex_);
        poll_requested_ = true;
    }
    signal_.notify();
}

void Store::start_orbit()
{
    {
        Lock lock(mutex_);
        start_requested_ = true;
        poll_requested_ = true;
    }
    signal_.notify();
}

void Store::download(const std::string &release_id, const std::string &storage_id, bool torbox)
{
    ++pending_actions_;
    action_.busy = true;
    action_.error.clear();
    action_.created_job.clear();
    {
        Lock lock(mutex_);
        Command command;
        command.kind = Command::Kind::download;
        command.a = release_id;
        command.b = storage_id;
        command.flag = torbox;
        commands_.push_back(std::move(command));
    }
    signal_.notify();
}

void Store::job_action(const std::string &job_id, const std::string &action, bool delete_partial)
{
    ++pending_actions_;
    action_.busy = true;
    action_.error.clear();
    {
        Lock lock(mutex_);
        Command command;
        command.kind = Command::Kind::job_action;
        command.a = job_id;
        command.b = action;
        command.flag = delete_partial;
        commands_.push_back(std::move(command));
    }
    signal_.notify();
}

void Store::set_favourite(const std::string &game_id, bool favourite)
{
    favourite_.busy = true;
    favourite_.error.clear();
    {
        Lock lock(mutex_);
        Command command;
        command.kind = Command::Kind::favourite;
        command.a = game_id;
        command.flag = favourite;
        commands_.push_back(std::move(command));
    }
    signal_.notify();
}

void Store::refresh_library()
{
    {
        Lock lock(mutex_);
        Command command;
        command.kind = Command::Kind::library_refresh;
        commands_.push_back(std::move(command));
    }
    signal_.notify();
}

void Store::run_library_action(api::Client::LibraryRequest request)
{
    library_.busy = true;
    library_.error.clear();
    // Unique per action: the backend refuses to carry out a request twice.
    char id[48];
    std::snprintf(id, sizeof(id), "tv-%llx-%llx",
                  static_cast<unsigned long long>(unix_seconds() * 1000.0),
                  static_cast<unsigned long long>(++request_count_));
    request.request_id = id;
    {
        Lock lock(mutex_);
        Command command;
        command.kind = Command::Kind::library_action;
        command.library = std::move(request);
        commands_.push_back(std::move(command));
    }
    signal_.notify();
}

void Store::send_setting(Setting setting, std::string a, std::string b, bool flag)
{
    setting_.busy = true;
    setting_.setting = setting;
    setting_.error.clear();
    {
        Lock lock(mutex_);
        Command command;
        command.kind = Command::Kind::setting;
        command.setting = setting;
        command.a = std::move(a);
        command.b = std::move(b);
        command.flag = flag;
        commands_.push_back(std::move(command));
    }
    signal_.notify();
}

void Store::save_sources(std::vector<std::string> enabled, int notice_version)
{
    std::string list;
    for (const std::string &id : enabled)
    {
        if (!list.empty())
            list += '\n';
        list += id;
    }
    send_setting(Setting::sources, std::move(list), std::to_string(notice_version));
}

void Store::set_preferred_storage(const std::string &storage_id)
{
    send_setting(Setting::storage, storage_id);
}

void Store::refresh_catalogue()
{
    send_setting(Setting::catalogue);
}

void Store::check_service_update()
{
    send_setting(Setting::service_check);
}

void Store::install_service_update(const std::string &version, const std::string &checksum)
{
    send_setting(Setting::service_install, version, checksum);
}

void Store::restart_service()
{
    send_setting(Setting::service_restart);
}

void Store::check_tv_app()
{
    send_setting(Setting::tv_check);
}

void Store::install_tv_app(const std::string &version, const std::string &checksum, bool replace)
{
    send_setting(Setting::tv_install, version, checksum, replace);
}

void Store::want_settings(bool wanted)
{
    const bool was = want_settings_.exchange(wanted);
    if (wanted && !was)
        refresh_now();
}

void Store::refresh_session()
{
    session_refresh_.store(true);
    refresh_now();
}

void Store::want_library_storage(bool wanted, bool refresh)
{
    const bool was = want_storage_.exchange(wanted);
    if (refresh)
        storage_refresh_.store(true);
    if (wanted && (!was || refresh))
        refresh_now();
}

void Store::worker()
{
    double next_poll = 0.0;
    for (;;)
    {
        bool have_command = false;
        bool poll = false;
        {
            Lock lock(mutex_);
            for (;;)
            {
                if (stopping_)
                    return;
                have_command = !commands_.empty();
                poll = poll_requested_ || now_seconds() >= next_poll;
                if (have_command || poll)
                    break;
                signal_.wait(mutex_, next_poll - now_seconds());
            }
            poll_requested_ = false;
        }
        if (have_command)
        {
            run_next_action();
            continue; // the action already refreshed the queue
        }
        poll_once();
        const bool hurry = start_waiting_ || restart_ == Restart::stopping ||
                           restart_ == Restart::starting ||
                           (want_settings_.load() && settings_busy_);
        next_poll = now_seconds() + (hurry ? 0.5 : fast_.load() ? 1.0 : 3.0);
    }
}

void Store::post_failure(const std::string &error, bool unreachable)
{
    inbox_.post(
        [this, error, unreachable]
        {
            state_.link_error = error;
            if (unreachable)
            {
                if (state_.link != Link::offline)
                    state_.offline_since = now_seconds();
                state_.link = Link::offline;
            }
            ++state_.serial;
        });
}

void Store::post_start(Starting starting, std::string detail)
{
    start_state_ = starting;
    inbox_.post(
        [this, starting, detail = std::move(detail)]() mutable
        {
            state_.starting = starting;
            state_.start_detail = std::move(detail);
            ++state_.serial;
        });
}

void Store::post_restart(Restart restart, std::string detail)
{
    inbox_.post(
        [this, restart, detail = std::move(detail)]() mutable
        {
            state_.restart = restart;
            state_.restart_detail = std::move(detail);
            ++state_.serial;
        });
}

// Runs on the worker while a restart is under way and Orbit isn't answering.
void Store::continue_restart(bool unreachable)
{
    const double now = now_seconds();
    bool retry = false;
    {
        Lock lock(mutex_);
        retry = start_requested_;
        start_requested_ = false;
    }
    if (restart_ == Restart::stopping)
    {
        if (!unreachable)
        {
            if (now - restart_since_ > kRestartStop)
            {
                restart_ = Restart::idle;
                post_restart(Restart::failed, tr("Orbit didn\xE2\x80\x99t stop. Try again."));
            }
            return;
        }
        // Orbit's port has closed; give it a moment to release its lock.
        restart_ = Restart::starting;
        restart_since_ = now;
        restart_send_at_ = now + restart_grace_;
        restart_attempts_ = 0;
        restart_sent_ = false;
        post_restart(Restart::starting, tr("Orbit stopped."));
        return;
    }
    if ((restart_ == Restart::manual || restart_ == Restart::failed) && retry)
    {
        // Try again, from the start.
        restart_ = Restart::starting;
        restart_send_at_ = now;
        restart_attempts_ = 0;
        restart_sent_ = false;
        post_restart(Restart::starting, {});
    }
    if (restart_ != Restart::starting)
        return;
    if (starter_ == nullptr)
    {
        restart_ = Restart::manual;
        post_restart(Restart::manual, {});
        return;
    }
    if (restart_sent_ && now < restart_deadline_)
        return;
    if (restart_sent_ && restart_attempts_ >= kRestartAttempts)
    {
        restart_ = Restart::failed;
        post_restart(Restart::failed, tr("Orbit didn\xE2\x80\x99t answer after it was started."));
        return;
    }
    if (now < restart_send_at_)
        return;
    ++restart_attempts_;
    starter::Result result = starter_->start();
    switch (result.outcome)
    {
    case starter::Outcome::sent:
        restart_sent_ = true;
        restart_deadline_ = now_seconds() + kRestartAnswer;
        post_restart(Restart::starting, std::move(result.detail));
        break;
    case starter::Outcome::no_loader:
    case starter::Outcome::no_payload:
        restart_ = Restart::manual;
        post_restart(Restart::manual, std::move(result.detail));
        break;
    case starter::Outcome::failed:
        if (restart_attempts_ < kRestartAttempts)
        {
            restart_send_at_ = now_seconds() + restart_grace_;
            post_restart(Restart::starting, std::move(result.detail));
        }
        else
        {
            restart_ = Restart::failed;
            post_restart(Restart::failed, std::move(result.detail));
        }
        break;
    }
}

// Runs on the worker after a poll found Orbit unreachable. Sends Orbit to the
// loader once per launch when nothing listens on its port (a refused
// connection, not a slow one), and whenever the player asks; then gives Orbit
// kStartWait seconds to answer.
void Store::start_if_needed(bool refused)
{
    bool requested = false;
    {
        Lock lock(mutex_);
        requested = start_requested_;
        start_requested_ = false;
    }
    if (starter_ == nullptr)
        return;
    if (start_waiting_)
    {
        if (now_seconds() < start_deadline_)
            return;
        start_waiting_ = false;
        post_start(Starting::failed, tr("Orbit didn\xE2\x80\x99t answer after it was started."));
        return;
    }
    const bool automatic = refused && !auto_start_used_;
    if (!requested && !automatic)
        return;
    auto_start_used_ = true;
    post_start(Starting::starting, {});
    starter::Result result = starter_->start();
    switch (result.outcome)
    {
    case starter::Outcome::sent:
        start_waiting_ = true;
        start_deadline_ = now_seconds() + kStartWait;
        post_start(Starting::waiting, std::move(result.detail));
        break;
    case starter::Outcome::no_loader:
        post_start(Starting::no_loader, std::move(result.detail));
        break;
    case starter::Outcome::no_payload:
        post_start(Starting::no_payload, std::move(result.detail));
        break;
    case starter::Outcome::failed:
        post_start(Starting::failed, std::move(result.detail));
        break;
    }
}

void Store::post_queue(std::vector<Job> jobs, std::vector<Drive> drives, bool have_drives)
{
    inbox_.post(
        [this, jobs = std::move(jobs), drives = std::move(drives), have_drives]() mutable
        {
            state_.jobs = std::move(jobs);
            if (have_drives)
                state_.drives = std::move(drives);
            state_.have_queue = true;
            ++state_.serial;
        });
}

void Store::poll_once()
{
    api::Result<System> system = client_.system();
    if (!system.ok())
    {
        post_failure(system.error, system.unreachable());
        if (system.unreachable())
            worker_link_ = Link::offline;
        if (restart_ != Restart::idle)
            continue_restart(system.unreachable());
        else if (system.unreachable())
            start_if_needed(system.error == "connection refused");
        return;
    }
    if (restart_ == Restart::stopping)
    {
        // Orbit answers for a moment after it agrees to stop.
        if (now_seconds() - restart_since_ > kRestartStop)
        {
            restart_ = Restart::idle;
            post_restart(Restart::failed, tr("Orbit is still running. Try restarting it again."));
        }
    }
    else if (restart_ != Restart::idle)
    {
        restart_ = Restart::idle;
        post_restart(Restart::done, system.value.version);
        session_refresh_.store(true); // a new pairing code
    }
    if (worker_link_ == Link::offline)
        session_refresh_.store(true); // Orbit may have restarted: a new pairing code
    worker_link_ = Link::online;
    {
        Lock lock(mutex_);
        start_requested_ = false; // Orbit is answering: nothing to start
    }
    if (start_waiting_ || start_state_ != Starting::idle)
    {
        start_waiting_ = false;
        post_start(Starting::idle, {});
    }
    api::Result<Sources> sources = client_.sources();
    if (!sources.ok())
    {
        post_failure(sources.error, sources.unreachable());
        return;
    }

    const std::string key = catalog_key(system.value, sources.value);
    bool have_games = false;
    std::vector<Game> games;
    if (key != catalog_key_)
    {
        api::Result<std::vector<Release>> catalog = client_.catalog();
        if (catalog.ok())
        {
            games = group_games(catalog.value);
            have_games = true;
            catalog_key_ = key;
        }
    }

    // The app runs on the console, so it opens the loopback-only session
    // rather than pairing with a code. It is asked for again only if the
    // backend stops recognising the token.
    const bool need_token = !system.value.paired && now_seconds() >= next_session_;
    if (system.value.local_session && (need_token || session_refresh_.load()))
    {
        session_refresh_.store(false);
        api::Result<Session> session = client_.session();
        if (need_token)
            next_session_ = now_seconds() + 10.0; // a refusal is retried, but not every poll
        if (session.ok())
        {
            token_ = session.value.token;
            client_.set_token(token_);
            system.value.paired = true;
            inbox_.post(
                [this, code = session.value.pair_code, address = session.value.address]() mutable
                {
                    state_.pair_code = std::move(code);
                    state_.address = std::move(address);
                    ++state_.serial;
                });
        }
    }
    if (system.value.paired)
        next_session_ = 0.0; // a later loss of the token is repaired at once

    inbox_.post(
        [this, system = std::move(system.value), sources = std::move(sources.value),
         games = std::move(games), have_games, token = token_]() mutable
        {
            if (state_.link != Link::online)
                state_.link_error.clear();
            state_.link = Link::online;
            state_.system = std::move(system);
            state_.have_system = true;
            state_.sources = std::move(sources);
            state_.have_sources = true;
            if (have_games)
            {
                state_.games = std::move(games);
                state_.have_catalog = true;
                ++state_.catalog_serial;
            }
            state_.paired = state_.system.paired;
            state_.token = token;
            ++state_.serial;
        });

    if (!system.value.paired && token_.empty())
        return;
    api::Result<std::vector<Job>> jobs = client_.downloads();
    if (!jobs.ok())
    {
        if (jobs.status == 401)
        {
            token_.clear();
            client_.set_token({});
        }
        post_failure(jobs.error, jobs.unreachable());
        return;
    }
    api::Result<std::vector<Drive>> drives = client_.storage();
    post_queue(std::move(jobs.value), std::move(drives.value), drives.ok());
    // Optional on older backends. A failed read clears availability so a
    // disconnected account cannot remain an apparently usable choice.
    api::Result<Debrid> debrid = client_.debrid();
    inbox_.post(
        [this, debrid = std::move(debrid)]() mutable
        {
            state_.debrid = debrid.ok() ? std::move(debrid.value) : Debrid{};
            ++state_.serial;
        });

    // Favourites and the Library (cheap reads: the backend answers from its
    // cache and limits ShadowMount reads itself).
    api::Result<std::vector<std::string>> favourites = client_.favourites();
    api::Result<LibrarySnapshot> library = client_.library(false);
    api::Result<LibraryStorage> storage;
    const bool want_storage = want_storage_.load();
    if (want_storage)
        storage = client_.library_storage(storage_refresh_.exchange(false));
    inbox_.post(
        [this, favourites = std::move(favourites), library = std::move(library),
         storage = std::move(storage), want_storage]() mutable
        {
            if (favourites.ok())
            {
                state_.favourites = std::move(favourites.value);
                state_.have_favourites = true;
            }
            if (library.ok())
            {
                state_.library = std::move(library.value);
                state_.have_library = true;
                state_.library_error.clear();
            }
            else if (!library.unreachable())
            {
                state_.library_error = library.error;
            }
            if (want_storage && storage.ok())
            {
                state_.library_storage = std::move(storage.value);
                state_.have_library_storage = true;
            }
            ++state_.serial;
        });

    // Opening the app looks for updates once, as the browser version does;
    // Orbit shares a six-hour cooldown between every client.
    if (!automatic_checks_sent_)
    {
        automatic_checks_sent_ = true;
        api::Client::UpdateRequest check;
        check.action = "check";
        check.automatic = true;
        api::Result<ServiceUpdate> service = client_.service_action(check);
        api::Result<TvAppStatus> tv = client_.tv_app_action(check);
        (void)service;
        (void)tv;
        settings_countdown_ = 0; // read the outcome at once
    }
    if (want_settings_.load() || settings_busy_ || --settings_countdown_ <= 0)
    {
        settings_countdown_ = kSettingsEvery;
        poll_settings();
    }
}

// The catalogue and update statuses (Settings, and the update badge).
void Store::poll_settings()
{
    api::Result<CatalogueStatus> catalogue = client_.catalogue_status();
    api::Result<ServiceUpdate> service = client_.service_update();
    api::Result<TvAppStatus> tv = client_.tv_app();
    settings_busy_ = (catalogue.ok() && catalogue.value.busy) ||
                     (service.ok() && service.value.busy) || (tv.ok() && tv.value.busy);
    inbox_.post(
        [this, catalogue = std::move(catalogue), service = std::move(service),
         tv = std::move(tv)]() mutable
        {
            if (catalogue.ok())
            {
                state_.catalogue = std::move(catalogue.value);
                state_.have_catalogue_status = true;
            }
            if (service.ok())
            {
                state_.service = std::move(service.value);
                state_.have_service_update = true;
            }
            if (tv.ok())
            {
                state_.tv_app = std::move(tv.value);
                state_.have_tv_app = true;
            }
            ++state_.serial;
        });
}

// Settings commands: each reports to setting_ and refreshes what it changed.
void Store::run_setting(Command &command)
{
    std::string error;
    switch (command.setting)
    {
    case Setting::sources:
    {
        std::vector<std::string> enabled;
        std::size_t start = 0;
        while (start < command.a.size())
        {
            std::size_t end = command.a.find('\n', start);
            if (end == std::string::npos)
                end = command.a.size();
            enabled.push_back(command.a.substr(start, end - start));
            start = end + 1;
        }
        api::Result<Sources> result = client_.save_sources(enabled, std::atoi(command.b.c_str()));
        error = result.error;
        if (result.ok())
        {
            inbox_.post(
                [this, sources = std::move(result.value)]() mutable
                {
                    state_.sources = std::move(sources);
                    state_.have_sources = true;
                    ++state_.serial;
                });
        }
        break;
    }
    case Setting::storage:
    {
        api::Result<std::string> result = client_.set_preferred_storage(command.a);
        error = result.status == 404 ? tr("Update the download service to choose a default drive here.")
                                     : result.error;
        if (result.ok())
        {
            inbox_.post(
                [this, id = std::move(result.value)]() mutable
                {
                    state_.system.preferred_storage = std::move(id);
                    ++state_.serial;
                });
        }
        break;
    }
    case Setting::catalogue:
    {
        api::Result<CatalogueStatus> result = client_.refresh_catalogue();
        error = result.error;
        break;
    }
    case Setting::service_check:
    case Setting::service_install:
    case Setting::service_restart:
    {
        api::Client::UpdateRequest request;
        request.action = command.setting == Setting::service_check     ? "check"
                         : command.setting == Setting::service_install ? "install"
                                                                       : "stop";
        request.version = command.a;
        request.checksum = command.b;
        request.confirmed = command.setting == Setting::service_restart;
        api::Result<ServiceUpdate> result = client_.service_action(request);
        error = result.error;
        if (result.ok() && command.setting == Setting::service_restart)
        {
            restart_ = Restart::stopping;
            restart_since_ = now_seconds();
            post_restart(Restart::stopping, {});
        }
        break;
    }
    case Setting::tv_check:
    case Setting::tv_install:
    {
        api::Client::UpdateRequest request;
        request.action = command.setting == Setting::tv_check ? "check" : "install";
        request.version = command.a;
        request.checksum = command.b;
        request.replace = command.flag;
        api::Result<TvAppStatus> result = client_.tv_app_action(request);
        error = result.error;
        break;
    }
    case Setting::none:
        break;
    }
    if (command.setting != Setting::service_restart || !error.empty())
        poll_settings();
    {
        Lock lock(mutex_);
        poll_requested_ = true;
    }
    inbox_.post(
        [this, error]
        {
            setting_.busy = false;
            setting_.error = error;
            ++setting_.serial;
        });
}

// Favourites and Library commands: each reports to its own ActionState.
void Store::run_aside(Command &command)
{
    std::string error;
    if (command.kind == Command::Kind::favourite)
    {
        api::Result<std::vector<std::string>> result =
            client_.set_favourite(command.a, command.flag);
        if (!result.ok())
            error = result.error;
        inbox_.post(
            [this, error, result = std::move(result)]() mutable
            {
                if (result.ok())
                {
                    state_.favourites = std::move(result.value);
                    state_.have_favourites = true;
                    ++state_.serial;
                }
                favourite_.busy = false;
                favourite_.error = error;
                ++favourite_.serial;
            });
        return;
    }
    api::Result<LibrarySnapshot> result = command.kind == Command::Kind::library_refresh
                                              ? client_.library(true)
                                              : client_.library_action(command.library);
    if (!result.ok())
        error = result.error;
    const bool report = command.kind == Command::Kind::library_action;
    inbox_.post(
        [this, error, report, result = std::move(result)]() mutable
        {
            if (result.ok())
            {
                state_.library = std::move(result.value);
                state_.have_library = true;
                ++state_.serial;
            }
            if (report)
            {
                library_.busy = false;
                library_.error = error;
                ++library_.serial;
            }
        });
}

bool Store::run_next_action()
{
    Command command;
    {
        Lock lock(mutex_);
        if (commands_.empty())
            return false;
        command = std::move(commands_.front());
        commands_.pop_front();
    }
    std::string error;
    std::string created;
    std::vector<Job> jobs;
    bool have_jobs = false;
    if (command.kind == Command::Kind::favourite ||
        command.kind == Command::Kind::library_refresh ||
        command.kind == Command::Kind::library_action)
    {
        run_aside(command);
        return true;
    }
    if (command.kind == Command::Kind::setting)
    {
        run_setting(command);
        return true;
    }
    if (command.kind == Command::Kind::download)
    {
        api::Result<std::string> result =
            client_.create_download(command.a, command.b, command.flag);
        if (result.ok())
            created = result.value;
        else
            error = result.error;
    }
    else
    {
        api::Result<std::vector<Job>> result =
            client_.job_action(command.a, command.b, command.flag);
        if (result.ok())
        {
            jobs = std::move(result.value);
            have_jobs = true;
        }
        else
        {
            error = result.error;
        }
    }
    if (!have_jobs)
    {
        api::Result<std::vector<Job>> queue = client_.downloads();
        if (queue.ok())
        {
            jobs = std::move(queue.value);
            have_jobs = true;
        }
    }
    api::Result<std::vector<Drive>> drives = client_.storage();
    if (have_jobs)
        post_queue(std::move(jobs), std::move(drives.value), drives.ok());
    inbox_.post(
        [this, error, created]
        {
            if (pending_actions_ > 0)
                --pending_actions_;
            action_.busy = pending_actions_ > 0;
            action_.error = error;
            action_.created_job = created;
            ++action_.serial;
        });
    return true;
}

} // namespace orbit
