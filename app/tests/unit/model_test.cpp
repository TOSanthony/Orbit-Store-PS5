// Orbit Store TV app - Storefront rules shared with the browser storefront.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "orbit/model.hpp"

#include <gtest/gtest.h>

#include <tuple>
#include <algorithm>

namespace orbit
{
namespace
{

Release make_release(const char *id, const char *game, const char *title, const char *date,
                     double gigabytes, const char *title_id = "PPSA00001")
{
    Release release;
    release.id = id;
    release.game_id = game;
    release.title = title;
    release.release_date = date;
    release.size_bytes = gigabytes * 1e9;
    release.title_id = title_id;
    release.format = "FFPFSC";
    release.filename = std::string(id) + ".ffpfsc";
    return release;
}

Job job_for(const Release &release, const char *status, const char *storage = "usb0")
{
    Job job;
    job.id = std::string("job-") + release.id;
    job.release_id = release.id;
    job.title_id = release.title_id;
    job.format = release.format;
    job.filename = release.filename;
    job.total = release.size_bytes;
    job.status = status;
    job.storage_id = storage;
    return job;
}

TEST(Model, GroupsOptionsIntoOneCardPerGameInCatalogueOrder)
{
    const std::vector<Game> games = group_games({make_release("a1", "alpha", "Alpha", "", 1),
                                                 make_release("b1", "beta", "Beta", "", 2),
                                                 make_release("a2", "alpha", "Alpha EU", "", 3)});
    ASSERT_EQ(games.size(), 2u);
    EXPECT_EQ(games[0].id, "alpha");
    EXPECT_EQ(games[0].title, "Alpha"); // the first option names the card
    EXPECT_EQ(games[0].releases.size(), 2u);
    EXPECT_EQ(games[1].releases.size(), 1u);
}

TEST(Model, LatestReleasesAreDatedPastAndNewestFirst)
{
    std::vector<Release> releases;
    releases.push_back(make_release("old", "old", "Old", "2024-01-01", 1));
    releases.push_back(make_release("none", "none", "Undated", "", 1));
    releases.push_back(make_release("new", "new", "New", "2026-09-30", 1));
    releases.push_back(make_release("future", "future", "Future", "2027-01-01", 1));
    releases.push_back(make_release("tie-b", "tie-b", "b tie", "2025-05-05", 1));
    releases.push_back(make_release("tie-a", "tie-a", "A tie", "2025-05-05", 1));
    const std::vector<Game> games = group_games(releases);
    const std::vector<int> latest = latest_games(games, "2026-10-04");
    std::vector<std::string> ids;
    for (int index : latest)
        ids.push_back(games[static_cast<std::size_t>(index)].id);
    EXPECT_EQ(ids, (std::vector<std::string>{"new", "tie-a", "tie-b", "old"}));

    std::vector<Release> many;
    for (int i = 0; i < 30; ++i)
    {
        const std::string id = "g" + std::to_string(i);
        many.push_back(make_release(id.c_str(), id.c_str(), id.c_str(), "2026-01-01", 1));
    }
    EXPECT_EQ(latest_games(group_games(many), "2026-10-04").size(), 20u);
}

TEST(Model, NewlyAddedUsesCatalogueDatesWithTimezoneOffsetsAndCapsTheRow)
{
    Release old = make_release("old", "old", "Old", "2026-10-07", 2);
    old.added_at = "2026-10-03T22:00:00+05:30";
    Release newer = make_release("new", "new", "New", "2020-01-01", 2);
    newer.added_at = "2026-10-03T18:00:00Z";
    Release tie = make_release("tie", "tie", "A tied addition", "", 2);
    tie.added_at = "2026-10-03T20:00:00+02:00";
    Release invalid = make_release("bad", "bad", "Bad", "", 2);
    invalid.added_at = "not-a-date";
    EXPECT_EQ(newly_added_games(group_games({old, newer, tie, invalid,
                  make_release("none", "none", "None", "", 2)})), (std::vector<int>{0, 1, 2}));
    std::vector<Release> many;
    for (int i = 0; i < 25; ++i)
    {
        Release r = newer;
        r.game_id = std::to_string(i);
        many.push_back(r);
    }
    old.release_date = "2026-10-07";
    many.push_back(old); // an older addition cannot enter just because its game is newer
    const auto picked = newly_added_games(group_games(many));
    EXPECT_EQ(picked.size(), 20u);
    EXPECT_TRUE(std::find(picked.begin(), picked.end(), 25) == picked.end());
}

TEST(Model, RegionFiltersMatchTheSameOptionAndKeepUnknownExplicit)
{
    json::Value parsed;
    ASSERT_TRUE(json::parse(R"([
      {"id":"image","gameId":"game","format":"FFPFSC","region":"EUR","sourceId":"archive","sizeBytes":10000000000},
      {"id":"pkg","gameId":"game","format":"FPKG","region":"USA","sourceId":"vikingfile","sizeBytes":60000000000},
      {"id":"missing","gameId":"unknown","format":"FPKG","sourceId":"vikingfile","sizeBytes":2000000000}
    ])", &parsed));
    const auto games = group_games(parse_catalog(parsed));
    BrowseFilters filters;
    filters.region = "EUR";
    filters.format = "FPKG";
    EXPECT_TRUE(browse(games, filters, {}).empty());
    filters.region = "USA";
    filters.source = "vikingfile";
    EXPECT_EQ(browse(games, filters, {}), (std::vector<int>{0}));
    filters.size = SizeFilter::small;
    EXPECT_TRUE(browse(games, filters, {}).empty());
    filters.region = "unknown";
    EXPECT_EQ(browse(games, filters, {}), (std::vector<int>{1}));
    EXPECT_TRUE(filters.narrowed());
    EXPECT_EQ(browse(games, BrowseFilters{}, {}).size(), 2u);
}

TEST(Model, SearchMatchesTitlesAndAnyOptionsTitleId)
{
    const std::vector<Game> games =
        group_games({make_release("a", "a", "Ember Hollow", "", 5, "PPSA90003"),
                     make_release("a-eu", "a", "Ember Hollow", "", 5, "PPSB90003"),
                     make_release("b", "b", "Tidewater", "", 1, "PPSA90001")});
    const auto search = [&](const char *query)
    {
        BrowseFilters filters;
        filters.query = query;
        return browse(games, filters, {});
    };
    EXPECT_EQ(search("  hollow ").size(), 1u);
    EXPECT_EQ(search("ppsb9"), (std::vector<int>{0}));
    EXPECT_EQ(search("").size(), 2u);
    EXPECT_TRUE(search("zelda").empty());
}

TEST(Model, BrowseSortsByTitleDateOrSmallestOption)
{
    const std::vector<Game> games = group_games(
        {make_release("c", "c", "charlie", "2024-01-01", 30),
         make_release("a", "a", "Alpha", "", 50), make_release("a2", "a", "Alpha", "", 2),
         make_release("b", "b", "Bravo", "2026-01-01", 10)});
    const auto titles = [&](Sort sort)
    {
        BrowseFilters filters;
        filters.sort = sort;
        std::vector<std::string> out;
        for (int index : browse(games, filters, {}))
            out.push_back(games[static_cast<std::size_t>(index)].id);
        return out;
    };
    // The browser's default is release date, newest first, undated last.
    EXPECT_EQ(BrowseFilters{}.sort, Sort::release);
    EXPECT_EQ(titles(Sort::release), (std::vector<std::string>{"b", "c", "a"}));
    EXPECT_EQ(titles(Sort::title), (std::vector<std::string>{"a", "b", "c"}));
    EXPECT_EQ(titles(Sort::title_desc), (std::vector<std::string>{"c", "b", "a"}));
    EXPECT_EQ(titles(Sort::size), (std::vector<std::string>{"a", "b", "c"}));
    EXPECT_EQ(titles(Sort::size_desc), (std::vector<std::string>{"c", "b", "a"}));
}

TEST(Model, BrowseFiltersMatchOneOptionAndFavourites)
{
    Release archive = make_release("a", "a", "Alpha", "", 60);
    archive.source_id = "archive";
    archive.provider = "Archive.org";
    Release viking = make_release("a-v", "a", "Alpha", "", 8);
    viking.source_id = "vikingfile";
    viking.provider = "Vikingfile";
    viking.format = "exFAT";
    Release bravo = make_release("b", "b", "Bravo", "", 120);
    bravo.source_id = "archive";
    bravo.provider = "Archive.org";
    const std::vector<Game> games = group_games({archive, viking, bravo});
    const auto ids = [&](const BrowseFilters &filters, const std::vector<std::string> &favourites)
    {
        std::vector<std::string> out;
        for (int index : browse(games, filters, favourites))
            out.push_back(games[static_cast<std::size_t>(index)].id);
        return out;
    };
    BrowseFilters filters;
    filters.sort = Sort::title;
    filters.source = "vikingfile";
    EXPECT_EQ(ids(filters, {}), (std::vector<std::string>{"a"}));
    // Source, format and size must hold for the same option.
    filters.size = SizeFilter::large;
    EXPECT_TRUE(ids(filters, {}).empty());
    filters = BrowseFilters{};
    filters.sort = Sort::title;
    filters.size = SizeFilter::huge;
    EXPECT_EQ(ids(filters, {}), (std::vector<std::string>{"b"}));
    filters = BrowseFilters{};
    filters.format = "exFAT";
    EXPECT_EQ(ids(filters, {}), (std::vector<std::string>{"a"}));
    // Size sorting uses the smallest option that matches the filters.
    filters = BrowseFilters{};
    filters.source = "archive";
    filters.sort = Sort::size;
    EXPECT_EQ(ids(filters, {}), (std::vector<std::string>{"a", "b"}));
    filters.favourites = true;
    EXPECT_EQ(ids(filters, {"b", "gone"}), (std::vector<std::string>{"b"}));
    EXPECT_TRUE(filters.narrowed());
    EXPECT_FALSE(BrowseFilters{}.narrowed());
    EXPECT_EQ(source_choices(games).size(), 2u);
    EXPECT_EQ(format_choices(games), (std::vector<std::string>{"FFPFSC", "exFAT"}));
    EXPECT_STREQ(sort_label(Sort::release), "Release date (newest first)");
    EXPECT_STREQ(size_label(SizeFilter::small), "Under 10 GB");
}

TEST(Model, FpkgIsASeparateSelectableFormatWithinTheSameGame)
{
    Release image = make_release("image", "game", "Game", "", 10);
    image.source_id = "archive";
    Release pkg = image;
    pkg.id = "pkg";
    pkg.source_id = "vikingfile";
    pkg.format = "FPKG";
    pkg.filename = "PPSA00001.pkg";
    const auto games = group_games({image, pkg});
    ASSERT_EQ(games.size(), 1u);
    EXPECT_EQ(format_choices(games), (std::vector<std::string>{"FFPFSC", "FPKG"}));
    BrowseFilters filters;
    filters.format = "FPKG";
    EXPECT_EQ(browse(games, filters, {}).size(), 1u);
    filters.source = "archive";
    EXPECT_TRUE(browse(games, filters, {}).empty());
}

TEST(Model, AJobBelongsToAnOptionOnlyWhileItsIdentityMatches)
{
    const Release release = make_release("r", "g", "Game", "", 10);
    Job job = job_for(release, "downloading");
    std::vector<Job> jobs{job};
    EXPECT_NE(release_job(release, jobs), nullptr);
    // A catalogue revision that changes the file must not relabel the saved job.
    jobs[0].total = 11e9;
    EXPECT_EQ(release_job(release, jobs), nullptr);
    jobs[0] = job_for(release, "cancelled");
    EXPECT_EQ(release_job(release, jobs), nullptr);
    // The most urgent state wins.
    jobs = {job_for(release, "complete", "usb0"), job_for(release, "paused", "internal")};
    EXPECT_EQ(release_job(release, jobs)->status, "paused");
    const Game game = group_games({release})[0];
    EXPECT_STREQ(game_state(game, jobs, {}).label, "Paused");
    EXPECT_EQ(game_state(game, std::vector<Job>{}, {}).label, nullptr);
}

TEST(Model, CollectionStatesFollowTheBrowsersRules)
{
    const Release release = make_release("r", "g", "Game", "", 10, "PPSA00009");
    LibraryGame copy;
    copy.title_id = "PPSA00009";
    copy.platform = "ps5";
    copy.on_drive = true;
    copy.format = "FFPFSC";
    copy.path = "/mnt/usb0/homebrew/r.ffpfsc";
    copy.location = "USB 1";
    // An exact copy: same title ID, format and filename, no version.
    CollectionState state = release_state(release, {}, {copy});
    EXPECT_EQ(state.kind, CollectionState::Kind::library);
    EXPECT_STREQ(state.label, "In Library");
    // A different filename is only related.
    copy.path = "/mnt/usb0/homebrew/other.ffpfsc";
    state = release_state(release, {}, {copy});
    EXPECT_EQ(state.kind, CollectionState::Kind::related);
    EXPECT_STREQ(state.label, "Related copy in Library");
    // A versioned option is never an exact match.
    Release versioned = release;
    versioned.version = "1.05";
    copy.path = "/mnt/usb0/homebrew/r.ffpfsc";
    EXPECT_EQ(release_state(versioned, {}, {copy}).kind, CollectionState::Kind::related);
    // An unfinished download outranks the Library; a finished one comes after it.
    EXPECT_STREQ(release_state(release, {job_for(release, "queued")}, {copy}).label, "Queued");
    EXPECT_STREQ(release_state(release, {job_for(release, "complete")}, {copy}).label,
                 "In Library");
    EXPECT_STREQ(release_state(release, {job_for(release, "complete")}, {}).label, "Downloaded");
    // PS4 copies and missing sources don't count.
    copy.platform = "ps4";
    EXPECT_EQ(release_state(release, {}, {copy}).kind, CollectionState::Kind::none);
}

TEST(Model, LibraryAndFavouritesParse)
{
    json::Value value;
    ASSERT_TRUE(json::parse(R"({"status":"ready","provider":"ShadowMount","busy":false,
        "games":[{"titleId":"PPSA00009","title":"Game","platform":"ps5","format":"FFPFSC",
        "path":"/mnt/usb0/g.ffpfsc","sourceKey":"k","location":"USB 1","installed":true,
        "mounted":false,"onDrive":true,"managed":true,"canManageSource":true,
        "installedSizeBytes":null,"sizeBytes":1000,"sizeStatus":"ready"},{"title":"no id"}],
        "capabilities":["list_games","mount_game"],
        "action":{"id":"x","action":"mount","titleId":"PPSA00009","state":"running","message":"Mounting"},
        "storageJob":null})",
                            &value));
    const LibrarySnapshot library = parse_library(value);
    EXPECT_TRUE(library.ready());
    ASSERT_EQ(library.games.size(), 1u);
    EXPECT_EQ(library.games[0].installed_size, -1.0);
    EXPECT_EQ(library.games[0].size, 1000.0);
    EXPECT_TRUE(library.can("mount_game"));
    EXPECT_FALSE(library.can("move_game_source"));
    EXPECT_TRUE(library.working());
    EXPECT_NE(library.game("PPSA00009", "k"), nullptr);
    EXPECT_EQ(library.game("PPSA00009", "other"), nullptr);
    ASSERT_TRUE(json::parse(R"(["a","",7,"b","a"])", &value));
    EXPECT_EQ(parse_favourites(value), (std::vector<std::string>{"a", "b"}));
    ASSERT_TRUE(
        json::parse(R"([{"id":"x","gameId":"g","browserAvailable":true,"directAvailable":false},
        {"id":"y","gameId":"g"}])",
                    &value));
    const std::vector<Release> releases = parse_catalog(value);
    ASSERT_EQ(releases.size(), 2u);
    EXPECT_TRUE(releases[0].browser_only());
    EXPECT_FALSE(releases[1].browser_only());
}

