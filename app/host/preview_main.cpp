// Orbit Store TV app - PC preview: runs the app headless and writes PNG pictures.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// usage: orbit_preview <assets dir> <output dir> [scenario|all] [width height]
//
// The same App, Store, ArtCache and drawing code as the console, rendered by
// Mesa's software OpenGL through surfaceless EGL, with a fixed 60 Hz clock.
// Data comes from the stand-in backend in fixture.cpp (invented games and
// generated art), or from a running Orbit backend when ORBIT_BACKEND is set
// to <ipv4>:<port> (for example the desktop preview container). Each
// scenario drives the app with scripted controller input and takes named
// pictures. Adapted from ps5-homebrew-ui's host/snapshot_main.cpp.

#include "fixture.hpp"

#include "gfx/gl_program.hpp"
#include "gfx/renderer.hpp"
#include "orbit/art.hpp"
#include "orbit/gl_textures.hpp"
#include "orbit/http.hpp"
#include "orbit/mark.hpp"
#include "orbit/screens.hpp"
#include "orbit/store.hpp"
#include "orbit/starter.hpp"
#include "orbit/i18n.hpp"
#include "ui/fonts.hpp"

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GL/glcorearb.h>

#include "../third_party/stb/stb_image_write.h"

#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace
{

using hui::Action;
using hui::Direction;

bool open_context()
{
    auto get_platform_display = reinterpret_cast<PFNEGLGETPLATFORMDISPLAYEXTPROC>(
        eglGetProcAddress("eglGetPlatformDisplayEXT"));
    EGLDisplay display =
        get_platform_display != nullptr
            ? get_platform_display(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, nullptr)
            : eglGetDisplay(EGL_DEFAULT_DISPLAY);
    EGLint major = 0;
    EGLint minor = 0;
    if (display == EGL_NO_DISPLAY || !eglInitialize(display, &major, &minor) ||
        !eglBindAPI(EGL_OPENGL_API))
        return false;
    const EGLint attributes[] = {EGL_CONTEXT_MAJOR_VERSION,
                                 4,
                                 EGL_CONTEXT_MINOR_VERSION,
                                 5,
                                 EGL_CONTEXT_OPENGL_PROFILE_MASK,
                                 EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT,
                                 EGL_NONE};
    EGLContext context = eglCreateContext(display, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, attributes);
    return context != EGL_NO_CONTEXT &&
           eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, context);
}

bool load_font(hui::gfx::Renderer &renderer, const std::string &path, hui::gfx::Font *font,
               hui::ui::FontRef *ref)
{
    std::FILE *file = std::fopen(path.c_str(), "rb");
    if (file == nullptr)
        return false;
    std::string data;
    char buffer[65536];
    std::size_t n = 0;
    while ((n = std::fread(buffer, 1, sizeof(buffer), file)) > 0)
        data.append(buffer, n);
    std::fclose(file);
    if (!font->load(data))
    {
        std::fprintf(stderr, "cannot load font %s\n", path.c_str());
        return false;
    }
    ref->font = font;
    ref->texture = renderer.batch().create_font_texture(*font);
    return true;
}

struct Step
{
    float wait = 0.4f; // simulated seconds before the step acts
    std::uint32_t press = 0;
    Direction nav = Direction::none;
    const char *capture = nullptr; // picture taken after the wait, before the input
    std::function<void(orbit::App &)> act;
};

struct Scenario
{
    const char *name;
    orbit::host::FixtureMode mode;
    bool live_ok; // runs against a live backend too
    std::vector<Step> steps;
};

Step pause(float seconds, const char *capture = nullptr)
{
    Step step;
    step.wait = seconds;
    step.capture = capture;
    return step;
}

Step move(Direction direction, float seconds = 0.3f)
{
    Step step;
    step.wait = seconds;
    step.nav = direction;
    return step;
}

Step tap(Action action, float seconds = 0.3f)
{
    Step step;
    step.wait = seconds;
    step.press = hui::action_bit(action);
    return step;
}

