// Orbit Store TV app - The Orbit API's data, and the storefront rules built on it.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "orbit/model.hpp"

#include "orbit/i18n.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>

namespace orbit
{

namespace
{

char lower(char c)
{
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}

bool contains_folded(std::string_view haystack, std::string_view needle)
{
    if (needle.empty())
        return true;
    if (needle.size() > haystack.size())
        return false;
    for (std::size_t i = 0; i + needle.size() <= haystack.size(); ++i)
    {
        std::size_t j = 0;
        while (j < needle.size() && lower(haystack[i + j]) == lower(needle[j]))
            ++j;
        if (j == needle.size())
            return true;
    }
    return false;
}

int compare_folded(std::string_view a, std::string_view b)
{
    const std::size_t n = std::min(a.size(), b.size());
    for (std::size_t i = 0; i < n; ++i)
    {
        const char x = lower(a[i]);
        const char y = lower(b[i]);
        if (x != y)
            return static_cast<unsigned char>(x) < static_cast<unsigned char>(y) ? -1 : 1;
    }
    if (a.size() == b.size())
        return 0;
    return a.size() < b.size() ? -1 : 1;
}

std::string_view trim(std::string_view value)
{
    while (!value.empty() && value.front() == ' ')
        value.remove_prefix(1);
    while (!value.empty() && value.back() == ' ')
        value.remove_suffix(1);
    return value;
}

// ISO catalogue timestamps include UTC or an explicit offset. Compare instants,
// not the spelling of the timezone; avoid depending on the console's local zone.
double addition_time(const std::string &date)
{
    int year = 0, month = 0, day = 0, hour = 0, minute = 0, end = 0;
    double second = 0;
    if (std::sscanf(date.c_str(), "%d-%d-%dT%d:%d:%lf%n", &year, &month, &day,
                    &hour, &minute, &second, &end) != 6 || end <= 0 ||
        year < 1900 || year > 9999 || month < 1 || month > 12 ||
        hour < 0 || hour > 23 || minute < 0 || minute > 59 || !(second >= 0 && second < 60))
        return 0;
    const bool leap = year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
    const int months[] = {31, 28 + (leap ? 1 : 0), 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (day < 1 || day > months[month - 1])
        return 0;
    const char *zone = date.c_str() + end;
    int offset = 0;
    if (std::string_view(zone) != "Z")
    {
        int hours = 0, minutes = 0, consumed = 0;
        if ((*zone != '+' && *zone != '-') ||
            std::sscanf(zone + 1, "%d:%d%n", &hours, &minutes, &consumed) != 2 ||
            zone[consumed + 1] != '\0' || hours < 0 || hours > 23 || minutes < 0 || minutes > 59)
            return 0;
        offset = (hours * 60 + minutes) * (*zone == '-' ? -1 : 1);
    }
    const long long y = year - 1;
    long long days = 365 * y + y / 4 - y / 100 + y / 400 + day - 1;
    for (int m = 0; m < month - 1; ++m)
        days += months[m];
    return static_cast<double>(days) * 86400 + hour * 3600 + minute * 60 + second - offset * 60;
}

int status_rank(std::string_view status)
{
    if (status == "downloading")
        return 0;
    if (status == "verifying")
        return 1;
    if (status == "queued")
        return 2;
    if (status == "retrying")
        return 3;
    if (status == "paused")
        return 4;
    if (status == "error")
        return 5;
    if (status == "complete")
        return 6;
    return 7;
}

} // namespace

System parse_system(const json::Value &value)
{
    System system;
    system.name = value.text("name");
    system.version = value.text("version");
    system.platform = value.text("platform");
    system.paired = value.flag("paired");
    system.local_session = value.flag("localSessionAvailable");
    system.state_healthy = value.flag("stateHealthy", true);
    system.catalogue_revision = value["catalogueRevision"].as_int();
    system.preferred_storage = value.text("preferredStorage");
    return system;
}

Sources parse_sources(const json::Value &value)
{
    Sources sources;
    for (const json::Value &id : value["enabled"].items())
    {
        if (id.is_string())
            sources.enabled.push_back(id.as_string());
    }
    sources.acknowledged = value.flag("acknowledged");
    sources.notice_version = static_cast<int>(value["noticeVersion"].as_int());
    for (const json::Value &item : value["options"].items())
    {
        SourceOption option;
        option.id = item.text("id");
        option.label = item.text("label");
        option.release_count = static_cast<int>(item["releaseCount"].as_int());
        option.download_ready = item.flag("downloadReady");
        sources.options.push_back(std::move(option));
    }
    return sources;
}

Debrid parse_debrid(const json::Value &value)
{
    Debrid status;
    if (value.text("provider") != "torbox")
        return status;
    status.available = value.flag("available");
    status.connected = value.flag("connected");
    status.busy = value.flag("busy");
    for (const json::Value &item : value["hosts"].items())
    {
        Debrid::Host host;
        host.source_id = item.text("sourceId");
        host.max_file_size = item.number("maxFileSize");
        host.available =
            item.flag("available") && item["maxFileSize"].is_number() && host.max_file_size >= 0;
        status.hosts.push_back(std::move(host));
    }
    return status;
}

bool Debrid::supports(const Release &release) const
{
    if (!available || !connected || busy)
        return false;
    for (const Host &host : hosts)
        if (host.source_id == release.source_id && host.available)
            return host.max_file_size == 0 || release.size_bytes <= host.max_file_size;
    return false;
}

std::vector<Release> parse_catalog(const json::Value &value)
{
    std::vector<Release> releases;
    releases.reserve(value.items().size());
    for (const json::Value &item : value.items())
    {
        Release release;
        release.id = item.text("id");
        release.game_id = item.text("gameId");
        if (release.id.empty() || release.game_id.empty())
            continue;
        release.title_id = item.text("titleId");
        release.title = item.text("title");
        release.genre = item.text("genre");
        release.tagline = item.text("tagline");
        release.description = item.text("description");
        release.provider = item.text("provider");
        release.source_id = item.text("sourceId");
        release.format = item.text("format");
        release.region = item.text("region");
        release.version = item.text("version");
        release.filename = item.text("filename");
        release.artwork_layout = item.text("artworkLayout");
        release.publisher = item.text("publisher");
        release.release_date = item.text("releaseDate");
        release.added_at = item.text("addedAt");
        release.size_bytes = item.number("sizeBytes");
        release.browser_available = item.flag("browserAvailable");
        release.direct_available = item.flag("directAvailable", true);
        releases.push_back(std::move(release));
    }
    return releases;
}

std::vector<Drive> parse_storage(const json::Value &value)
{
    std::vector<Drive> drives;
    for (const json::Value &item : value.items())
    {
        Drive drive;
        drive.id = item.text("id");
        if (drive.id.empty())
            continue;
        drive.label = item.text("label");
        drive.path = item.text("path");
        drive.free_bytes = item.number("freeBytes");
        drive.total_bytes = item.number("totalBytes");
        drive.pending_bytes = item.number("pendingBytes");
        drive.projected_free_bytes = item.number("projectedFreeBytes");
        drive.external = item.flag("external");
        drives.push_back(std::move(drive));
    }
    return drives;
}

std::vector<Job> parse_jobs(const json::Value &value)
{
    std::vector<Job> jobs;
    for (const json::Value &item : value.items())
    {
        Job job;
        job.id = item.text("id");
        if (job.id.empty())
            continue;
        job.release_id = item.text("releaseId");
        job.game_id = item.text("gameId");
        job.title_id = item.text("titleId");
        job.title = item.text("title");
        job.storage_id = item.text("storageId");
        job.filename = item.text("filename");
        job.format = item.text("format");
        job.status = item.text("status");
        job.error = item.text("error");
        job.verification = item.text("verification");
        job.delivery = item.text("delivery");
        job.delivery_phase = item.text("deliveryPhase");
        job.received = item.number("received");
        job.total = item.number("total");
        job.speed = item.number("speed");
        job.retry_at = item.number("retryAt");
        job.order = static_cast<int>(item["order"].as_int());
        jobs.push_back(std::move(job));
    }
    // The backend sorts by order; keep that even if a reply arrives unsorted.
    std::stable_sort(jobs.begin(), jobs.end(),
                     [](const Job &a, const Job &b) { return a.order < b.order; });
    return jobs;
}

std::vector<std::string> parse_favourites(const json::Value &value)
{
    std::vector<std::string> ids;
    for (const json::Value &item : value.items())
    {
        if (item.is_string() && !item.as_string().empty() && !contains(ids, item.as_string()))
            ids.push_back(item.as_string());
    }
    return ids;
}

namespace
{

double optional_bytes(const json::Value &value)
{
    return value.is_number() ? value.as_number() : -1.0;
}

LibraryDrive parse_library_drive(const json::Value &item)
{
    LibraryDrive drive;
    drive.id = item.text("id");
    drive.path = item.text("path");
    drive.label = item.text("label");
    drive.filesystem = item.text("filesystem");
    drive.read_only = item.flag("readOnly");
    drive.total = item.number("totalBytes");
    drive.free = item.number("freeBytes");
    drive.used = item.number("usedBytes");
    return drive;
}

} // namespace

LibrarySnapshot parse_library(const json::Value &value)
{
    LibrarySnapshot snapshot;
    snapshot.status = value.text("status");
    snapshot.message = value.text("message");
    snapshot.provider = value.text("provider");
    snapshot.provider_version = value.text("providerVersion");
    snapshot.busy = value.flag("busy");
    snapshot.stale = value.flag("stale");
    snapshot.checked_at = value.number("checkedAt");
    snapshot.updated_at = value.number("updatedAt");
    snapshot.refresh_after = value.number("refreshAfter");
    for (const json::Value &item : value["games"].items())
    {
        LibraryGame game;
        game.title_id = item.text("titleId");
        if (game.title_id.empty())
            continue;
        game.title = item.text("title");
        game.platform = item.text("platform");
        game.format = item.text("format");
        game.path = item.text("path");
        game.runtime_path = item.text("runtimePath");
        game.source_key = item.text("sourceKey");
        game.location = item.text("location");
        game.installed = item.flag("installed");
        game.mounted = item.flag("mounted");
        game.on_drive = item.flag("onDrive");
        game.managed = item.flag("managed");
        game.can_manage_source = item.flag("canManageSource");
        game.installed_size = optional_bytes(item["installedSizeBytes"]);
        game.size = optional_bytes(item["sizeBytes"]);
        game.size_status = item.text("sizeStatus");
        snapshot.games.push_back(std::move(game));
    }
    for (const json::Value &item : value["capabilities"].items())
    {
        if (item.is_string())
            snapshot.capabilities.push_back(item.as_string());
    }
    const json::Value &action = value["action"];
    if (action.is_object())
    {
        snapshot.has_action = true;
        snapshot.action.id = action.text("id");
        snapshot.action.action = action.text("action");
        snapshot.action.title_id = action.text("titleId");
        snapshot.action.state = action.text("state");
        snapshot.action.message = action.text("message");
    }
    const json::Value &job = value["storageJob"];
    if (job.is_object())
    {
        snapshot.has_storage_job = true;
        StorageJob &out = snapshot.storage_job;
        out.id = static_cast<int>(job["id"].as_int());
        out.operation = job.text("operation");
        out.state = job.text("state");
        out.title_id = job.text("titleId");
        out.source = job.text("source");
        out.destination = job.text("destination");
        out.error = job.text("error");
        out.total = job.number("totalBytes");
        out.processed = job.number("processedBytes");
        out.speed = job.number("speed");
        out.active = job.flag("active");
        out.cancellable = job.flag("cancellable");
        out.cancel_requested = job.flag("cancelRequested");
    }
    snapshot.storage_error = value.text("storageError");
    snapshot.storage_busy = value.flag("storageBusy");
    return snapshot;
}

LibraryStorage parse_library_storage(const json::Value &value)
{
    LibraryStorage storage;
    for (const json::Value &item : value["drives"].items())
        storage.drives.push_back(parse_library_drive(item));
    for (const json::Value &item : value["destinations"].items())
        storage.destinations.push_back(parse_library_drive(item));
    storage.busy = value.flag("busy");
    storage.stale = value.flag("stale");
    storage.updated_at = value.number("updatedAt");
    storage.error = value.text("error");
    return storage;
}

Session parse_session(const json::Value &value)
{
    Session session;
    session.token = value.text("token");
    session.pair_code = value.text("pairCode");
    session.address = value.text("address");
    return session;
}

CatalogueStatus parse_catalogue_status(const json::Value &value)
{
    CatalogueStatus status;
    status.available = value.flag("available");
    status.busy = value.flag("busy");
    status.cached = value.flag("cached");
    status.revision = value["revision"].as_int();
    status.game_count = static_cast<int>(value["gameCount"].as_int());
    status.checked_at = value.number("checkedAt");
    status.check_after = value.number("checkAfter");
    status.error = value.text("error");
    return status;
}

ServiceUpdate parse_service_update(const json::Value &value)
{
    ServiceUpdate update;
    update.available = value.flag("available");
    update.update_available = value.flag("updateAvailable");
    update.busy = value.flag("busy");
    update.restart_required = value.flag("restartRequired");
    update.running_version = value.text("runningVersion");
    update.saved_version = value.text("savedVersion");
    update.latest_version = value.text("latestVersion");
    update.checksum = value.text("checksum");
    update.phase = value.text("phase");
    update.error = value.text("error");
    update.received = value.number("received");
    update.retry_at = value.number("retryAt");
    update.checked_at = value.number("checkedAt");
    update.check_after = value.number("checkAfter");
    return update;
}

TvAppStatus parse_tv_app(const json::Value &value)
{
    TvAppStatus status;
    status.available = value.flag("available");
    status.installed = value.text("installed");
    status.installed_version = value.text("installedVersion");
    status.latest_version = value.text("latestVersion");
    status.checksum = value.text("checksum");
    status.size = value.number("size");
    status.phase = value.text("phase");
    status.error = value.text("error");
    status.busy = value.flag("busy");
    status.received = value.number("received");
    status.retry_at = value.number("retryAt");
    status.checked_at = value.number("checkedAt");
    status.check_after = value.number("checkAfter");
    return status;
}

bool LibrarySnapshot::can(std::string_view capability) const
{
    return contains(capabilities, capability);
}

bool LibrarySnapshot::working() const
{
    return (has_action && (action.state == "queued" || action.state == "running")) ||
           (has_storage_job && storage_job.active) || storage_busy;
}

const LibraryGame *LibrarySnapshot::game(std::string_view title_id,
                                         std::string_view source_key) const
{
    for (const LibraryGame &candidate : games)
    {
        if (candidate.title_id == title_id && candidate.source_key == source_key)
            return &candidate;
    }
    return nullptr;
}

std::vector<Game> group_games(const std::vector<Release> &releases)
{
    std::vector<Game> games;
    for (const Release &release : releases)
    {
        Game *game = nullptr;
        for (Game &existing : games)
        {
            if (existing.id == release.game_id)
            {
                game = &existing;
                break;
            }
        }
        if (game == nullptr)
        {
            games.emplace_back();
            game = &games.back();
            game->id = release.game_id;
            game->title = release.title;
            game->genre = release.genre;
            game->tagline = release.tagline;
            game->description = release.description;
            game->artwork_layout = release.artwork_layout;
            game->publisher = release.publisher;
            game->release_date = release.release_date;
        }
        game->releases.push_back(release);
    }
    return games;
}

std::vector<int> latest_games(const std::vector<Game> &games, std::string_view today)
{
    std::vector<int> result;
    for (int i = 0; i < static_cast<int>(games.size()); ++i)
    {
        const std::string &date = games[static_cast<std::size_t>(i)].release_date;
        if (!date.empty() && std::string_view(date) <= today)
            result.push_back(i);
    }
    std::stable_sort(result.begin(), result.end(),
                     [&](int a, int b)
                     {
                         const Game &x = games[static_cast<std::size_t>(a)];
                         const Game &y = games[static_cast<std::size_t>(b)];
                         if (x.release_date != y.release_date)
                             return x.release_date > y.release_date;
                         return compare_folded(x.title, y.title) < 0;
                     });
    if (result.size() > 20)
        result.resize(20);
    return result;
}

std::vector<int> newly_added_games(const std::vector<Game> &games)
{
    std::vector<std::pair<int, double>> dated;
    for (int i = 0; i < static_cast<int>(games.size()); ++i)
    {
        const Game &game = games[static_cast<std::size_t>(i)];
        const double added = game.releases.empty() ? 0 : addition_time(game.releases.front().added_at);
        if (added > 0)
            dated.emplace_back(i, added);
    }
    std::stable_sort(dated.begin(), dated.end(), [&](const auto &a, const auto &b)
    {
        return a.second != b.second ? a.second > b.second
            : compare_folded(games[static_cast<std::size_t>(a.first)].title,
                             games[static_cast<std::size_t>(b.first)].title) < 0;
    });
    if (dated.size() > 20)
        dated.resize(20);
    std::stable_sort(dated.begin(), dated.end(), [&](const auto &a, const auto &b)
    {
        const Game &x = games[static_cast<std::size_t>(a.first)];
        const Game &y = games[static_cast<std::size_t>(b.first)];
        return x.release_date != y.release_date ? x.release_date > y.release_date
                                               : compare_folded(x.title, y.title) < 0;
    });
    std::vector<int> result;
    for (const auto &entry : dated)
        result.push_back(entry.first);
    return result;
}

bool matches_query(const Game &game, std::string_view query)
{
    query = trim(query);
    if (query.empty())
        return true;
    if (contains_folded(game.title, query))
        return true;
    for (const Release &release : game.releases)
    {
        if (contains_folded(release.title_id, query))
            return true;
    }
    return false;
}

const char *sort_label(Sort sort)
{
    switch (sort)
    {
    case Sort::title:
        return tr("Title A\xE2\x80\x93Z");
    case Sort::title_desc:
        return tr("Title Z\xE2\x80\x93"
               "A");
    case Sort::size:
        return tr("Smallest download");
    case Sort::size_desc:
        return tr("Largest download");
    default:
        return tr("Release date (newest first)");
    }
}

const char *size_label(SizeFilter size)
{
    switch (size)
    {
    case SizeFilter::small:
        return tr("Under 10 GB");
    case SizeFilter::medium:
        return tr("10\xE2\x80\x93"
                  "50 GB");
    case SizeFilter::large:
        return tr("50\xE2\x80\x93"
                  "100 GB");
    case SizeFilter::huge:
        return tr("100 GB or more");
    default:
        return tr("Any size");
    }
}

bool contains(const std::vector<std::string> &values, std::string_view value)
{
    return std::find(values.begin(), values.end(), value) != values.end();
}

std::vector<int> browse(const std::vector<Game> &games, const BrowseFilters &filters,
                        const std::vector<std::string> &favourites)
{
    struct Match
    {
        int index;
        double size; // the smallest matching option
    };
    std::vector<Match> matches;
    for (int i = 0; i < static_cast<int>(games.size()); ++i)
    {
        const Game &game = games[static_cast<std::size_t>(i)];
        if (!matches_query(game, filters.query))
            continue;
        if (filters.favourites && !contains(favourites, game.id))
            continue;
        double smallest = -1.0;
        for (const Release &release : game.releases)
        {
            if (!filters.source.empty() && release.source_id != filters.source)
                continue;
            if (!filters.format.empty() && release.format != filters.format)
                continue;
            if (!filters.region.empty() &&
                (release.region.empty() ? "unknown" : release.region) != filters.region)
                continue;
            const double gb = release.size_bytes / 1e9;
            const bool sized = filters.size == SizeFilter::small    ? gb < 10.0
                               : filters.size == SizeFilter::medium ? gb >= 10.0 && gb < 50.0
                               : filters.size == SizeFilter::large  ? gb >= 50.0 && gb < 100.0
                               : filters.size == SizeFilter::huge   ? gb >= 100.0
                                                                    : true;
            if (!sized)
                continue;
            if (smallest < 0.0 || release.size_bytes < smallest)
                smallest = release.size_bytes;
        }
        if (smallest >= 0.0)
            matches.push_back({i, smallest});
    }
    const Sort sort = filters.sort;
    std::stable_sort(matches.begin(), matches.end(),
                     [&](const Match &a, const Match &b)
                     {
                         const Game &x = games[static_cast<std::size_t>(a.index)];
                         const Game &y = games[static_cast<std::size_t>(b.index)];
                         const int title = compare_folded(x.title, y.title);
                         switch (sort)
                         {
                         case Sort::title_desc:
                             return title > 0;
                         case Sort::release:
                             // Newest first; an undated game sorts as "" (last).
                             if (x.release_date != y.release_date)
                                 return x.release_date > y.release_date;
                             return title < 0;
                         case Sort::size:
                             if (a.size != b.size)
                                 return a.size < b.size;
                             return title < 0;
                         case Sort::size_desc:
                             if (a.size != b.size)
                                 return a.size > b.size;
                             return title < 0;
                         default:
                             return title < 0;
                         }
                     });
    std::vector<int> result;
    result.reserve(matches.size());
    for (const Match &match : matches)
        result.push_back(match.index);
    return result;
}

std::vector<std::pair<std::string, std::string>> source_choices(const std::vector<Game> &games)
{
    std::vector<std::pair<std::string, std::string>> choices;
    for (const Game &game : games)
    {
        for (const Release &release : game.releases)
        {
            bool known = false;
            for (const auto &choice : choices)
                known = known || choice.first == release.source_id;
            if (!known && !release.source_id.empty())
                choices.emplace_back(release.source_id, release.provider);
        }
    }
    return choices;
}

std::vector<std::string> format_choices(const std::vector<Game> &games)
{
    std::vector<std::string> choices;
    for (const Game &game : games)
    {
        for (const Release &release : game.releases)
        {
            if (!release.format.empty() && !contains(choices, release.format))
                choices.push_back(release.format);
        }
    }
    return choices;
}

bool job_active(const Job &job)
{
    return job.status == "queued" || job.status == "downloading" || job.status == "retrying" ||
           job.status == "verifying";
}

bool job_inactive(const Job &job)
{
    return job.status == "complete" || job.status == "cancelled";
}

const char *status_label(std::string_view status)
{
    if (status == "downloading")
        return tr("Downloading");
    if (status == "verifying")
        return tr("Verifying");
    if (status == "queued")
        return tr("Queued");
    if (status == "retrying")
        return tr("Retrying");
    if (status == "paused")
        return tr("Paused");
    if (status == "error")
        return tr("Needs attention");
    if (status == "complete")
        return tr("Downloaded");
    if (status == "cancelled")
        return tr("Cancelled");
    return "";
}

const Job *release_job(const Release &release, const std::vector<Job> &jobs)
{
    const Job *best = nullptr;
    for (const Job &job : jobs)
    {
        if (job.release_id != release.id || job.title_id != release.title_id ||
            job.format != release.format || job.filename != release.filename ||
            job.total != release.size_bytes || job.status == "cancelled")
            continue;
        if (best == nullptr || status_rank(job.status) < status_rank(best->status))
            best = &job;
    }
    return best;
}

CollectionState release_state(const Release &release, const std::vector<Job> &jobs,
                              const std::vector<LibraryGame> &library)
{
    CollectionState state;
    const Job *job = release_job(release, jobs);
    if (job != nullptr && job->status != "complete")
    {
        state.kind = CollectionState::Kind::download;
        state.label = status_label(job->status);
        state.job = job;
        return state;
    }
    // ShadowMount reports no trustworthy version, so a versioned option is
    // never treated as already in the Library on the strength of its title ID.
    const LibraryGame *related = nullptr;
    for (const LibraryGame &copy : library)
    {
        if (copy.title_id != release.title_id || copy.platform != "ps5" ||
            !(copy.on_drive || copy.installed))
            continue;
        if (related == nullptr)
            related = &copy;
        const std::size_t slash = copy.path.rfind('/');
        const std::string_view name = slash == std::string::npos
                                          ? std::string_view(copy.path)
                                          : std::string_view(copy.path).substr(slash + 1);
        if (copy.on_drive && copy.format == release.format && release.version.empty() &&
            name == release.filename && (copy.size < 0.0 || copy.size == release.size_bytes))
        {
            state.kind = CollectionState::Kind::library;
            state.label = tr("In Library");
            state.library = &copy;
            return state;
        }
    }
    if (job != nullptr)
    {
        state.kind = CollectionState::Kind::download;
        state.label = tr("Downloaded");
        state.job = job;
        return state;
    }
    if (related != nullptr)
    {
        state.kind = CollectionState::Kind::related;
        state.label = tr("Related copy in Library");
        state.library = related;
    }
    return state;
}

CollectionState game_state(const Game &game, const std::vector<Job> &jobs,
                           const std::vector<LibraryGame> &library)
{
    // An unfinished download first, then a Library copy, then a finished
    // download, then anything else (gameState in src/collection.ts).
    std::vector<CollectionState> states;
    for (const Release &release : game.releases)
    {
        CollectionState state = release_state(release, jobs, library);
        if (state.kind != CollectionState::Kind::none)
            states.push_back(state);
    }
    for (const CollectionState &state : states)
    {
        if (state.job != nullptr && state.job->status != "complete")
            return state;
    }
    for (const CollectionState &state : states)
    {
        if (state.kind == CollectionState::Kind::library)
            return state;
    }
    for (const CollectionState &state : states)
    {
        if (state.kind == CollectionState::Kind::download)
            return state;
    }
    return states.empty() ? CollectionState{} : states.front();
}

int default_drive(const std::vector<Drive> &drives, std::string_view preferred)
{
    for (int i = 0; i < static_cast<int>(drives.size()); ++i)
    {
        if (!preferred.empty() && drives[static_cast<std::size_t>(i)].id == preferred)
            return i;
    }
    for (int i = 0; i < static_cast<int>(drives.size()); ++i)
    {
        if (drives[static_cast<std::size_t>(i)].external)
            return i;
    }
    return drives.empty() ? -1 : 0;
}

SpacePlan plan_space(const Release &release, const Drive &drive, const std::vector<Job> &jobs)
{
    SpacePlan plan;
    for (const Job &job : jobs)
    {
        if (job.release_id == release.id && job.storage_id == drive.id &&
            job.status != "complete" && job.status != "cancelled")
        {
            plan.planned = true;
            break;
        }
    }
    // Download again starts from zero. Cancelled history may describe a deleted
    // partial or a disconnected drive and cannot reduce a fresh download's size.
    double needed = plan.planned ? 0.0 : release.size_bytes;
    plan.after = drive.projected_free_bytes - needed;
    plan.enough = plan.after >= kSpaceMargin;
    return plan;
}

JobView job_view(const Job &job)
{
    if (job.status == "complete")
        return JobView::finished;
    if (job.status == "cancelled")
        return JobView::cancelled;
    if (job.status == "error")
        return JobView::failed;
    return JobView::active;
}

const char *job_view_label(JobView view)
{
    switch (view)
    {
    case JobView::finished:
        return tr("Finished");
    case JobView::failed:
        return tr("Failed");
    case JobView::cancelled:
        return tr("Cancelled");
    default:
        return tr("Active");
    }
}

std::string format_bytes(double bytes)
{
    // 49.4 GB in English, 49,4 GB in German, 49,4 ГБ in Russian.
    const bool giga = bytes >= 1e9;
    return i18n::decimal(giga ? bytes / 1e9 : bytes / 1e6) + " " + std::string(i18n::unit(giga));
}

std::string format_age(double unix_time, double now)
{
    if (unix_time <= 0.0)
        return tr("never");
    const double age = std::max(0.0, now - unix_time);
    if (age < 60.0)
        return tr("just now");
    if (age < 3600.0)
        return trn(static_cast<long>(age / 60.0), "{count} min ago", "{count} min ago");
    if (age < 86400.0)
        return trn(static_cast<long>(age / 3600.0), "{count} h ago", "{count} h ago");
    return trn(static_cast<long>(age / 86400.0), "{count} day ago", "{count} days ago");
}

std::string app_release_version(std::string_view content_version)
{
    // Two digits, three and three, as the package's param.json writes it.
    if (content_version.size() != 10 || content_version[2] != '.' || content_version[6] != '.')
        return {};
    unsigned parts[3] = {};
    const std::size_t starts[3] = {0, 3, 7};
    const std::size_t lengths[3] = {2, 3, 3};
    for (int i = 0; i < 3; ++i)
    {
        for (std::size_t j = 0; j < lengths[i]; ++j)
        {
            const char c = content_version[starts[i] + j];
            if (c < '0' || c > '9')
                return {};
            parts[i] = parts[i] * 10 + static_cast<unsigned>(c - '0');
        }
    }
    return std::to_string(parts[0]) + "." + std::to_string(parts[1]) + "." +
           std::to_string(parts[2]);
}

bool check_fresh(double checked_at, double now)
{
    return checked_at > 0.0 && now - checked_at <= 900.0;
}

std::string format_date(std::string_view iso)
{
    if (iso.size() < 10 || iso[4] != '-' || iso[7] != '-')
        return std::string(iso);
    const int year = std::atoi(std::string(iso.substr(0, 4)).c_str());
    const int month = (iso[5] - '0') * 10 + (iso[6] - '0');
    const int day = (iso[8] - '0') * 10 + (iso[9] - '0');
    if (month < 1 || month > 12 || day < 1 || day > 31)
        return std::string(iso);
    // As the browser writes it (Intl, medium date): "Jul 18, 2026", "18.07.2026".
    return i18n::date(year, month, day);
}

} // namespace orbit
