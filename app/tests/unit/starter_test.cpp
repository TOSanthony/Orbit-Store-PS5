// Orbit Store TV app - Starting Orbit through the ELF loader.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "fixture.hpp"
#include "orbit/screens.hpp"
#include "orbit/starter.hpp"
#include "orbit/store.hpp"
#include "support.hpp"

#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cstdlib>
#include <fstream>
#include <string>

namespace orbit
{
namespace
{

using starter::Outcome;

std::string elf_bytes(std::size_t size, char fill)
{
    std::string bytes(size, fill);
    const char header[] = "\177ELF\2\1\1";
    bytes.replace(0, 7, header, 7);
    bytes[16] = 2;
    bytes[17] = 0;
    bytes[18] = 62;
    bytes[19] = 0;
    return bytes;
}

void write(const std::string &path, const std::string &bytes)
{
    std::ofstream out(path, std::ios::binary);
    out << bytes;
}

struct TempDir
{
    std::string path;
    TempDir()
    {
        char name[] = "/tmp/orbit-starter-XXXXXX";
        path = mkdtemp(name);
    }
    ~TempDir()
    {
        std::system(("rm -rf '" + path + "'").c_str());
    }
    starter::Locations locations() const
    {
        starter::Locations where;
        where.saved_elf = path + "/saved.elf";
        where.saved_version = path + "/saved-version.txt";
        where.bundled_elf = path + "/bundled.elf";
        where.bundled_version = path + "/version.txt";
        return where;
    }
};

// A one-connection stand-in for the ELF loader: keeps what it was sent.
class Loader
{
  public:
    Loader()
    {
        fd_ = socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        bind(fd_, reinterpret_cast<sockaddr *>(&address), sizeof(address));
        socklen_t length = sizeof(address);
        getsockname(fd_, reinterpret_cast<sockaddr *>(&address), &length);
        port = ntohs(address.sin_port);
        listen(fd_, 1);
        thread_.start(
            [this]
            {
                const int client = accept(fd_, nullptr, nullptr);
                char buffer[65536];
                for (;;)
                {
                    const ssize_t n = recv(client, buffer, sizeof(buffer), 0);
                    if (n <= 0)
                        break;
                    received.append(buffer, static_cast<std::size_t>(n));
                }
                close(client);
            });
    }
    ~Loader()
    {
        thread_.join();
        close(fd_);
    }
    std::string take()
    {
        thread_.join();
        return received;
    }

    std::uint16_t port = 0;

