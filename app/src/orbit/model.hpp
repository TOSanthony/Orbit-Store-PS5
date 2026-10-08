// Orbit Store TV app - The Orbit API's data, and the storefront rules built on it.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// These mirror the backend's JSON (docs/api.md) and the web storefront's rules
// (src/catalog.ts, src/browse.ts, src/collection.ts, src/Details.tsx), so the TV
// app and the phone/browser app show the same games, states and numbers.

#pragma once

#include "orbit/json.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace orbit
{

struct System
{
    std::string name;
    std::string version;
    std::string platform; // "ps5", "desktop" or "fixture"
    bool paired = false;
    bool local_session = false;
    bool state_healthy = true;
    std::int64_t catalogue_revision = 0;
    std::string preferred_storage;
};

struct SourceOption
{
    std::string id;
    std::string label;
    int release_count = 0;
    bool download_ready = false;
};

struct Sources
{
    std::vector<std::string> enabled;
    bool acknowledged = false;
    int notice_version = 0;
    std::vector<SourceOption> options;
};

// One download option (a "release" at the API boundary).
struct Release
{
    std::string id;
    std::string game_id;
    std::string title_id;
    std::string title;
    std::string genre;
    std::string tagline;
    std::string description;
    std::string provider;
    std::string source_id;
    std::string format;
    std::string region; // EUR, USA, JPN, ASIA; empty when unknown
    std::string version;
    std::string filename;
    std::string artwork_layout; // "wide" or "ambient"
    std::string publisher;
    std::string release_date; // YYYY-MM-DD or empty
    std::string added_at;
    double size_bytes = 0.0;
    // Provider capabilities (0.5.0): a browser-only option downloads through
    // the PS5 web browser, which the TV app leaves to the browser version.
    bool browser_available = false;
    bool direct_available = true;

    bool browser_only() const
    {
        return browser_available && !direct_available;
    }
};

// One card: every option that shares a curated gameId.
struct Game
{
    std::string id;
    std::string title;
    std::string genre;
    std::string tagline;
    std::string description;
    std::string artwork_layout;
    std::string publisher;
    std::string release_date;
    std::vector<Release> releases;

    bool wide_art() const
    {
        return artwork_layout == "wide";
    }
};

struct Debrid
{
    struct Host
    {
        std::string source_id;
        bool available = false;
        double max_file_size = 0;
    };
    bool available = false;
    bool connected = false;
    bool busy = false;
    std::vector<Host> hosts;
    bool supports(const Release &release) const;
};

struct Drive
{
    std::string id;
    std::string label;
    std::string path;
    double free_bytes = 0.0;
    double total_bytes = 0.0;
    double pending_bytes = 0.0;
    double projected_free_bytes = 0.0;
    bool external = false;
};

struct Job
{
    std::string id;
    std::string release_id;
    std::string game_id;
    std::string title_id;
    std::string title;
    std::string storage_id;
    std::string filename;
    std::string format;
    std::string status; // queued downloading paused retrying verifying complete cancelled error
    std::string error;
    std::string verification;
    std::string delivery;
    std::string delivery_phase;
    double received = 0.0;
    double total = 0.0;
    double speed = 0.0;
    double retry_at = 0.0; // Unix seconds
    int order = 0;
};

// The Library (docs/library.md): ShadowMount's inventory as Orbit reports it.
struct LibraryGame
{
    std::string title_id;
    std::string title;
    std::string platform; // "ps5", "ps4" or "unknown"
    std::string format;
    std::string path;
    std::string runtime_path;
    std::string source_key;
    std::string location;
    bool installed = false;
    bool mounted = false;
    bool on_drive = false;
    bool managed = false;
    bool can_manage_source = false;
    double installed_size = -1.0; // bytes, or -1 when not reported
    double size = -1.0;           // bytes, or -1 when unknown
    std::string size_status;      // ready, unknown, unavailable
};

struct LibraryAction
{
    std::string id;
    std::string action;
    std::string title_id;
    std::string state; // queued, running, complete, error
    std::string message;
};

struct StorageJob
{
    int id = 0;
    std::string operation;
    std::string state;
    std::string title_id;
    std::string source;
    std::string destination;
    std::string error;
    double total = 0.0;
    double processed = 0.0;
    double speed = 0.0;
    bool active = false;
    bool cancellable = false;
    bool cancel_requested = false;
};

struct LibrarySnapshot
{
    std::string status; // idle, ready, unavailable, unsupported, error
    std::string message;
    std::string provider;
    std::string provider_version;
    bool busy = false;
    bool stale = false;
    double checked_at = 0.0;
    double updated_at = 0.0;
    double refresh_after = 0.0;
    std::vector<LibraryGame> games;
    std::vector<std::string> capabilities;
    bool has_action = false;
    LibraryAction action;
    bool has_storage_job = false;
    StorageJob storage_job;
    std::string storage_error;
    bool storage_busy = false;

    bool ready() const
    {
        return status == "ready";
    }
    bool can(std::string_view capability) const;
    // A Library action or storage job is queued or running.
    bool working() const;
    const LibraryGame *game(std::string_view title_id, std::string_view source_key) const;
};

struct LibraryDrive
{
    std::string id;
    std::string path;
    std::string label;
    std::string filesystem;
    bool read_only = false;
    double total = 0.0;
    double free = 0.0;
    double used = 0.0;
};

struct LibraryStorage
{
    std::vector<LibraryDrive> drives;
    std::vector<LibraryDrive> destinations;
    bool busy = false;
    bool stale = false;
    double updated_at = 0.0;
    std::string error;
};

// The console's own session (GET /api/v1/session, loopback only).
struct Session
{
    std::string token;
    std::string pair_code; // what a phone or computer enters to pair
    std::string address;   // the console's LAN address; empty when Orbit doesn't know it
};

// Catalogue updates (GET /api/v1/catalog/updates).
struct CatalogueStatus
{
    bool available = false; // refreshing works (Orbit on a console)
    bool busy = false;
    bool cached = false;
    std::int64_t revision = 0;
    int game_count = 0;
    double checked_at = 0.0; // Unix seconds
    double check_after = 0.0;
    std::string error;
};

// The download service's own updates (GET /api/v1/updates): Orbit, the payload.
struct ServiceUpdate
{
    bool available = false; // updating works (Orbit on a console)
    bool update_available = false;
    bool busy = false;
    bool restart_required = false; // a saved version differs from the running one
    std::string running_version;
    std::string saved_version; // what the next start runs
    std::string latest_version;
    std::string checksum;
    std::string phase; // idle checking checked downloading saving saved error stopping
    std::string error;
    double received = 0.0;
    double retry_at = 0.0; // Unix seconds
    double checked_at = 0.0;
    double check_after = 0.0;
};

// The TV app installer (GET /api/v1/tv-app): this app's own releases.
struct TvAppStatus
{
    bool available = false;
    std::string installed; // none, orbit, manual, folder or blocked
    std::string installed_version;
    std::string latest_version;
    std::string checksum;
    double size = 0.0;
    std::string phase; // idle checking checked downloading verifying installing installed error
    std::string error;
    bool busy = false;
    double received = 0.0;
    double retry_at = 0.0;
    double checked_at = 0.0;
    double check_after = 0.0;
};

// ---- parsing (tolerant: missing or mistyped fields read as empty) ----
System parse_system(const json::Value &value);
Sources parse_sources(const json::Value &value);
Debrid parse_debrid(const json::Value &value);
std::vector<Release> parse_catalog(const json::Value &value);
std::vector<Drive> parse_storage(const json::Value &value);
std::vector<Job> parse_jobs(const json::Value &value);
std::vector<std::string> parse_favourites(const json::Value &value);
LibrarySnapshot parse_library(const json::Value &value);
LibraryStorage parse_library_storage(const json::Value &value);
Session parse_session(const json::Value &value);
CatalogueStatus parse_catalogue_status(const json::Value &value);
ServiceUpdate parse_service_update(const json::Value &value);
TvAppStatus parse_tv_app(const json::Value &value);

// ---- storefront rules ----

// Groups options by gameId in catalogue order (src/catalog.ts).
std::vector<Game> group_games(const std::vector<Release> &releases);

// Dated games newest first, undated and future-dated left out, at most 20
// (the web Discover's "Latest releases"). `today` is YYYY-MM-DD.
std::vector<int> latest_games(const std::vector<Game> &games, std::string_view today);

// Case-insensitive match on the game's title or any option's title ID.
bool matches_query(const Game &game, std::string_view query);

// Browse's filters and order, as the browser's (src/browse.ts).
enum class Sort : std::uint8_t
{
    release,    // release date, newest first; undated games last
    title,      // A to Z
    title_desc, // Z to A
    size,       // smallest matching download first
    size_desc,  // largest matching download first
    count,
};
const char *sort_label(Sort sort);

enum class SizeFilter : std::uint8_t
{
    any,
    small,  // under 10 GB
    medium, // 10 to 50 GB
    large,  // 50 to 100 GB
    huge,   // 100 GB or more
    count,
};
const char *size_label(SizeFilter size);

struct BrowseFilters
{
    std::string query;
    std::string source; // a sourceId; empty for all
    std::string format; // empty for all
    std::string region; // empty for all, "unknown" for missing metadata
    SizeFilter size = SizeFilter::any;
    bool favourites = false;
    Sort sort = Sort::release;

    // Source, format, region, size and sort are at their defaults (Reset filters).
    bool narrowed() const
    {
        return !source.empty() || !format.empty() || !region.empty() || size != SizeFilter::any ||
               sort != Sort::release;
    }
};
// Indices of the games with an option matching every filter, in order. Size
// sorting uses the smallest option that matches.
std::vector<int> browse(const std::vector<Game> &games, const BrowseFilters &filters,
                        const std::vector<std::string> &favourites);
// The sources (sourceId, provider) and formats the catalogue offers, in order.
std::vector<std::pair<std::string, std::string>> source_choices(const std::vector<Game> &games);
std::vector<std::string> format_choices(const std::vector<Game> &games);
// Select the 20 newest catalogue additions, then order by release date (undated last).
std::vector<int> newly_added_games(const std::vector<Game> &games);
bool contains(const std::vector<std::string> &values, std::string_view value);

// Statuses, as the web storefront words them.
bool job_active(const Job &job);   // queued, downloading, retrying, verifying
bool job_inactive(const Job &job); // complete or cancelled
const char *status_label(std::string_view status);

// The most relevant unfinished-or-finished job for one option, matched on the
// option's identity so a catalogue revision cannot relabel a saved job.
const Job *release_job(const Release &release, const std::vector<Job> &jobs);
// Where an option or a game stands in the player's collection (src/collection.ts):
// an unfinished download first, then an exact Library copy, a finished
// download, or a related copy (same title ID, unconfirmed match).
struct CollectionState
{
    enum class Kind : std::uint8_t
    {
        none,
        download,
        library,
        related,
    } kind = Kind::none;
    const char *label = nullptr;
    const Job *job = nullptr;
    const LibraryGame *library = nullptr;
};
CollectionState release_state(const Release &release, const std::vector<Job> &jobs,
                              const std::vector<LibraryGame> &library);
CollectionState game_state(const Game &game, const std::vector<Job> &jobs,
                           const std::vector<LibraryGame> &library);

// Where a new download goes by default: the console's preferred drive, then
// the first external drive, then any.
int default_drive(const std::vector<Drive> &drives, std::string_view preferred);

// Space left on a drive after its queue and this option (Details.tsx). A
// value below kSpaceMargin means the backend will refuse the download.
constexpr double kSpaceMargin = 16.0 * 1024.0 * 1024.0;
struct SpacePlan
{
    double after = 0.0;   // bytes left; negative is a shortfall
    bool planned = false; // the option is already queued on this drive
    bool enough = false;
};
SpacePlan plan_space(const Release &release, const Drive &drive, const std::vector<Job> &jobs);

// The download view a job belongs to on the Downloads screen.
enum class JobView : std::uint8_t
{
    active,
    finished,
    failed,
    cancelled,
    count,
};
JobView job_view(const Job &job);
const char *job_view_label(JobView view);

// ---- versions ----

// (Versions compare with starter::compare_versions.)
// This app's release version from its package's content version:
// "01.002.003" is 1.2.3, as the release feed numbers it. Empty if malformed.
std::string app_release_version(std::string_view content_version);
// A checked release is fresh for 15 minutes; installing needs a fresh check.
bool check_fresh(double checked_at, double now);

// ---- formatting ----
std::string format_bytes(double bytes);        // "12.4 GB", "512.0 MB" (as the web app)
std::string format_date(std::string_view iso); // "Feb 15, 2022"
// How long ago, relative ("just now", "12 min ago", "3 h ago", "2 days ago",
// "never"), because the console's runtime offers no local time zone.
std::string format_age(double unix_time, double now);

} // namespace orbit
