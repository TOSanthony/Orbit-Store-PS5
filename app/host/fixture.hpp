// Orbit Store TV app - A stand-in Orbit backend for PC previews.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Answers the API calls the app makes with invented games, two drives and a
// queue in every state, and draws simple cover art as PNG. Nothing here is a
// real product or real artwork. Preview only: never part of the console app.

#pragma once

#include "orbit/http.hpp"
#include "orbit/thread.hpp"

#include <string>
#include <vector>

namespace orbit::host
{

enum class FixtureMode
{
    normal,  // a catalogue, drives, a queue, favourites and a Library
    debrid,  // normal fixture plus a connected TorBox account
    offline, // Orbit is not running
    setup,   // sources not chosen yet
    updates, // as normal, with newer releases of Orbit and the TV app
};

class Fixture final : public http::Transport
{
  public:
    explicit Fixture(FixtureMode mode);
    http::Response send(const http::Request &request) override;

  private:
    struct Job
    {
        std::string id;
        std::string release;
        std::string game;
        std::string title_id;
        std::string title;
        std::string storage;
        std::string filename;
        std::string format;
        std::string status;
        std::string error;
        double received = 0.0;
        double total = 0.0;
        double speed = 0.0;
        double retry_at = 0.0;
        std::string delivery = "direct";
    };
    std::string jobs_json() const;
    std::string art(const std::string &game, bool hero) const;

    std::string library_json() const;
    std::string favourites_json() const;
    std::string sources_json() const;
    std::string catalogue_json() const;
    std::string service_json() const;
    std::string tv_app_json() const;

    FixtureMode mode_;
    Mutex mutex_;
    std::vector<Job> jobs_;
    int next_job_ = 100;
    std::vector<std::string> favourites_{"tidewater"};
    std::string library_action_; // the last accepted Library action, as JSON

    // Settings and updates.
    std::vector<std::string> enabled_;
    bool acknowledged_ = false;
    std::string preferred_ = "usb0";
    double catalogue_checked_ = 0.0; // Unix seconds
    double catalogue_after_ = 0.0;
    std::string running_ = "0.6.0"; // Orbit
    std::string saved_ = "0.6.0";
    double service_checked_ = 0.0;
    std::string service_phase_ = "idle";
    double stopped_until_ = 0.0; // after Stop Orbit: refused until then (steady seconds)
    std::string tv_installed_ = "1.0.0";
    double tv_checked_ = 0.0;
    std::string tv_phase_ = "idle";
};

} // namespace orbit::host
