// SPDX-License-Identifier: GPL-3.0-or-later
#include "fixture.hpp"
#include "orbit/screens.hpp"
#include "support.hpp"
#include <cstdio>
#include <gtest/gtest.h>

namespace orbit
{
namespace
{
struct DiscoverHarness
{
    test::TestFonts fonts;
    host::Fixture backend{host::FixtureMode::normal};
    test::ScriptedTransport transport;
    test::FakeSink sink;
    Store store{transport};
    ArtCache art{transport, sink};
    Context context{fonts.type, fonts.fonts, store, art};
    DiscoverScreen screen{context};
    Feedback feedback;
    int count;
    int revision = 1;
    bool dated;
    bool additions = false;
    std::vector<int> sizes;
    explicit DiscoverHarness(int size = 55, bool dates = true) : count(size), dated(dates)
    {
        transport.reply = [this](const http::Request &request)
        {
            if (request.path == "/api/v1/catalog")
            {
                std::string catalogue = "[";
                for (int i = 0; i < count; ++i)
                {
                    char id[24];
                    std::snprintf(id, sizeof(id), "game-%02d", i);
                    if (i > 0)
                        catalogue += ',';
                    catalogue += "{\"id\":\"" + std::string(id) + "-r\",\"gameId\":\"" + id +
                                 "\",\"title\":\"" + id + "\",\"sourceId\":\"archive\","
                                 "\"provider\":\"Archive.org\",\"format\":\"FFPFSC\",\"sizeBytes\":" +
                                 std::to_string(i < static_cast<int>(sizes.size()) ? sizes[static_cast<std::size_t>(i)] : 1000) +
                                 ",\"addedAt\":\"" + (additions ? "2026-10-05T12:00:00Z" : std::string()) + "\",\"releaseDate\":\"" + (dated ? "2026-09-12" : std::string()) + "\"}";
                }
                return test::json_reply(200, catalogue + ']');
            }
            http::Response response = backend.send(request);
            if (request.path == "/api/v1/system")
            {
                const std::string old = "\"catalogueRevision\":7";
                const auto at = response.body.find(old);
                if (at != std::string::npos)
                    response.body.replace(at, old.size(), "\"catalogueRevision\":" + std::to_string(revision));
            }
            return response;
        };
        context.today = "2026-10-06";
        refresh();
    }
    void refresh()
    {
        ++revision;
        store.poll_once();
        store.tick();
        screen.focus();
        input();
    }
    Intent input(Direction nav = Direction::none, bool confirm = false)
    {
        InputFrame input;
        input.connected = true;
        input.nav = nav;
        if (confirm)
            input.pressed = hui::action_bit(Action::confirm);
        Intent result = screen.update(input, 1.0f / 60.0f, feedback, true);
        Frame frame;
        screen.draw(frame, true);
        return result;
    }
    void all_games()
    {
        input(Direction::down);
        if (dated)
            input(Direction::down);
    }
};

TEST(Discover, NewOnOrbitIsASeparateNavigableRailBeforeTheGrid)
{
    DiscoverHarness h;
    h.additions = true;
    h.refresh();
    h.input(Direction::down); // latest
    h.input(Direction::down); // New on Orbit
    h.input(Direction::right);
    EXPECT_EQ(h.input(Direction::none, true).id, "game-01");
    h.screen.return_to("game-01");
    EXPECT_EQ(h.input(Direction::none, true).id, "game-01");
    h.input(Direction::down); // all-games grid
    h.input(Direction::down); // next grid row
    EXPECT_EQ(h.input(Direction::none, true).id, "game-06");
    h.input(Direction::up);
    h.input(Direction::up); // remembers selection in New on Orbit
    EXPECT_EQ(h.input(Direction::none, true).id, "game-01");
}

TEST(Discover, GridMovesVerticallyAndReturnsToTheSelectedGame)
{
    DiscoverHarness h;
    h.all_games();
    h.input(Direction::right);
    h.input(Direction::down);
    EXPECT_EQ(h.input(Direction::none, true).id, "game-07");
    h.screen.return_to("game-07");
    EXPECT_EQ(h.input(Direction::none, true).id, "game-07");
    h.input(Direction::up);
    EXPECT_EQ(h.input(Direction::none, true).id, "game-01");
    for (int i = 0; i < 10; ++i)
        h.input(Direction::right);
    EXPECT_EQ(h.input(Direction::none, true).id, "game-05"); // no wrap to next row
    h.input(Direction::up);
    EXPECT_EQ(h.input(Direction::none, true).id, "game-00"); // latest rail
}

TEST(Discover, PageControlsReachTheWholeCatalogueWithoutOpeningAGame)
{
    DiscoverHarness h(99);
    h.all_games();
    for (int i = 0; i < 16; ++i)
        h.input(Direction::down);
    EXPECT_NE(h.input(Direction::none, true).kind, Intent::Kind::open_game);
    EXPECT_EQ(h.input(Direction::none, true).id, "game-96");
    h.input(Direction::right);
    h.input(Direction::right);
    EXPECT_EQ(h.input(Direction::none, true).id, "game-98");
    h.input(Direction::down); // last row leads to Previous on the final page
    EXPECT_NE(h.input(Direction::none, true).kind, Intent::Kind::open_game);
    EXPECT_EQ(h.input(Direction::none, true).id, "game-00");
}

TEST(Discover, CatalogueShrinkAndUndatedGamesKeepNavigationInBounds)
{
    DiscoverHarness h;
    h.all_games();
    for (int i = 0; i < 8; ++i)
        h.input(Direction::down);
    h.input(Direction::none, true);
    h.count = 1;
    h.dated = false;
    h.refresh();
    EXPECT_EQ(h.input(Direction::none, true).id, "game-00");
    h.input(Direction::down);
    EXPECT_EQ(h.input(Direction::none, true).id, "game-00");
    h.count = 0;
    h.refresh();
    EXPECT_NE(h.input(Direction::none, true).kind, Intent::Kind::open_game);
    EXPECT_EQ(h.input(Direction::up).kind, Intent::Kind::top_bar);
}

TEST(Discover, AllGamesUsesLargestDownloadFirstWithoutChangingTheLatestRail)
{
    DiscoverHarness h(3);
    h.sizes = {3000, 9000, 1000};
    h.refresh();
    h.input(Direction::down);
    EXPECT_EQ(h.input(Direction::none, true).id, "game-00");
    h.input(Direction::down);
    EXPECT_EQ(h.input(Direction::none, true).id, "game-01");
    h.input(Direction::right);
    EXPECT_EQ(h.input(Direction::none, true).id, "game-00");
    h.input(Direction::right);
    EXPECT_EQ(h.input(Direction::none, true).id, "game-02");
}
} // namespace
} // namespace orbit