TEST(Model, SpaceIsPlannedAgainstTheDrivesQueue)
{
    const Release release = make_release("r", "g", "Game", "", 10);
    Drive drive;
    drive.id = "usb0";
    drive.projected_free_bytes = 12e9;
    SpacePlan plan = plan_space(release, drive, {});
    EXPECT_DOUBLE_EQ(plan.after, 2e9);
    EXPECT_TRUE(plan.enough);
    EXPECT_FALSE(plan.planned);

    // Already queued on this drive: the projection already counts it.
    plan = plan_space(release, drive, {job_for(release, "queued")});
    EXPECT_TRUE(plan.planned);
    EXPECT_DOUBLE_EQ(plan.after, 12e9);

    // Fresh downloads cannot reuse cancelled progress (it may be deleted).
    Job cancelled = job_for(release, "cancelled");
    cancelled.received = 9e9;
    plan = plan_space(release, drive, {cancelled});
    EXPECT_DOUBLE_EQ(plan.after, 2e9);
    EXPECT_FALSE(plan.planned);
    plan = plan_space(release, drive, {cancelled, job_for(release, "queued")});
    EXPECT_TRUE(plan.planned);
    EXPECT_DOUBLE_EQ(plan.after, 12e9);

    drive.projected_free_bytes = 10e9 + kSpaceMargin - 1.0;
    EXPECT_FALSE(plan_space(release, drive, {}).enough);
}