Step run(float seconds, std::function<void(orbit::App &)> act)
{
    Step step;
    step.wait = seconds;
    step.act = std::move(act);
    return step;
}

// Stands in for the ELF loader: the scenario's name says what starting Orbit does.
class PreviewStarter final : public orbit::starter::Starter
{
  public:
    explicit PreviewStarter(orbit::starter::Outcome outcome) : outcome_(outcome)
    {
    }
    orbit::starter::Result start() override
    {
        orbit::starter::Result result;
        result.outcome = outcome_;
        result.detail = outcome_ == orbit::starter::Outcome::sent
                            ? "Sent Orbit 0.6.0 to the ELF loader."
                            : "No ELF loader is listening on port 9021.";
        return result;
    }

  private:
    orbit::starter::Outcome outcome_;
};

std::vector<Scenario> scenarios()
{
    using Mode = orbit::host::FixtureMode;
    std::vector<Scenario> list;
    list.push_back(
        {"opening", Mode::offline, false, {pause(0.9f, "opening"), pause(3.0f, "offline")}});
    list.push_back({"starting", Mode::offline, false, {pause(1.0f, "starting")}});
    list.push_back({"no-loader", Mode::offline, false, {pause(1.0f, "no-loader")}});
    // First run: choose sources in the app, then on to Discover.
    list.push_back({"setup",
                    Mode::setup,
                    true,
                    {pause(1.5f, "setup"), tap(Action::confirm), pause(0.8f, "setup-sources"),
                     move(Direction::right), tap(Action::confirm), move(Direction::down),
                     tap(Action::confirm), move(Direction::down), pause(0.8f, "setup-ready"),
                     tap(Action::confirm), pause(2.5f, "setup-done")}});
    list.push_back({"discover",
                    Mode::normal,
                    true,
                    {run(2.0f, [](orbit::App &app) { app.show_tab(orbit::tab::discover); }),
                     pause(1.2f, "discover"), move(Direction::down), move(Direction::right),
                     move(Direction::right), pause(1.2f, "discover-rail"), move(Direction::down),
                     pause(1.0f, "discover-added"), move(Direction::down),
                     pause(1.0f, "discover-all"), move(Direction::up), move(Direction::up), move(Direction::up),
                     move(Direction::up), pause(0.6f, "top-bar")}});
    list.push_back({"game", Mode::normal, false,
        {run(1.5f, [](orbit::App &app) { app.open_game("ember-hollow"); }),
         pause(1.2f, "game"), tap(Action::confirm), pause(0.8f, "game-options"),
         move(Direction::up), move(Direction::left), move(Direction::left), tap(Action::confirm),
         pause(0.8f, "game-source"), move(Direction::down), tap(Action::confirm),
         move(Direction::right), move(Direction::right), tap(Action::confirm),
         pause(0.8f, "game-drive-menu"), tap(Action::back), move(Direction::left), tap(Action::confirm),
         pause(0.8f, "game-delivery"), tap(Action::back), tap(Action::back),
         move(Direction::right), move(Direction::right), tap(Action::confirm), pause(0.8f, "game-about")}});
    list.push_back({"game-browser", Mode::normal, false,
        {run(1.5f, [](orbit::App &app) { app.open_game("ironclad-skies"); }),
         pause(1.0f, "game-browser-hub"), tap(Action::confirm), pause(1.0f, "game-browser"),
         move(Direction::left), pause(0.6f, "game-source-notes"), move(Direction::down),
         pause(0.6f, "game-source-notes-scroll"), tap(Action::back), move(Direction::right),
         move(Direction::right), tap(Action::confirm), pause(0.6f, "game-browser-about")}});
    list.push_back({"game-library", Mode::normal, false,
        {run(1.5f, [](orbit::App &app) { app.open_game("quiet-harbour"); }),
         tap(Action::confirm), pause(1.0f, "game-library")}});
    list.push_back({"debrid", Mode::debrid, false,
        {run(1.5f, [](orbit::App &app) { app.open_game("neon-courier"); }),
         tap(Action::confirm), move(Direction::up), move(Direction::left), tap(Action::confirm),
         pause(0.6f, "debrid-choice"), move(Direction::down), tap(Action::confirm),
         move(Direction::down), pause(0.8f, "debrid-ready"), tap(Action::confirm), pause(1.5f, "debrid-queued")}});
    list.push_back({"delete-cancelled",
                    Mode::normal,
                    false,
                    {run(1.5f, [](orbit::App &app) {
                         app.show_tab(orbit::tab::downloads);
                         app.downloads().show_job("job105");
                     }), pause(0.8f, "cancelled-download"),
                     move(Direction::right), tap(Action::confirm),
                     pause(0.8f, "delete-cancelled"), move(Direction::left),
                     move(Direction::left), tap(Action::confirm),
                     pause(0.8f, "forget-cancelled")}});
    list.push_back({"queued",
                    Mode::normal,
                    false,
                    {run(1.5f, [](orbit::App &app) { app.open_game("tidewater"); }),
                     pause(1.2f, "game-queued")}});
    list.push_back(
        {"download",
         Mode::normal,
         false,
         {run(1.5f, [](orbit::App &app) { app.open_game("neon-courier"); }),
          move(Direction::down, 0.6f), move(Direction::down), pause(0.6f, "download-ready"),
          tap(Action::confirm, 0.2f), pause(1.5f, "download-added")}});
    list.push_back(
        {"downloads",
         Mode::normal,
         true,
         {run(1.5f, [](orbit::App &app) { app.show_tab(orbit::tab::downloads); }),
          pause(1.2f, "downloads"), move(Direction::down), pause(0.8f, "downloads-row"),
          move(Direction::right), tap(Action::confirm), pause(0.8f, "downloads-dialog"),
          tap(Action::back), move(Direction::up), move(Direction::up), move(Direction::right),
          move(Direction::right), pause(0.8f, "downloads-failed")}});
    list.push_back(
        {"browse",
         Mode::normal,
         true,
         {run(1.5f, [](orbit::App &app) { app.show_tab(orbit::tab::browse); }),
          pause(1.2f, "browse"), tap(Action::confirm),
          run(0.3f, [](orbit::App &app) { app.browse().set_query("o"); }),
          pause(0.9f, "browse-keyboard"), tap(Action::back), move(Direction::down),
          move(Direction::left), move(Direction::left), tap(Action::confirm),
          pause(0.8f, "browse-filters"), move(Direction::down), tap(Action::confirm),
          move(Direction::down), move(Direction::right), pause(1.0f, "browse-results")}});
    list.push_back(
        {"library",
         Mode::normal,
         false,
         {run(1.5f, [](orbit::App &app) { app.show_tab(orbit::tab::library); }),
          pause(1.2f, "library"), move(Direction::down), move(Direction::down),
          move(Direction::left), move(Direction::left), move(Direction::left),
          pause(0.8f, "library-grid"), tap(Action::confirm), pause(0.8f, "library-details"),
          move(Direction::right), tap(Action::confirm), pause(1.2f, "library-copy"),
          tap(Action::back), tap(Action::back), move(Direction::up), move(Direction::up),
          tap(Action::confirm), pause(1.2f, "library-storage")}});
    list.push_back({"settings",
                    Mode::normal,
                    true,
                    {run(1.5f, [](orbit::App &app) { app.open_settings(); }),
                     pause(0.8f, "settings"),
                     move(Direction::down),
                     pause(0.8f, "settings-storage"),
                     move(Direction::right),
                     move(Direction::down),
                     move(Direction::down),
                     tap(Action::confirm),
                     pause(1.0f, "settings-storage-saved"),
                     move(Direction::left),
                     move(Direction::down),
                     pause(1.0f, "settings-pairing"),
                     move(Direction::down),
                     move(Direction::right),
                     pause(0.8f, "settings-catalogue"),
                     tap(Action::confirm),
                     pause(0.8f, "settings-catalogue-refreshed"),
                     move(Direction::left),
                     move(Direction::down),
                     pause(1.0f, "settings-updates-current"),
                     move(Direction::down),
                     pause(0.8f, "settings-more")}});
    // Newer releases of both parts: the badge, guided installs and a restart.
    list.push_back(
        {"updates",
         Mode::updates,
         false,
         {pause(2.5f, "updates-badge"),
          run(0.5f, [](orbit::App &app)
              { app.open_settings(orbit::SettingsScreen::Section::updates, true); }),
          pause(1.0f, "settings-updates"), move(Direction::right), tap(Action::confirm),
          pause(1.0f, "settings-tv-installed"), move(Direction::down), move(Direction::right),
          tap(Action::confirm), pause(1.2f, "settings-service-saved"), tap(Action::confirm),
          pause(0.8f, "settings-restart-confirm"), move(Direction::left), tap(Action::confirm),
          pause(0.6f, "settings-restarting"), pause(40.0f, "settings-restarted")}});
    return list;
}

} // namespace

