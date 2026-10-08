// Orbit Store TV app - Typed calls to the Orbit backend's local API v1.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "orbit/api.hpp"
#include "orbit/i18n.hpp"

namespace orbit::api
{

namespace
{

constexpr std::size_t kArtLimit = 24u << 20;

} // namespace

bool safe_path_segment(std::string_view value)
{
    if (value.empty() || value.size() > 128)
        return false;
    for (const char c : value)
    {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.';
        if (!ok)
            return false;
    }
    return value != "." && value != "..";
}

http::Response Client::send(const char *method, std::string path, std::string body,
                            std::size_t max_bytes)
{
    http::Request request;
    request.method = method;
    request.path = std::move(path);
    request.body = std::move(body);
    request.bearer = token_;
    request.max_bytes = max_bytes;
    return transport_.send(request);
}

template <typename T> bool Client::fail(const http::Response &response, Result<T> *result)
{
    result->status = response.status;
    result->retry_after = response.retry_after;
    if (!response.error.empty())
    {
        result->error = response.error;
        return true;
    }
    if (response.status >= 200 && response.status < 300)
        return false;
    // The backend explains every refusal as {"error": "..."}.
    json::Value value;
    if (json::parse(response.body, &value) && !value.text("error").empty())
        result->error = value.text("error");
    else
        result->error = tr("Orbit could not complete this request.");
    return true;
}

Result<System> Client::system()
{
    Result<System> result;
    const http::Response response = send("GET", "/api/v1/system");
    if (fail(response, &result))
        return result;
    json::Value value;
    if (!json::parse(response.body, &value) || !value.is_object())
        result.error = tr("Orbit sent an unreadable status.");
    else
        result.value = parse_system(value);
    return result;
}

Result<Sources> Client::sources()
{
    Result<Sources> result;
    const http::Response response = send("GET", "/api/v1/sources");
    if (fail(response, &result))
        return result;
    json::Value value;
    if (!json::parse(response.body, &value) || !value.is_object())
        result.error = tr("Orbit sent unreadable source settings.");
    else
        result.value = parse_sources(value);
    return result;
}

Result<Debrid> Client::debrid()
{
    Result<Debrid> result;
    const http::Response response = send("GET", "/api/v1/debrid");
    if (fail(response, &result))
        return result;
    json::Value value;
    if (!json::parse(response.body, &value) || !value.is_object())
        result.error = "Orbit sent unreadable TorBox availability.";
    else
        result.value = parse_debrid(value);
    return result;
}

Result<std::vector<Release>> Client::catalog()
{
    Result<std::vector<Release>> result;
    const http::Response response = send("GET", "/api/v1/catalog", {}, 16u << 20);
    if (fail(response, &result))
        return result;
    json::Value value;
    if (!json::parse(response.body, &value) || !value.is_array())
        result.error = tr("Orbit sent an unreadable catalogue.");
    else
        result.value = parse_catalog(value);
    return result;
}

Result<Session> Client::session()
{
    Result<Session> result;
    const http::Response response = send("GET", "/api/v1/session");
    if (fail(response, &result))
        return result;
    json::Value value;
    if (!json::parse(response.body, &value) || value.text("token").size() != 64)
        result.error = tr("Orbit did not open a console session.");
    else
        result.value = parse_session(value);
    return result;
}

Result<std::vector<Drive>> Client::storage()
{
    Result<std::vector<Drive>> result;
    const http::Response response = send("GET", "/api/v1/storage");
    if (fail(response, &result))
        return result;
    json::Value value;
    if (!json::parse(response.body, &value) || !value.is_array())
        result.error = tr("Orbit sent unreadable storage details.");
    else
        result.value = parse_storage(value);
    return result;
}

Result<std::vector<std::string>> Client::favourites()
{
    Result<std::vector<std::string>> result;
    const http::Response response = send("GET", "/api/v1/favorites");
    if (fail(response, &result))
        return result;
    json::Value value;
    if (!json::parse(response.body, &value) || !value.is_array())
        result.error = tr("Orbit sent unreadable favourites.");
    else
        result.value = parse_favourites(value);
    return result;
}

Result<std::vector<std::string>> Client::set_favourite(std::string_view game_id, bool favourite)
{
    Result<std::vector<std::string>> result;
    std::string body = "{\"gameId\":";
    body += json::quote(game_id);
    body += favourite ? ",\"favorite\":true}" : ",\"favorite\":false}";
    const http::Response response = send("POST", "/api/v1/favorites", std::move(body));
    if (fail(response, &result))
        return result;
    json::Value value;
    if (json::parse(response.body, &value) && value.is_array())
        result.value = parse_favourites(value);
    return result;
}

Result<LibrarySnapshot> Client::library(bool refresh)
{
    Result<LibrarySnapshot> result;
    const http::Response response =
        refresh ? send("POST", "/api/v1/library", "{}") : send("GET", "/api/v1/library");
    if (fail(response, &result))
        return result;
    json::Value value;
    if (!json::parse(response.body, &value) || !value.is_object())
        result.error = tr("Orbit sent an unreadable Library.");
    else
        result.value = parse_library(value);
    return result;
}

Result<LibraryStorage> Client::library_storage(bool refresh)
{
    Result<LibraryStorage> result;
    const http::Response response = refresh ? send("POST", "/api/v1/library/storage", "{}")
                                            : send("GET", "/api/v1/library/storage");
    if (fail(response, &result))
        return result;
    json::Value value;
    if (!json::parse(response.body, &value) || !value.is_object())
        result.error = tr("Orbit sent unreadable drive details.");
    else
        result.value = parse_library_storage(value);
    return result;
}

Result<LibrarySnapshot> Client::library_action(const LibraryRequest &request)
{
    Result<LibrarySnapshot> result;
    std::string body = "{\"action\":";
    body += json::quote(request.action);
    if (!request.title_id.empty())
    {
        body += ",\"titleId\":";
        body += json::quote(request.title_id);
    }
    if (!request.source_key.empty())
    {
        body += ",\"sourceKey\":";
        body += json::quote(request.source_key);
    }
    if (!request.destination_id.empty())
    {
        body += ",\"destinationId\":";
        body += json::quote(request.destination_id);
    }
    if (request.job_id > 0)
        body += ",\"jobId\":" + std::to_string(request.job_id);
    if (request.confirmed)
        body += ",\"confirmed\":true";
    body += ",\"requestId\":";
    body += json::quote(request.request_id);
    body += "}";
    const http::Response response = send("POST", "/api/v1/library/actions", std::move(body));
    if (fail(response, &result))
        return result;
    json::Value value;
    if (json::parse(response.body, &value) && value.is_object())
        result.value = parse_library(value);
    return result;
}

Result<std::vector<Job>> Client::downloads()
{
    Result<std::vector<Job>> result;
    const http::Response response = send("GET", "/api/v1/downloads");
    if (fail(response, &result))
        return result;
    json::Value value;
    if (!json::parse(response.body, &value) || !value.is_array())
        result.error = tr("Orbit sent an unreadable download queue.");
    else
        result.value = parse_jobs(value);
    return result;
}

Result<std::string> Client::create_download(std::string_view release_id,
                                            std::string_view storage_id, bool torbox)
{
    Result<std::string> result;
    std::string body = "{\"releaseId\":";
    body += json::quote(release_id);
    body += ",\"storageId\":";
    body += json::quote(storage_id);
    if (torbox)
        body += ",\"delivery\":\"torbox\"";
    body += "}";
    const http::Response response = send("POST", "/api/v1/downloads", std::move(body));
    if (fail(response, &result))
        return result;
    json::Value value;
    if (json::parse(response.body, &value))
        result.value = value.text("id");
    return result;
}

Result<std::vector<Job>> Client::job_action(std::string_view job_id, std::string_view action,
                                            bool delete_partial)
{
    Result<std::vector<Job>> result;
    if (!safe_path_segment(job_id) || !safe_path_segment(action))
    {
        result.error = tr("Unknown download action.");
        return result;
    }
    std::string path = "/api/v1/downloads/";
    path += job_id;
    path += '/';
    path += action;
    const http::Response response =
        send("POST", std::move(path), delete_partial ? "{\"deletePartial\":true}" : "{}");
    if (fail(response, &result))
        return result;
    json::Value value;
    if (json::parse(response.body, &value) && value.is_array())
        result.value = parse_jobs(value);
    return result;
}

Result<Art> Client::art(std::string_view game_id, ArtKind kind)
{
    Result<Art> result;
    if (!safe_path_segment(game_id))
    {
        result.error = tr("Unknown game.");
        return result;
    }
    std::string path = "/api/v1/art/";
    path += game_id;
    path += kind == ArtKind::hero ? "/hero" : "/cover";
    const http::Response response = send("GET", std::move(path), {}, kArtLimit);
    if (fail(response, &result))
        return result;
    if (response.status == 202)
    {
        result.value.pending = true;
        return result;
    }
    result.value.bytes = response.body;
    result.value.content_type = response.content_type;
    return result;
}

namespace
{

std::string update_body(const Client::UpdateRequest &request)
{
    std::string body = "{\"action\":";
    body += json::quote(request.action);
    if (!request.version.empty())
    {
        body += ",\"version\":";
        body += json::quote(request.version);
    }
    if (!request.checksum.empty())
    {
        body += ",\"checksum\":";
        body += json::quote(request.checksum);
    }
    if (request.automatic)
        body += ",\"automatic\":true";
    if (request.confirmed)
        body += ",\"confirmed\":true";
    if (request.replace)
        body += ",\"replace\":true";
    body += "}";
    return body;
}

} // namespace

Result<Sources> Client::save_sources(const std::vector<std::string> &enabled, int notice_version)
{
    Result<Sources> result;
    std::string body = "{\"enabled\":[";
    for (std::size_t i = 0; i < enabled.size(); ++i)
    {
        if (i > 0)
            body += ",";
        body += json::quote(enabled[i]);
    }
    body += "],\"acknowledged\":true,\"noticeVersion\":";
    body += std::to_string(notice_version);
    body += "}";
    const http::Response response = send("POST", "/api/v1/sources", std::move(body));
    if (fail(response, &result))
        return result;
    json::Value value;
    if (!json::parse(response.body, &value) || !value.is_object())
        result.error = tr("Orbit sent unreadable source settings.");
    else
        result.value = parse_sources(value);
    return result;
}

Result<std::string> Client::set_preferred_storage(std::string_view storage_id)
{
    Result<std::string> result;
    std::string body = "{\"storageId\":";
    body += json::quote(storage_id);
    body += "}";
    const http::Response response = send("POST", "/api/v1/storage/preferred", std::move(body));
    if (fail(response, &result))
        return result;
    json::Value value;
    if (json::parse(response.body, &value))
        result.value = value.text("preferredStorage");
    return result;
}

Result<CatalogueStatus> Client::catalogue_status()
{
    Result<CatalogueStatus> result;
    const http::Response response = send("GET", "/api/v1/catalog/updates");
    if (fail(response, &result))
        return result;
    json::Value value;
    if (!json::parse(response.body, &value) || !value.is_object())
        result.error = tr("Orbit sent an unreadable catalogue status.");
    else
        result.value = parse_catalogue_status(value);
    return result;
}

Result<CatalogueStatus> Client::refresh_catalogue()
{
    Result<CatalogueStatus> result;
    const http::Response response =
        send("POST", "/api/v1/catalog/updates", "{\"action\":\"refresh\"}");
    if (fail(response, &result))
        return result;
    json::Value value;
    if (json::parse(response.body, &value) && value.is_object())
        result.value = parse_catalogue_status(value);
    return result;
}

Result<ServiceUpdate> Client::service_update()
{
    Result<ServiceUpdate> result;
    const http::Response response = send("GET", "/api/v1/updates");
    if (fail(response, &result))
        return result;
    json::Value value;
    if (!json::parse(response.body, &value) || !value.is_object())
        result.error = tr("Orbit sent an unreadable update status.");
    else
        result.value = parse_service_update(value);
    return result;
}

Result<ServiceUpdate> Client::service_action(const UpdateRequest &request)
{
    Result<ServiceUpdate> result;
    const http::Response response = send("POST", "/api/v1/updates", update_body(request));
    if (fail(response, &result))
        return result;
    json::Value value;
    if (json::parse(response.body, &value) && value.is_object())
        result.value = parse_service_update(value);
    return result;
}

Result<TvAppStatus> Client::tv_app()
{
    Result<TvAppStatus> result;
    const http::Response response = send("GET", "/api/v1/tv-app");
    if (fail(response, &result))
        return result;
    json::Value value;
    if (!json::parse(response.body, &value) || !value.is_object())
        result.error = tr("Orbit sent an unreadable TV app status.");
    else
        result.value = parse_tv_app(value);
    return result;
}

Result<TvAppStatus> Client::tv_app_action(const UpdateRequest &request)
{
    Result<TvAppStatus> result;
    const http::Response response = send("POST", "/api/v1/tv-app", update_body(request));
    if (fail(response, &result))
        return result;
    json::Value value;
    if (json::parse(response.body, &value) && value.is_object())
        result.value = parse_tv_app(value);
    return result;
}

} // namespace orbit::api