TEST(Model, PrefersTheConsolesPreferredDriveThenExternal)
{
    std::vector<Drive> drives(3);
    drives[0].id = "internal";
    drives[1].id = "usb0";
    drives[1].external = true;
    drives[2].id = "usb1";
    drives[2].external = true;
    EXPECT_EQ(default_drive(drives, "usb1"), 2);
    EXPECT_EQ(default_drive(drives, "gone"), 1);
    drives[1].external = drives[2].external = false;
    EXPECT_EQ(default_drive(drives, ""), 0);
    EXPECT_EQ(default_drive({}, "usb0"), -1);
}

TEST(Model, JobViewsAndLabelsMatchTheWebStorefront)
{
    Job job;
    for (const auto &[status, view, label] :
         std::vector<std::tuple<const char *, JobView, const char *>>{
             {"downloading", JobView::active, "Downloading"},
             {"queued", JobView::active, "Queued"},
             {"paused", JobView::active, "Paused"},
             {"retrying", JobView::active, "Retrying"},
             {"error", JobView::failed, "Needs attention"},
             {"complete", JobView::finished, "Downloaded"},
             {"cancelled", JobView::cancelled, "Cancelled"}})
    {
        job.status = status;
        EXPECT_EQ(job_view(job), view) << status;
        EXPECT_STREQ(status_label(status), label);
    }
    job.status = "paused";
    EXPECT_FALSE(job_active(job));
    job.status = "verifying";
    EXPECT_TRUE(job_active(job));
}