int main(int argc, char **argv)
{
    if (argc < 3)
    {
        std::fprintf(stderr, "usage: %s <assets dir> <output dir> [scenario|all] [width height]\n",
                     argv[0]);
        return 2;
    }
    const std::string assets = argv[1];
    const std::string output = argv[2];
    const std::string only = argc > 3 && std::string(argv[3]) != "all" ? argv[3] : "";
    const int width = argc > 5 ? std::atoi(argv[4]) : 1920;
    const int height = argc > 5 ? std::atoi(argv[5]) : 1080;
    const char *backend = std::getenv("ORBIT_BACKEND");
    // ORBIT_LANG=de renders the screens in German, from the repository's catalogues.
    if (const char *language = std::getenv("ORBIT_LANG"))
    {
        if (!orbit::i18n::load(assets + "/../../i18n", language))
            std::fprintf(stderr, "language %s unavailable; using English\n", language);
    }

    if (!open_context())
    {
        std::fprintf(stderr, "no surfaceless EGL OpenGL 4.5 context\n");
        return 1;
    }
    hui::gfx::set_glsl_prefix("#version 450 core\n");
    hui::gfx::Renderer renderer;
    hui::gfx::Font light;
    hui::gfx::Font regular;
    hui::gfx::Font medium;
    hui::gfx::Font semibold;
    orbit::ui::Type type;
    if (!renderer.init() ||
        !load_font(renderer, assets + "/fonts/inter-display-light.huifont", &light, &type.light) ||
        !load_font(renderer, assets + "/fonts/inter-regular.huifont", &regular, &type.regular) ||
        !load_font(renderer, assets + "/fonts/inter-medium.huifont", &medium, &type.medium) ||
        !load_font(renderer, assets + "/fonts/inter-semibold.huifont", &semibold, &type.semibold))
        return 1;
    hui::ui::Fonts fonts;
    fonts.regular = type.regular;
    fonts.semibold = type.semibold;
    fonts.display = type.light;
    fonts.mono = type.medium;
    fonts.pixel = type.regular;
    fonts.hand = type.regular;
    orbit::GlTextures textures(renderer.batch());
    const std::uint32_t mark = textures.create(orbit::mark::render(128));
    const std::uint32_t loader_mark = textures.create(orbit::mark::render(256, 0.32f));

    GLuint framebuffer = 0;
    GLuint color = 0;
    glGenFramebuffers(1, &framebuffer);
    glGenRenderbuffers(1, &color);
    glBindRenderbuffer(GL_RENDERBUFFER, color);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, width, height);
    glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, color);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        return 1;
    std::vector<unsigned char> pixels(static_cast<std::size_t>(width) * height * 4);
    stbi_flip_vertically_on_write(1);

    bool ok = true;
    int ran = 0;
    for (const Scenario &scenario : scenarios())
    {
        if (!only.empty() && only != scenario.name)
            continue;
        if (backend != nullptr && !scenario.live_ok)
            continue;
        ++ran;
        std::unique_ptr<orbit::http::Transport> transport;
        std::unique_ptr<orbit::http::Transport> art_transport;
        if (backend != nullptr)
        {
            std::string address = backend;
            const std::size_t colon = address.find(':');
            const auto port = static_cast<std::uint16_t>(
                colon == std::string::npos ? 34177 : std::atoi(address.c_str() + colon + 1));
            address = address.substr(0, colon);
            transport = std::make_unique<orbit::http::SocketTransport>(address, port);
            art_transport = std::make_unique<orbit::http::SocketTransport>(address, port, 30000);
        }
        else
        {
            transport = std::make_unique<orbit::host::Fixture>(scenario.mode);
            art_transport = std::make_unique<orbit::host::Fixture>(scenario.mode);
        }
        const std::string name = scenario.name;
        std::unique_ptr<PreviewStarter> starter;
        if (backend == nullptr && (name == "starting" || name == "no-loader" || name == "updates"))
            starter = std::make_unique<PreviewStarter>(name == "no-loader"
                                                           ? orbit::starter::Outcome::no_loader
                                                           : orbit::starter::Outcome::sent);
        orbit::Store store(*transport, starter.get());
        orbit::ArtCache art(*art_transport, textures);
        store.start();
        art.start();
        orbit::Context context(type, fonts, store, art);
        context.mark = mark;
        context.loader_mark = loader_mark;
        context.today = "2026-10-04";
        const char *preview_version = std::getenv("ORBIT_APP_VERSION");
        context.app_version = preview_version != nullptr ? preview_version : "01.000.001";
        context.open_browser = [] { return true; };
        // Preview callbacks never open websites or submit a real download.
        context.open_download = [](const orbit::BrowserSelection &) { return true; };
        orbit::App app(context);
        orbit::Frame frame;
        frame.glass_texture = renderer.glass_texture();

        constexpr float kDt = 1.0f / 60.0f;
        hui::ui::Feedback feedback;
        const auto tick = [&](const hui::InputFrame &input)
        {
            feedback.clear();
            app.update(input, kDt, feedback);
            // The workers run in real time: give them a moment every frame.
            usleep(1500);
        };
        const auto render = [&](const std::string &name)
        {
            // Draw once so the screen asks for its art, then wait (in real
            // time) until it has arrived, so pictures are complete.
            frame.reset();
            app.draw(frame);
            for (int i = 0; i < 600 && !art.idle(); ++i)
            {
                tick({});
                frame.reset();
                app.draw(frame);
            }
            // Art fades in over real time: let it finish.
            for (int i = 0; i < 30; ++i)
                tick({});
            usleep(300000);
            tick({});
            frame.reset();
            app.draw(frame);
            renderer.begin();
            renderer.backdrop(frame.backdrop);
            renderer.draw(frame.scene);
            if (frame.glass)
                renderer.glass();
            renderer.draw(frame.overlay);
            glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
            glClearColor(0, 0, 0, 1);
            glClear(GL_COLOR_BUFFER_BIT);
            renderer.present(framebuffer, width, height);
            glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
            glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
            for (std::size_t i = 3; i < pixels.size(); i += 4)
                pixels[i] = 255;
            const std::string path = output + "/" + name + ".png";
            ok =
                stbi_write_png(path.c_str(), width, height, 4, pixels.data(), width * 4) != 0 && ok;
            std::fprintf(stderr, "wrote %s: %zu shapes, %zu draw calls, GL error 0x%x\n",
                         path.c_str(), renderer.last_instances(), renderer.last_draw_calls(),
                         glGetError());
        };

        for (const Step &step : scenario.steps)
        {
            const int frames = static_cast<int>(step.wait / kDt);
            for (int i = 0; i < frames; ++i)
                tick({});
            if (step.act)
                step.act(app);
            if (step.capture != nullptr)
                render(step.capture);
            hui::InputFrame input;
            input.connected = true;
            input.pressed = step.press;
            input.nav = step.nav;
            tick(input);
        }
        art.stop();
        store.stop();
        art.clear();
    }
    if (ran == 0)
    {
        std::fprintf(stderr, "no scenario named '%s'\n", only.c_str());
        return 2;
    }
    return ok ? 0 : 1;
}
