// Orbit Store TV app - Starting Orbit when it isn't running.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The app is a screen for Orbit, which runs as a payload so downloads carry on
// after the app is suspended or closed. When nothing answers on Orbit's port,
// the app hands a copy of Orbit to the ELF loader the console's jailbreak runs
// on 127.0.0.1:9021, as a payload sender would. It sends the copy Orbit saved
// in /data/orbit-store (which Orbit's updater keeps current) unless the copy in
// this package is a newer release, or the saved one can't be read.

#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

namespace orbit::starter
{

// Release versions as Orbit numbers them: "0.4.2", "v0.5.0-beta.1".
// Returns -1, 0 or 1; anything else sorts below every real version.
int compare_versions(std::string_view a, std::string_view b);

struct Locations
{
    std::string saved_elf = "/data/orbit-store/orbit_store.elf";
    std::string saved_version = "/data/orbit-store/saved-version.txt";
    std::string bundled_elf = "/app0/orbit/orbit_store.elf";
    std::string bundled_version = "/app0/orbit/version.txt";
};

struct Payload
{
    std::string path; // empty when there is nothing to start
    std::string version;
    const char *source = ""; // "saved" or "bundled"
    long long bytes = 0;
};

// The copy to start: a readable x86-64 ELF of at most 64 MiB.
Payload choose(const Locations &where);

enum class Outcome : std::uint8_t
{
    sent,       // the loader took the whole file
    no_loader,  // nothing listens on the loader's port
    no_payload, // neither copy of Orbit can be read
    failed,     // the transfer broke off
};

// Streams the file to the loader on 127.0.0.1:port.
Outcome send(const std::string &path, std::uint16_t port, int timeout_ms, std::string *error);

struct Result
{
    Outcome outcome = Outcome::failed;
    std::string detail; // for the screen and the log
};

// What the store calls to start Orbit; tests substitute their own.
class Starter
{
  public:
    virtual ~Starter() = default;
    // Blocks while the payload is sent; runs on the store's worker.
    virtual Result start() = 0;
};

class LoaderStarter final : public Starter
{
  public:
    static constexpr std::uint16_t kLoaderPort = 9021;

    LoaderStarter(Locations where, std::uint16_t port,
                  std::function<void(const std::string &)> log = {});
    Result start() override;

  private:
    Locations where_;
    std::uint16_t port_;
    std::function<void(const std::string &)> log_;
};

} // namespace orbit::starter