TEST(Model, FormatsSizesAndDatesLikeTheWebStorefront)
{
    EXPECT_EQ(format_bytes(61.7e9), "61.7 GB");
    EXPECT_EQ(format_bytes(512e6), "512.0 MB");
    EXPECT_EQ(format_bytes(1420e9), "1,420.0 GB"); // grouped, as the browser shows it
    EXPECT_EQ(format_date("2026-07-18"), "Jul 18, 2026");
    EXPECT_EQ(format_date("2026-13-01"), "2026-13-01");
    EXPECT_EQ(format_date("soon"), "soon");
}

TEST(Model, ParsesTolerantlyFromTheApi)
{
    json::Value value;
    ASSERT_TRUE(json::parse(R"([{"id":"j2","order":2,"status":"queued","total":5},
                                {"id":"j1","order":1,"status":"downloading","received":2,"total":5},
                                {"status":"orphan"}])",
                            &value));
    const std::vector<Job> jobs = parse_jobs(value);
    ASSERT_EQ(jobs.size(), 2u);
    EXPECT_EQ(jobs[0].id, "j1");
    ASSERT_TRUE(json::parse(
        R"([{"id":"r","gameId":"g","version":null,"releaseDate":null,"sizeBytes":3},{"id":"x"}])",
        &value));
    const std::vector<Release> releases = parse_catalog(value);
    ASSERT_EQ(releases.size(), 1u);
    EXPECT_EQ(releases[0].version, "");
    ASSERT_TRUE(
        json::parse(R"({"enabled":["archive",5],"acknowledged":true,"noticeVersion":1})", &value));
    const Sources sources = parse_sources(value);
    EXPECT_EQ(sources.enabled, (std::vector<std::string>{"archive"}));
}

