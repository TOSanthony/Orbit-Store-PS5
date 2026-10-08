// SPDX-License-Identifier: GPL-3.0-or-later
#include "fixture.hpp"
#include "orbit/screens.hpp"
#include "support.hpp"
#include <gtest/gtest.h>

namespace orbit
{
namespace
{
constexpr const char *connected =
    R"({"provider":"torbox","available":true,"connected":true,"busy":false,"hosts":[{"sourceId":"archive","available":true,"maxFileSize":107374182400}]})";

TEST(Debrid, AvailabilityRequiresTheSelectedHostAndItsFileLimit)
{
    json::Value value;
    ASSERT_TRUE(json::parse(connected, &value));
    Debrid status = parse_debrid(value);
    Release release;
    release.source_id = "archive";
    release.size_bytes = 107374182400.0;
    EXPECT_TRUE(status.supports(release));
    release.size_bytes += 1;
    EXPECT_FALSE(status.supports(release));
    release.size_bytes = 1000;
    release.source_id = "vikingfile";
    EXPECT_FALSE(status.supports(release));
    release.source_id = "archive";
    status.busy = true;
    EXPECT_FALSE(status.supports(release));
    status.busy = false;
    status.connected = false;
    EXPECT_FALSE(status.supports(release));
}

TEST(Debrid, ApiSubmitsDeliveryOnlyAfterAnExplicitChoice)
{
    test::ScriptedTransport transport;
    transport.reply = [](const http::Request &)
    { return test::json_reply(201, R"({"id":"job1"})"); };
    api::Client client(transport);
    ASSERT_TRUE(client.create_download("release", "usb0").ok());
    EXPECT_EQ(transport.seen.back().body, R"({"releaseId":"release","storageId":"usb0"})");
    ASSERT_TRUE(client.create_download("release", "usb0", true).ok());
    EXPECT_EQ(transport.seen.back().body,
              R"({"releaseId":"release","storageId":"usb0","delivery":"torbox"})");
}

struct DebridHarness
{
    test::TestFonts fonts;
    host::Fixture backend{host::FixtureMode::normal};
    test::ScriptedTransport transport;
    test::FakeSink sink;
    Store store{transport};
    ArtCache art{transport, sink};
    Context context{fonts.type, fonts.fonts, store, art};
    GamePage page{context};
    Feedback feedback;
    bool linked = true;
    DebridHarness()
    {
        transport.reply = [this](const http::Request &request)
        {
            if (request.path == "/api/v1/debrid")
                return test::json_reply(
                    200, linked ? connected
                                : R"({"provider":"torbox","available":true,"connected":false})");
            return backend.send(request);
        };
        context.today = "2026-10-05";
        store.poll_once();
        store.tick();
        page.open("neon-courier");
    }
    void input(Direction nav = Direction::none, bool confirm = false)
    {
        InputFrame frame;
        frame.connected = true;
        frame.nav = nav;
        if (confirm)
            frame.pressed = hui::action_bit(Action::confirm);
        page.update(frame, 1.0f / 60.0f, feedback, true);
        Frame drawn;
        page.draw(drawn, true);
    }
    void choose_torbox()
    {
        input(Direction::none,true); // Review
        EXPECT_FALSE(store.action().busy);
        input(Direction::up); // Save to
        input(Direction::left); // Delivery
        input(Direction::none,true);
        ASSERT_TRUE(page.modal());
        input(Direction::down); // TorBox
        input(Direction::none,true); // Returns to Delivery setting
        EXPECT_TRUE(page.modal());
        input(Direction::down); // Primary

    }
};

TEST(Debrid, ControllerChoosesTorboxAndQueuesOnTheSelectedDrive)
{
    DebridHarness h;
    h.choose_torbox();
    h.input(Direction::none, true);
    ASSERT_TRUE(h.store.action().busy);
    ASSERT_TRUE(h.store.run_next_action());
    bool submitted = false;
    for (const auto &request : h.transport.seen)
    {
        if (request.method == "POST" && request.path == "/api/v1/downloads")
        {
            json::Value value;
            ASSERT_TRUE(json::parse(request.body, &value));
            EXPECT_EQ(value.text("delivery"), "torbox");
            EXPECT_EQ(value.text("releaseId"), "neon-courier-ffpfsc");
            EXPECT_FALSE(value.text("storageId").empty());
            submitted = true;
        }
    }
    EXPECT_TRUE(submitted);
}

TEST(Debrid, SetupOpensTheBrowserWithTheSelectionWithoutQueuing)
{
    DebridHarness h;
    h.linked = false;
    h.store.poll_once();
    h.store.tick();
    int opened = 0;
    h.context.open_download = [&](const BrowserSelection &selection) {
        EXPECT_EQ(selection.game_id, "neon-courier");
        EXPECT_EQ(selection.release_id, "neon-courier-ffpfsc");
        EXPECT_EQ(selection.storage_id, "usb0");
        ++opened;
        return true;
    };
    h.choose_torbox();
    EXPECT_EQ(opened, 1);
    EXPECT_TRUE(h.page.modal());
    EXPECT_FALSE(h.store.action().busy);
    EXPECT_FALSE(h.store.run_next_action());
    for (const auto &request : h.transport.seen)
        EXPECT_FALSE(request.method == "POST" && request.path == "/api/v1/downloads");
}

TEST(Debrid, DisconnectedAccountDoesNotSilentlySwitchTheDownloadToDirect)
{
    DebridHarness h;
    h.choose_torbox();
    h.linked = false;
    h.store.poll_once();
    h.store.tick();
    h.input(Direction::none, true);
    EXPECT_FALSE(h.store.action().busy);
    EXPECT_FALSE(h.store.run_next_action());
}
} // namespace
} // namespace orbit
