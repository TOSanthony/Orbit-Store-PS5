// Orbit Store TV app - Game artwork: fetched through Orbit, decoded off the frame.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Screens ask for a game's cover or background every frame they draw it. The
// first ask queues a fetch (GET /api/v1/art/...) on the art worker, which
// decodes the image at drawing size. The frame thread turns finished images
// into textures, a couple per frame so a burst of arrivals cannot stall a
// frame, and evicts the least recently drawn when over budget. The newest
// request is served first: what is on screen now loads before what scrolled
// past.

#pragma once

#include "orbit/api.hpp"
#include "orbit/image.hpp"
#include "orbit/thread.hpp"

#include <cstdint>
#include <deque>
#include <map>
#include <string>

namespace orbit
{

// Where textures come from: OpenGL in the app and the preview, a fake in tests.
class TextureSink
{
  public:
    virtual ~TextureSink() = default;
    virtual std::uint32_t create(const image::Pixels &pixels) = 0;
    virtual void destroy(std::uint32_t texture) = 0;
};

struct Artwork
{
    std::uint32_t texture = 0;
    int width = 0;
    int height = 0;
    std::uint32_t ambient = 0; // covers only: the blurred colour wash
    double ready_at = 0.0;     // when it became drawable, for a fade-in
};

struct ArtLimits
{
    int cover_size = 400;   // longest side of a decoded cover
    int hero_width = 1920;  // backgrounds fit inside this ...
    int hero_height = 1080; // ... and this
    int max_covers = 64;    // textures kept before the least recent go
    int max_heroes = 4;
    int uploads_per_frame = 2;
    double retry_failed = 60.0; // seconds before a failed image is asked again
};

class ArtCache
{
  public:
    ArtCache(http::Transport &transport, TextureSink &sink, ArtLimits limits = {});
    ~ArtCache();
    ArtCache(const ArtCache &) = delete;
    ArtCache &operator=(const ArtCache &) = delete;

    void start();
    void stop();
    // The backend serves artwork to paired clients only.
    void set_token(const std::string &token);

    // Frame thread: the image when it is ready, else nullptr (and it is asked for).
    const Artwork *get(const std::string &game_id, api::ArtKind kind);
    // Frame thread, once per frame: textures from finished decodes, eviction.
    void tick();
    // Nothing queued, fetching or waiting for upload (previews wait for this).
    bool idle();
    // Releases every texture (before the GL context goes away).
    void clear();

    // Worker step, public for tests: serves one request. False when none was ready.
    bool work_once();

  private:
    enum class State : std::uint8_t
    {
        loading,
        ready,
        failed,
    };
    struct Entry
    {
        State state = State::loading;
        Artwork art;
        double last_used = 0.0;
        double retry_at = 0.0;
    };
    struct Request
    {
        std::string key;
        std::string game_id;
        api::ArtKind kind = api::ArtKind::cover;
        double not_before = 0.0;
        int attempts = 0;
    };
    struct Decoded
    {
        std::string key;
        bool ok = false;
        image::Pixels pixels;
        image::Pixels ambient;
    };

    void worker();
    void evict(api::ArtKind kind, int keep);

    http::Transport &transport_;
    TextureSink &sink_;
    ArtLimits limits_;
    std::map<std::string, Entry> entries_; // frame thread only
    bool has_token_ = false;               // frame thread only
    Inbox inbox_;
    std::deque<Decoded> uploads_; // frame thread only

    Mutex mutex_;
    Signal signal_;
    std::deque<Request> requests_;
    std::string token_;
    int in_flight_ = 0;
    bool stopping_ = false;
    Thread thread_;
};

} // namespace orbit