TEST(Model, TheAppsVersionComesFromItsContentVersion)
{
    // As the release tooling numbers the TV app (tools/release-artifacts.mjs).
    EXPECT_EQ(app_release_version("01.000.000"), "1.0.0");
    EXPECT_EQ(app_release_version("01.002.010"), "1.2.10");
    EXPECT_EQ(app_release_version("12.345.678"), "12.345.678");
    EXPECT_EQ(app_release_version("1.0.0"), "");
    EXPECT_EQ(app_release_version("01.00a.000"), "");
    EXPECT_EQ(app_release_version("01-000-000"), "");
    EXPECT_EQ(app_release_version(""), "");
}

TEST(Model, AgesAndChecksAreRelativeToNow)
{
    const double now = 1'800'000'000.0;
    EXPECT_EQ(format_age(0.0, now), "never");
    EXPECT_EQ(format_age(now - 20.0, now), "just now");
    EXPECT_EQ(format_age(now + 50.0, now), "just now"); // a clock ahead of the console
    EXPECT_EQ(format_age(now - 25 * 60.0, now), "25 min ago");
    EXPECT_EQ(format_age(now - 3 * 3600.0, now), "3 h ago");
    EXPECT_EQ(format_age(now - 2 * 86400.0, now), "2 days ago");
    EXPECT_TRUE(check_fresh(now - 900.0, now));
    EXPECT_FALSE(check_fresh(now - 901.0, now));
    EXPECT_FALSE(check_fresh(0.0, now));
}