  private:
    int fd_ = -1;
    std::string received;
    Thread thread_;
};

// A port on which nothing listens.
std::uint16_t closed_port()
{
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    bind(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address));
    socklen_t length = sizeof(address);
    getsockname(fd, reinterpret_cast<sockaddr *>(&address), &length);
    close(fd);
    return ntohs(address.sin_port);
}

TEST(Starter, OrdersReleaseVersionsAsOrbitDoes)
{
    using starter::compare_versions;
    EXPECT_EQ(compare_versions("0.4.2", "0.4.2"), 0);
    EXPECT_EQ(compare_versions("v0.4.2", "0.4.2"), 0);
    EXPECT_EQ(compare_versions("0.4.10", "0.4.9"), 1);
    EXPECT_EQ(compare_versions("0.5.0", "0.4.99"), 1);
    EXPECT_EQ(compare_versions("0.5.0-beta.2", "0.5.0"), -1);
    EXPECT_EQ(compare_versions("0.5.0-beta.10", "0.5.0-beta.9"), 1);
    EXPECT_EQ(compare_versions("", "0.0.1"), -1);
    EXPECT_EQ(compare_versions("0.0.1", "1.2"), 1);
    EXPECT_EQ(compare_versions("nonsense", ""), 0);
    EXPECT_EQ(compare_versions("1.2.3-rc.1", "0.0.1"), -1);
}

TEST(Starter, PrefersTheSavedCopyUnlessThePackageIsNewer)
{
    TempDir dir;
    const starter::Locations where = dir.locations();
    EXPECT_TRUE(starter::choose(where).path.empty());

    write(where.bundled_elf, elf_bytes(4096, 'b'));
    write(where.bundled_version, "0.4.2\n");
    starter::Payload payload = starter::choose(where);
    EXPECT_EQ(payload.path, where.bundled_elf);
    EXPECT_STREQ(payload.source, "bundled");
    EXPECT_EQ(payload.version, "0.4.2");
    EXPECT_EQ(payload.bytes, 4096);

    write(where.saved_elf, elf_bytes(2048, 's'));
    write(where.saved_version, "0.4.2");
    EXPECT_EQ(starter::choose(where).path, where.saved_elf); // a tie keeps the saved copy

    write(where.saved_version, "0.5.0");
    EXPECT_EQ(starter::choose(where).path, where.saved_elf);

    write(where.saved_version, "0.4.1");
    EXPECT_EQ(starter::choose(where).path, where.bundled_elf);

    std::remove(where.saved_version.c_str()); // an unknown saved release counts as older
    EXPECT_EQ(starter::choose(where).path, where.bundled_elf);

    write(where.bundled_elf, "#!/bin/sh\n" + std::string(100, 'x')); // not an ELF
    EXPECT_EQ(starter::choose(where).path, where.saved_elf);

    write(where.saved_elf, elf_bytes(32, 's')); // too short to be a payload
    EXPECT_TRUE(starter::choose(where).path.empty());
}

TEST(Starter, StreamsThePayloadToTheLoader)
{
    TempDir dir;
    const std::string path = dir.path + "/orbit.elf";
    const std::string bytes = elf_bytes(300000, 'o');
    write(path, bytes);
    Loader loader;
    std::string error;
    EXPECT_EQ(starter::send(path, loader.port, 2000, &error), Outcome::sent) << error;
    EXPECT_EQ(loader.take(), bytes);
}

TEST(Starter, ReportsAMissingLoaderAndAMissingFile)
{
    TempDir dir;
    const std::string path = dir.path + "/orbit.elf";
    write(path, elf_bytes(4096, 'o'));
    std::string error;
    EXPECT_EQ(starter::send(path, closed_port(), 2000, &error), Outcome::no_loader);
    EXPECT_NE(error.find("No ELF loader"), std::string::npos);
    EXPECT_EQ(starter::send(dir.path + "/missing.elf", closed_port(), 2000, &error),
              Outcome::no_payload);
}

TEST(Starter, LoaderStarterSendsTheChosenCopyAndLogsIt)
{
    TempDir dir;
    const starter::Locations where = dir.locations();
    std::vector<std::string> lines;
    {
        starter::LoaderStarter nothing(where, closed_port(),
                                       [&](const std::string &line) { lines.push_back(line); });
        const starter::Result result = nothing.start();
        EXPECT_EQ(result.outcome, Outcome::no_payload);
        ASSERT_EQ(lines.size(), 1u);
        EXPECT_NE(lines[0].find("no payload"), std::string::npos);
    }
    const std::string bytes = elf_bytes(8192, 'b');
    write(where.bundled_elf, bytes);
    write(where.bundled_version, "0.4.2");
    Loader loader;
    starter::LoaderStarter starter(where, loader.port,
                                   [&](const std::string &line) { lines.push_back(line); });
    const starter::Result result = starter.start();
    EXPECT_EQ(result.outcome, Outcome::sent);
    EXPECT_NE(result.detail.find("0.4.2"), std::string::npos);
    EXPECT_EQ(loader.take(), bytes);
    ASSERT_EQ(lines.size(), 2u);
    EXPECT_NE(lines[1].find("copy=bundled version=0.4.2 bytes=8192"), std::string::npos);
}

// ---- the store's rules for starting Orbit ----

class CountingStarter final : public starter::Starter
{
  public:
    Outcome outcome = Outcome::sent;
    std::atomic<int> calls{0};
    starter::Result start() override
    {
        ++calls;
        starter::Result result;
        result.outcome = outcome;
        result.detail = "detail";
        return result;
    }
};

// Orbit refusing connections until `online` is set, then the fixture backend.
// Stop Orbit is accepted here; the test decides when Orbit goes away.
struct Console
{
    host::Fixture fixture{host::FixtureMode::normal};
    test::ScriptedTransport transport;
    std::atomic<bool> online{false};
    std::string failure = "connection refused";
    Console()
    {
        transport.reply = [this](const http::Request &request)
        {
            if (online && request.method == "POST" && request.path == "/api/v1/updates")
                return test::json_reply(202, R"({"phase":"stopping"})");
            if (online)
                return fixture.send(request);
            http::Response response;
            response.error = failure;
            return response;
        };
    }
};

TEST(StoreStart, StartsOrbitOnceWhenNothingAnswersThenClearsWhenItDoes)
{
    Console console;
    CountingStarter starter;
    Store store(console.transport, &starter);
    EXPECT_TRUE(store.can_start());
    store.poll_once();
    store.tick();
    EXPECT_EQ(starter.calls, 1);
    EXPECT_EQ(store.state().starting, Starting::waiting);
    EXPECT_EQ(store.state().start_detail, "detail");
    store.poll_once(); // still starting: no second payload
    EXPECT_EQ(starter.calls, 1);
    console.online = true;
    store.poll_once();
    store.tick();
    EXPECT_EQ(store.state().link, Link::online);
    EXPECT_EQ(store.state().starting, Starting::idle);
    EXPECT_TRUE(store.state().start_detail.empty());
    // A later loss of Orbit is not answered with another payload by itself.
    console.online = false;
    store.poll_once();
    store.tick();
    EXPECT_EQ(starter.calls, 1);
    EXPECT_EQ(store.state().link, Link::offline);
}

TEST(StoreStart, ReportsAMissingLoaderAndRetriesOnlyWhenAsked)
{
    Console console;
    CountingStarter starter;
    starter.outcome = Outcome::no_loader;
    Store store(console.transport, &starter);
    store.poll_once();
    store.poll_once();
    store.tick();
    EXPECT_EQ(starter.calls, 1);
    EXPECT_EQ(store.state().starting, Starting::no_loader);
    store.start_orbit();
    store.poll_once();
    store.tick();
    EXPECT_EQ(starter.calls, 2);
    starter.outcome = Outcome::no_payload;
    store.start_orbit();
    store.poll_once();
    store.tick();
    EXPECT_EQ(store.state().starting, Starting::no_payload);
}

TEST(StoreStart, LeavesASlowOrbitAloneAndWorksWithoutAStarter)
{
    Console console;
    console.failure = "connect failed"; // something answers, slowly: not a stopped Orbit
    CountingStarter starter;
    Store store(console.transport, &starter);
    store.poll_once();
    store.tick();
    EXPECT_EQ(starter.calls, 0);
    EXPECT_EQ(store.state().starting, Starting::idle);

    Console plain;
    Store waiting(plain.transport);
    EXPECT_FALSE(waiting.can_start());
    waiting.start_orbit();
    waiting.poll_once();
    waiting.tick();
    EXPECT_EQ(waiting.state().starting, Starting::idle);
    EXPECT_EQ(waiting.state().link, Link::offline);
}

TEST(StoreStart, CrossOnTheOfflineScreenStartsOrbitAgain)
{
    Console console;
    CountingStarter starter;
    starter.outcome = Outcome::no_loader;
    test::TestFonts fonts;
    test::FakeSink sink;
    Store store(console.transport, &starter);
    ArtCache art(console.fixture, sink);
    Context context(fonts.type, fonts.fonts, store, art);
    App app(context);
    Feedback feedback;
    store.poll_once();
    const auto frame = [&](std::uint32_t press)
    {
        InputFrame input;
        input.connected = true;
        input.pressed = press;
        feedback.clear();
        app.update(input, 1.0f / 60.0f, feedback);
        Frame drawn;
        app.draw(drawn);
    };
    frame(0);
    EXPECT_EQ(store.state().starting, Starting::no_loader);
    frame(hui::action_bit(Action::confirm));
    EXPECT_FALSE(feedback.cues.empty());
    store.poll_once();
    EXPECT_EQ(starter.calls, 2);
}

// ---- restarting the download service from Settings ----

TEST(StoreRestart, StopsOrbitThenStartsTheSavedCopyAndReportsItsVersion)
{
    Console console;
    console.online = true;
    CountingStarter starter;
    Store store(console.transport, &starter);
    store.set_restart_grace(0.0);
    store.poll_once();
    store.tick();
    ASSERT_EQ(store.state().link, Link::online);
    store.restart_service();
    store.run_next_action();
    store.tick();
    EXPECT_EQ(store.state().restart, Restart::stopping);
    int stops = 0;
    for (const http::Request &request : console.transport.seen)
        stops += request.body == R"({"action":"stop","confirmed":true})" ? 1 : 0;
    EXPECT_EQ(stops, 1);
    // Orbit answers for a moment, then its port closes.
    store.poll_once();
    store.tick();
    EXPECT_EQ(store.state().restart, Restart::stopping);
    console.online = false;
    store.poll_once(); // stopped
    store.tick();
    EXPECT_EQ(store.state().restart, Restart::starting);
    EXPECT_EQ(starter.calls, 0);
    store.poll_once(); // sends the saved copy
    store.tick();
    EXPECT_EQ(starter.calls, 1);
    EXPECT_EQ(store.state().restart, Restart::starting);
    store.poll_once(); // waiting for it to answer: not sent again yet
    EXPECT_EQ(starter.calls, 1);
    console.online = true;
    const int sessions = console.transport.count("/api/v1/session");
    store.poll_once();
    store.tick();
    EXPECT_EQ(store.state().restart, Restart::done);
    EXPECT_EQ(store.state().restart_detail, "0.6.0");
    EXPECT_EQ(console.transport.count("/api/v1/session"), sessions + 1); // a new pairing code
    store.dismiss_restart();
    EXPECT_EQ(store.state().restart, Restart::idle);
    // Afterwards the usual rule applies: one automatic start per launch.
    console.online = false;
    store.poll_once();
    EXPECT_EQ(starter.calls, 2);
    EXPECT_EQ(store.state().restart, Restart::idle);
    store.poll_once();
    EXPECT_EQ(starter.calls, 2);
}

TEST(StoreRestart, WithoutALoaderItAsksForAPayloadManagerAndWaits)
{
    Console console;
    console.online = true;
    CountingStarter starter;
    starter.outcome = Outcome::no_loader;
    Store store(console.transport, &starter);
    store.set_restart_grace(0.0);
    store.poll_once();
    store.restart_service();
    store.run_next_action();
    console.online = false;
    store.poll_once();
    store.poll_once();
    store.tick();
    EXPECT_EQ(store.state().restart, Restart::manual);
    store.poll_once();
    EXPECT_EQ(starter.calls, 1); // no retry on its own
    store.start_orbit();         // Try again
    store.poll_once();
    store.poll_once();
    EXPECT_EQ(starter.calls, 2);
    console.online = true; // started from a payload manager
    store.poll_once();
    store.tick();
    EXPECT_EQ(store.state().restart, Restart::done);

    Console plain;
    plain.online = true;
    Store waiting(plain.transport); // no starter at all
    waiting.set_restart_grace(0.0);
    waiting.poll_once();
    waiting.restart_service();
    waiting.run_next_action();
    plain.online = false;
    waiting.poll_once();
    waiting.poll_once();
    waiting.tick();
    EXPECT_EQ(waiting.state().restart, Restart::manual);
}

TEST(StoreRestart, ARefusedStopChangesNothing)
{
    Console console;
    console.online = true;
    console.transport.reply = [&console](const http::Request &request)
    {
        if (request.method == "POST" && request.path == "/api/v1/updates")
            return test::json_reply(409, R"({"error":"An Orbit update is in progress."})");
        return console.fixture.send(request);
    };
    CountingStarter starter;
    Store store(console.transport, &starter);
    store.poll_once();
    store.restart_service();
    store.run_next_action();
    store.tick();
    EXPECT_EQ(store.state().restart, Restart::idle);
    EXPECT_EQ(store.setting_result().error, "An Orbit update is in progress.");
    EXPECT_EQ(store.setting_result().setting, Setting::service_restart);
}

} // namespace
} // namespace orbit
