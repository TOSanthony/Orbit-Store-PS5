// Orbit Store TV app - The API client, the synced snapshot and the artwork cache.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "fixture.hpp"
#include "orbit/api.hpp"
#include "orbit/art.hpp"
#include "orbit/store.hpp"
#include "support.hpp"

#include <gtest/gtest.h>

#include <ctime>

namespace orbit
{
namespace
{

using test::json_reply;
using test::ScriptedTransport;

TEST(Api, CarriesTheTokenAndExplainsRefusals)
{
    ScriptedTransport transport;
    transport.reply = [](const http::Request &request)
    {
        if (request.path == "/api/v1/downloads")
            return json_reply(409, R"({"error":"This game's source is turned off."})");
        return json_reply(500, "not json");
    };
    api::Client client(transport);
    client.set_token("tok");
    const auto created = client.create_download("rel-1", "usb0");
    EXPECT_FALSE(created.ok());
    EXPECT_EQ(created.status, 409);
    EXPECT_EQ(created.error, "This game's source is turned off.");
    EXPECT_EQ(transport.seen.back().bearer, "tok");
    EXPECT_EQ(transport.seen.back().body, R"({"releaseId":"rel-1","storageId":"usb0"})");
    EXPECT_EQ(client.system().error, "Orbit could not complete this request.");
}

TEST(Api, NeverPutsUnsafeIdsIntoPaths)
{
    ScriptedTransport transport;
    api::Client client(transport);
    EXPECT_FALSE(client.job_action("../system", "pause").ok());
    EXPECT_FALSE(client.job_action("job1", "pause/../x").ok());
    EXPECT_FALSE(client.art("a/b", api::ArtKind::cover).ok());
    EXPECT_TRUE(transport.seen.empty());
    EXPECT_TRUE(api::safe_path_segment("cyberpunk-2077"));
    EXPECT_FALSE(api::safe_path_segment(".."));
    EXPECT_FALSE(api::safe_path_segment(""));
}

TEST(Api, ArtworkReportsPendingSeparately)
{
    ScriptedTransport transport;
    transport.reply = [](const http::Request &) { return json_reply(202, R"({"pending":true})"); };
    api::Client client(transport);
    const auto art = client.art("game", api::ArtKind::hero);
    EXPECT_TRUE(art.ok());
    EXPECT_TRUE(art.value.pending);
    EXPECT_EQ(transport.seen.back().path, "/api/v1/art/game/hero");
}

TEST(Store, OpensTheConsoleSessionAndLoadsEverything)
{
    host::Fixture fixture(host::FixtureMode::normal);
    Store store(fixture);
    store.poll_once();
    store.tick();
    const Snapshot &state = store.state();
    EXPECT_EQ(state.link, Link::online);
    EXPECT_TRUE(state.paired);
    EXPECT_EQ(state.token.size(), 64u);
    EXPECT_TRUE(state.have_catalog);
    EXPECT_EQ(state.games.size(), 12u);
    EXPECT_TRUE(state.have_queue);
    EXPECT_EQ(state.jobs.size(), 6u);
    EXPECT_EQ(state.drives.size(), 2u);
    EXPECT_FALSE(state.setup_needed());
}

TEST(Store, FetchesTheCatalogueOnlyWhenItChanges)
{
    host::Fixture fixture(host::FixtureMode::normal);
    ScriptedTransport transport;
    transport.reply = [&](const http::Request &request) { return fixture.send(request); };
    Store store(transport);
    store.poll_once();
    store.poll_once();
    store.poll_once();
    EXPECT_EQ(transport.count("/api/v1/catalog"), 1);
    EXPECT_EQ(transport.count("/api/v1/session"), 1);
    EXPECT_EQ(transport.count("/api/v1/downloads"), 3);
}

TEST(Store, ReportsOfflineAndSetup)
{
    host::Fixture offline(host::FixtureMode::offline);
    Store lost(offline);
    lost.poll_once();
    lost.tick();
    EXPECT_EQ(lost.state().link, Link::offline);
    EXPECT_EQ(lost.state().link_error, "connection refused");

    host::Fixture setup(host::FixtureMode::setup);
    Store first_run(setup);
    first_run.poll_once();
    first_run.tick();
    EXPECT_TRUE(first_run.state().setup_needed());
    EXPECT_TRUE(first_run.state().games.empty());
}

TEST(Store, CarriesOutActionsInOrderAndRefreshesTheQueue)
{
    host::Fixture fixture(host::FixtureMode::normal);
    Store store(fixture);
    store.poll_once();
    store.tick();
    store.download("neon-courier-ffpfsc", "usb0");
    store.job_action(store.state().jobs[0].id, "pause", false);
    EXPECT_TRUE(store.action().busy);
    EXPECT_TRUE(store.run_next_action());
    store.tick();
    EXPECT_TRUE(store.action().busy); // one more to go
    EXPECT_FALSE(store.action().created_job.empty());
    EXPECT_TRUE(store.run_next_action());
    EXPECT_FALSE(store.run_next_action());
    store.tick();
    EXPECT_FALSE(store.action().busy);
    EXPECT_EQ(store.action().error, "");
    EXPECT_EQ(store.state().jobs.size(), 7u);
    EXPECT_EQ(store.state().jobs[0].status, "paused");
}

TEST(Store, ShowsTheBackendsReasonWhenAnActionFails)
{
    host::Fixture fixture(host::FixtureMode::normal);
    Store store(fixture);
    store.poll_once();
    store.tick();
    store.download("no-such-release", "usb0");
    store.run_next_action();
    store.tick();
    EXPECT_EQ(store.action().error, "Unknown release.");
}

TEST(Art, WaitsForTheSessionThenDecodesAndUploads)
{
    host::Fixture fixture(host::FixtureMode::normal);
    test::FakeSink sink;
    ArtCache art(fixture, sink);
    EXPECT_EQ(art.get("tidewater", api::ArtKind::cover), nullptr);
    EXPECT_FALSE(art.work_once()); // nothing is asked for without a session
    art.set_token("0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef");
    EXPECT_EQ(art.get("tidewater", api::ArtKind::cover), nullptr);
    EXPECT_TRUE(art.work_once());
    art.tick();
    const Artwork *cover = art.get("tidewater", api::ArtKind::cover);
    ASSERT_NE(cover, nullptr);
    EXPECT_EQ(cover->width, 400); // decoded at drawing size, not the source's 512
    EXPECT_NE(cover->ambient, 0u);
    EXPECT_EQ(sink.live.size(), 2u);
    art.clear();
    EXPECT_TRUE(sink.live.empty());
}

TEST(Art, AsksAgainWhileTheBackendIsFetching)
{
    host::Fixture fixture(host::FixtureMode::normal);
    ScriptedTransport transport;
    int pending = 2;
    transport.reply = [&](const http::Request &request)
    {
        if (pending-- > 0)
            return json_reply(202, R"({"pending":true})");
        return fixture.send(request);
    };
    test::FakeSink sink;
    ArtCache art(transport, sink);
    art.set_token("0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef");
    art.get("frostline", api::ArtKind::hero);
    EXPECT_TRUE(art.work_once());
    EXPECT_FALSE(art.idle());
    for (int i = 0; i < 400 && art.get("frostline", api::ArtKind::hero) == nullptr; ++i)
    {
        art.work_once();
        art.tick();
        timespec pause{0, 5000000}; // pending requests wait a moment before they are due
        nanosleep(&pause, nullptr);
    }
    const Artwork *hero = art.get("frostline", api::ArtKind::hero);
    ASSERT_NE(hero, nullptr);
    EXPECT_GT(hero->width, hero->height);
    EXPECT_EQ(transport.count("/api/v1/art/frostline/hero"), 3);
}

TEST(Art, AsksAgainWhileTheArtworkWorkersAreStarting)
{
    // Orbit answers before its artwork workers start, as when the app has
    // just started it: a 503 is asked again shortly, not held as a failure.
    host::Fixture fixture(host::FixtureMode::normal);
    ScriptedTransport transport;
    int busy = 1;
    transport.reply = [&](const http::Request &request)
    {
        if (busy-- > 0)
            return json_reply(503, R"({"error":"Artwork is busy. Try again shortly."})");
        return fixture.send(request);
    };
    test::FakeSink sink;
    ArtCache art(transport, sink);
    art.set_token("0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef");
    art.get("tidewater", api::ArtKind::cover);
    for (int i = 0; i < 400 && art.get("tidewater", api::ArtKind::cover) == nullptr; ++i)
    {
        art.work_once();
        art.tick();
        timespec pause{0, 5000000};
        nanosleep(&pause, nullptr);
    }
    ASSERT_NE(art.get("tidewater", api::ArtKind::cover), nullptr);
    EXPECT_EQ(transport.count("/api/v1/art/tidewater/cover"), 2);
}

TEST(Art, MarksFailuresAndEvictsTheLeastRecentlyDrawn)
{
    ScriptedTransport transport;
    host::Fixture fixture(host::FixtureMode::normal);
    transport.reply = [&](const http::Request &request)
    {
        if (request.path.find("broken") != std::string::npos)
            return json_reply(502, R"({"error":"This artwork could not be loaded."})");
        return fixture.send(request);
    };
    test::FakeSink sink;
    ArtLimits limits;
    limits.max_covers = 2;
    ArtCache art(transport, sink, limits);
    art.set_token("0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef");
    art.get("broken", api::ArtKind::cover);
    art.work_once();
    art.tick();
    EXPECT_EQ(art.get("broken", api::ArtKind::cover), nullptr);
    EXPECT_FALSE(art.work_once()); // failed: not asked again for a while

    for (const char *game : {"tidewater", "frostline", "echo-valley"})
    {
        art.get(game, api::ArtKind::cover);
        art.work_once();
    }
    art.tick();
    art.tick(); // two uploads per frame
    // Eviction never takes what was drawn in the last half second.
    EXPECT_EQ(sink.live.size(), 6u);
}

// ---- settings ----

TEST(StoreSettings, SavesSourcesWithTheNoticeAccepted)
{
    host::Fixture fixture(host::FixtureMode::setup);
    ScriptedTransport transport;
    transport.reply = [&](const http::Request &request) { return fixture.send(request); };
    Store store(transport);
    store.poll_once();
    store.tick();
    ASSERT_TRUE(store.state().setup_needed());
    store.save_sources({"archive"}, 1);
    EXPECT_TRUE(store.setting_result().busy);
    store.run_next_action();
    store.tick();
    EXPECT_FALSE(store.setting_result().busy);
    EXPECT_TRUE(store.setting_result().error.empty());
    EXPECT_EQ(store.setting_result().setting, Setting::sources);
    bool sent = false;
    for (const http::Request &request : transport.seen)
    {
        if (request.method == "POST" && request.path == "/api/v1/sources")
        {
            sent = true;
            EXPECT_EQ(request.body,
                      R"({"enabled":["archive"],"acknowledged":true,"noticeVersion":1})");
        }
    }
    EXPECT_TRUE(sent);
    EXPECT_FALSE(store.state().setup_needed());
    // The next poll sees new source settings and loads the catalogue.
    store.poll_once();
    store.tick();
    EXPECT_EQ(store.state().games.size(), 12u);
}

TEST(StoreSettings, SavesTheDefaultDriveAndExplainsAnOlderOrbit)
{
    host::Fixture fixture(host::FixtureMode::normal);
    Store store(fixture);
    store.poll_once();
    store.tick();
    EXPECT_EQ(store.state().system.preferred_storage, "usb0");
    store.set_preferred_storage("internal");
    store.run_next_action();
    store.tick();
    EXPECT_EQ(store.state().system.preferred_storage, "internal");
    EXPECT_EQ(store.setting_result().setting, Setting::storage);

    ScriptedTransport old;
    old.reply = [&](const http::Request &request)
    {
        if (request.path == "/api/v1/storage/preferred")
            return json_reply(404, R"({"error":"Unknown endpoint."})");
        return fixture.send(request);
    };
    Store older(old);
    older.poll_once();
    older.set_preferred_storage("usb0");
    older.run_next_action();
    older.tick();
    EXPECT_EQ(older.setting_result().error,
              "Update the download service to choose a default drive here.");
}

TEST(StoreSettings, LooksForUpdatesOnceAndReadsThePairingCode)
{
    host::Fixture fixture(host::FixtureMode::updates);
    ScriptedTransport transport;
    transport.reply = [&](const http::Request &request) { return fixture.send(request); };
    Store store(transport);
    store.poll_once();
    store.poll_once();
    store.poll_once();
    store.tick();
    int automatic = 0;
    for (const http::Request &request : transport.seen)
    {
        if (request.method == "POST" &&
            (request.path == "/api/v1/updates" || request.path == "/api/v1/tv-app"))
        {
            EXPECT_EQ(request.body, R"({"action":"check","automatic":true})");
            ++automatic;
        }
    }
    EXPECT_EQ(automatic, 2); // once each, not on every poll
    const Snapshot &state = store.state();
    ASSERT_TRUE(state.have_service_update);
    EXPECT_TRUE(state.service.update_available);
    EXPECT_EQ(state.service.latest_version, "0.6.1");
    ASSERT_TRUE(state.have_tv_app);
    EXPECT_EQ(state.tv_app.latest_version, "1.1.0");
    EXPECT_TRUE(state.have_catalogue_status);
    // The session that opened the app also gave the pairing code and address.
    EXPECT_EQ(state.pair_code, "482915");
    EXPECT_EQ(state.address, "192.168.1.20");
    // Settings closed: the statuses are read about once a minute, not every poll.
    const int reads = transport.count("/api/v1/updates");
    store.poll_once();
    EXPECT_EQ(transport.count("/api/v1/updates"), reads);
    store.want_settings(true);
    store.poll_once();
    EXPECT_EQ(transport.count("/api/v1/updates"), reads + 1);
}

TEST(StoreSettings, InstallsTheCheckedReleases)
{
    host::Fixture fixture(host::FixtureMode::updates);
    ScriptedTransport transport;
    transport.reply = [&](const http::Request &request) { return fixture.send(request); };
    Store store(transport);
    store.poll_once();
    store.tick();
    store.install_tv_app("1.1.0", "abc", true);
    store.run_next_action();
    store.install_service_update("0.6.1", "def");
    store.run_next_action();
    store.tick();
    std::vector<std::string> bodies;
    for (const http::Request &request : transport.seen)
    {
        if (request.method == "POST" && request.body.find("install") != std::string::npos)
            bodies.push_back(request.path + " " + request.body);
    }
    ASSERT_EQ(bodies.size(), 2u);
    EXPECT_EQ(
        bodies[0],
        R"(/api/v1/tv-app {"action":"install","version":"1.1.0","checksum":"abc","replace":true})");
    EXPECT_EQ(bodies[1],
              R"(/api/v1/updates {"action":"install","version":"0.6.1","checksum":"def"})");
    EXPECT_EQ(store.state().tv_app.installed_version, "1.1.0");
    EXPECT_TRUE(store.state().service.restart_required);
    EXPECT_EQ(store.state().service.saved_version, "0.6.1");
}

} // namespace
} // namespace orbit