TEST(Model, ParsesSettingsAndUpdateStatuses)
{
    json::Value value;
    ASSERT_TRUE(
        json::parse(R"({"token":"t","pairCode":"482915","address":"192.168.1.20"})", &value));
    const Session session = parse_session(value);
    EXPECT_EQ(session.pair_code, "482915");
    EXPECT_EQ(session.address, "192.168.1.20");
    ASSERT_TRUE(json::parse(R"({"available":true,"busy":false,"revision":10,"gameCount":581,
                                "checkedAt":1800000000,"checkAfter":1800000060,"error":""})",
                            &value));
    const CatalogueStatus catalogue = parse_catalogue_status(value);
    EXPECT_EQ(catalogue.revision, 10);
    EXPECT_EQ(catalogue.game_count, 581);
    EXPECT_EQ(catalogue.check_after, 1800000060.0);
    ASSERT_TRUE(json::parse(R"({"available":true,"updateAvailable":true,"runningVersion":"0.6.0",
                                "savedVersion":"0.6.1","latestVersion":"0.6.1","checksum":"c",
                                "phase":"saved","restartRequired":true,"busy":false})",
                            &value));
    const ServiceUpdate service = parse_service_update(value);
    EXPECT_TRUE(service.update_available);
    EXPECT_TRUE(service.restart_required);
    EXPECT_EQ(service.saved_version, "0.6.1");
    EXPECT_EQ(service.phase, "saved");
    ASSERT_TRUE(json::parse(R"({"available":true,"installed":"manual","installedVersion":"",
                                "latestVersion":"1.1.0","size":71303168,"phase":"checked"})",
                            &value));
    const TvAppStatus tv = parse_tv_app(value);
    EXPECT_EQ(tv.installed, "manual");
    EXPECT_EQ(tv.latest_version, "1.1.0");
    EXPECT_EQ(tv.size, 71303168.0);
    // Older Orbits without these endpoints read as empty.
    ASSERT_TRUE(json::parse(R"({"error":"Unknown endpoint."})", &value));
    EXPECT_FALSE(parse_tv_app(value).available);
    EXPECT_TRUE(parse_session(value).address.empty());
}

} // namespace
} // namespace orbit
