// Orbit Store TV app - Typed calls to the Orbit backend's local API v1.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Every call blocks on the transport; run them on a worker thread. The app
// runs on the console itself, so it takes the backend's loopback-only local
// session instead of a pairing code (GET /api/v1/session).

#pragma once

#include "orbit/http.hpp"
#include "orbit/model.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace orbit::api
{

template <typename T> struct Result
{
    T value{};
    int status = 0;    // HTTP status, 0 when the backend was not reached
    std::string error; // empty on success: the backend's message, or a transport reason
    int retry_after = -1;

    bool ok() const
    {
        return error.empty();
    }
    // Orbit itself is not answering (stopped, not started, port taken).
    bool unreachable() const
    {
        return status == 0 && !error.empty();
    }
};

enum class ArtKind : std::uint8_t
{
    cover,
    hero,
};

struct Art
{
    bool pending = false; // the backend is still fetching it; ask again shortly
    std::string bytes;    // the encoded image (WebP, PNG or JPEG)
    std::string content_type;
};

class Client
{
  public:
    explicit Client(http::Transport &transport) : transport_(transport)
    {
    }

    void set_token(std::string token)
    {
        token_ = std::move(token);
    }
    const std::string &token() const
    {
        return token_;
    }

    Result<System> system();
    Result<Sources> sources();
    Result<Debrid> debrid();
    Result<std::vector<Release>> catalog();
    // The console's own session: its token, the pairing code and the console's
    // address (loopback callers only). Orbit saves its state on each call, so
    // ask only when these are needed.
    Result<Session> session();
    Result<std::vector<Drive>> storage();
    Result<std::vector<Job>> downloads();
    // Queues one option on one drive; the value is the new job ID.
    Result<std::string> create_download(std::string_view release_id, std::string_view storage_id,
                                        bool torbox = false);
    // pause, resume, retry, cancel or remove; the value is the updated queue.
    Result<std::vector<Job>> job_action(std::string_view job_id, std::string_view action,
                                        bool delete_partial = false);
    // Catalogue artwork through the backend (GET /api/v1/art/<gameId>/<kind>).
    Result<Art> art(std::string_view game_id, ArtKind kind);
    // Favourites: the saved game IDs, shared with every paired client.
    Result<std::vector<std::string>> favourites();
    Result<std::vector<std::string>> set_favourite(std::string_view game_id, bool favourite);
    // Library: refresh asks for a read sooner (POST {}), subject to the shared cooldown.
    Result<LibrarySnapshot> library(bool refresh);
    Result<LibraryStorage> library_storage(bool refresh);
    // One explicit Library action (docs/library.md). request_id makes it
    // idempotent: the same request is never carried out twice.
    struct LibraryRequest
    {
        std::string action; // scan, mount, unmount, measure, copy, move, cancel
        std::string title_id;
        std::string source_key;
        std::string destination_id;
        int job_id = 0;
        bool confirmed = false;
        std::string request_id;
    };
    Result<LibrarySnapshot> library_action(const LibraryRequest &request);

    // ---- settings ----
    // Saves the source choice with the download notice accepted.
    Result<Sources> save_sources(const std::vector<std::string> &enabled, int notice_version);
    // The drive new downloads start with; empty lets the app choose. The value
    // is the saved choice. A 404 means this Orbit is too old to save one.
    Result<std::string> set_preferred_storage(std::string_view storage_id);
    Result<CatalogueStatus> catalogue_status();
    Result<CatalogueStatus> refresh_catalogue();
    // Updates: Orbit's own (/updates) and this app's (/tv-app).
    struct UpdateRequest
    {
        std::string action; // check, install; stop (Orbit only)
        std::string version;
        std::string checksum;
        bool automatic = false; // a check on opening, sharing the console's cooldown
        bool confirmed = false; // stop
        bool replace = false;   // install over a copy the owner placed (TV app only)
    };
    Result<ServiceUpdate> service_update();
    Result<ServiceUpdate> service_action(const UpdateRequest &request);
    Result<TvAppStatus> tv_app();
    Result<TvAppStatus> tv_app_action(const UpdateRequest &request);

  private:
    http::Response send(const char *method, std::string path, std::string body = {},
                        std::size_t max_bytes = 0);
    template <typename T> bool fail(const http::Response &response, Result<T> *result);

    http::Transport &transport_;
    std::string token_;
};

// Characters allowed in IDs placed into request paths.
bool safe_path_segment(std::string_view value);

} // namespace orbit::api
