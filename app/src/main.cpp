// Orbit Store TV app - Console entry point.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Opens the display, controller and sound, connects to the Orbit backend on
// this console's loopback address, then runs the storefront every frame:
// read input, update, play the sounds it asked for, draw, present. Adapted
// from ps5-homebrew-ui's src/main.cpp.

#include "audio/cues.hpp"
#include "audio/mixer.hpp"
#include "core/input.hpp"
#include "core/save_file.hpp"
#include "core/version.hpp"
#include "gfx/renderer.hpp"
#include "orbit/art.hpp"
#include "orbit/gl_textures.hpp"
#include "orbit/http.hpp"
#include "orbit/i18n.hpp"
#include "orbit/mark.hpp"
#include "orbit/screens.hpp"
#include "orbit/starter.hpp"
#include "orbit/store.hpp"
#include "orbit/ui.hpp"
#include "platform/ps5/audio_out.hpp"
#include "platform/ps5/display_egl.hpp"
#include "platform/ps5/pad.hpp"
#include "platform/ps5/system.hpp"
#include "ui/fonts.hpp"

#include <GL/glcorearb.h>

#include <cstdint>
#include <cstdio>
#include <ctime>
#include <span>
#include <string>

extern "C" int sceSystemServiceLaunchWebBrowser(const char *uri, void *parameters);
extern "C" int sceSystemServiceParamGetInt(int param_id, int *value);

namespace
{

constexpr const char *kAssets = "/app0/assets";
constexpr const char *kBackend = "127.0.0.1";
constexpr std::uint16_t kPort = 34177; // config/network.json, as the backend
constexpr const char *kBrowserUrl = "http://127.0.0.1:34177/";

bool load_font(hui::gfx::Renderer &renderer, const char *name, hui::gfx::Font *font,
               hui::ui::FontRef *ref)
{
    std::string data;
    const std::string path = std::string(kAssets) + "/fonts/" + name;
    if (!hui::save::read_file(path, &data) || !font->load(data))
    {
        hui::sys::log("[ORBIT] font %s failed: %s", name, font->error().c_str());
        return false;
    }
    ref->font = font;
    ref->texture = renderer.batch().create_font_texture(*font);
    return true;
}

std::string today()
{
    // Days since 1970 to a civil date (H. Hinnant's algorithm): the console's
    // C library has no gmtime_r.
    const long long days = static_cast<long long>(std::time(nullptr)) / 86400;
    const long long z = days + 719468;
    const long long era = (z >= 0 ? z : z - 146096) / 146097;
    const long long doe = z - era * 146097;
    const long long yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const long long doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const long long mp = (5 * doy + 2) / 153;
    const long long day = doy - (153 * mp + 2) / 5 + 1;
    const long long month = mp < 10 ? mp + 3 : mp - 9;
    const long long year = yoe + era * 400 + (month <= 2 ? 1 : 0);
    char text[16];
    std::snprintf(text, sizeof(text), "%04lld-%02lld-%02lld", year, month, day);
    return text;
}

} // namespace

int main()
{
    using namespace hui;
    sys::log("[ORBIT] entry");

    // 4K when the OpenGL runtime can choose its size, else its fixed profile.
    ps5::Display display;
    const bool modes = ps5::Display::supports_display_modes();
    if (!(modes && display.open(3840, 2160)) && !display.open(1920, 1080))
    {
        sys::log("[ORBIT] fatal: display open failed");
        sys::park();
    }
    sys::log("[ORBIT] display %dx%d", display.width(), display.height());

    gfx::Renderer renderer;
    gfx::Font light;
    gfx::Font regular;
    gfx::Font medium;
    gfx::Font semibold;
    orbit::ui::Type type;
    if (!renderer.init() ||
        !load_font(renderer, "inter-display-light.huifont", &light, &type.light) ||
        !load_font(renderer, "inter-regular.huifont", &regular, &type.regular) ||
        !load_font(renderer, "inter-medium.huifont", &medium, &type.medium) ||
        !load_font(renderer, "inter-semibold.huifont", &semibold, &type.semibold))
    {
        sys::log("[ORBIT] fatal: renderer init failed");
        sys::park();
    }
    // The kit's six slots, for controller glyphs and the on-screen keyboard.
    ui::Fonts fonts;
    fonts.regular = type.regular;
    fonts.semibold = type.semibold;
    fonts.display = type.light;
    fonts.mono = type.medium;
    fonts.pixel = type.regular;
    fonts.hand = type.regular;

    orbit::GlTextures textures(renderer.batch());
    const std::uint32_t mark = textures.create(orbit::mark::render(128));
    const std::uint32_t loader_mark = textures.create(orbit::mark::render(256, 0.32f));

    ps5::Pad pad;
    pad.open();
    pad.set_light_bar(0x2a, 0x6d, 0xf4);
    InputTracker tracker;
    audio::Mixer mixer;
    mixer.set_bus_gain(audio::Bus::ui, 0.7f);
    ps5::AudioOut audio_out;
    audio_out.start(mixer);
    audio::SoundBank sounds;
    const auto bank = sounds.load(std::string(kAssets) + "/audio/sfx");
    sys::log("[ORBIT] sounds files=%d rejected=%d", bank.files, bank.rejected);

    orbit::http::SocketTransport transport(kBackend, kPort);
    orbit::http::SocketTransport art_transport(kBackend, kPort, 30000);
    // When Orbit isn't running, the app starts it through the jailbreak's ELF
    // loader: the copy Orbit saved, or the one in this package if newer.
    orbit::starter::LoaderStarter starter({}, orbit::starter::LoaderStarter::kLoaderPort,
                                          [](const std::string &line)
                                          { sys::log("[ORBIT] %s", line.c_str()); });
    // The console's own language, before anything shows or reports text.
    // SCE_SYSTEM_SERVICE_PARAM_ID_LANG is 1; English when Orbit has no catalogue.
    int system_language = 1;
    if (sceSystemServiceParamGetInt(1, &system_language) != 0)
        system_language = 1;
    const std::string language(orbit::i18n::from_system(system_language));
    if (!orbit::i18n::load(std::string(kAssets) + "/i18n", language))
        hui::sys::log("[ORBIT] language %s unavailable; using English", language.c_str());
    hui::sys::log("[ORBIT] language=%s system=%d", orbit::i18n::language().data(), system_language);

    orbit::Store store(transport, &starter);
    orbit::ArtCache art(art_transport, textures);
    store.start();
    art.start();

    orbit::Context context(type, fonts, store, art);
    context.mark = mark;
    context.loader_mark = loader_mark;
    context.today = today();
    context.app_version = read_content_version("/app0/sce_sys/param.json");
    context.open_browser = []
    { return sceSystemServiceLaunchWebBrowser(kBrowserUrl, nullptr) == 0; };
    context.open_download = [](const orbit::BrowserSelection &selection)
    {
        const std::string url = orbit::browser_handoff_url(selection);
        return !url.empty() && sceSystemServiceLaunchWebBrowser(url.c_str(), nullptr) == 0;
    };
    orbit::App app(context);
    orbit::Frame frame;
    frame.glass_texture = renderer.glass_texture();
    sys::log("[ORBIT] ready version=%s", context.app_version.c_str());

    std::int64_t last = sys::monotonic_us();
    std::uint64_t frames = 0;
    PadSample samples[64];
    ui::Feedback feedback;
    for (;;)
    {
        const std::int64_t now = sys::monotonic_us();
        float dt = frames == 0 ? 1.0f / 60.0f : static_cast<float>(now - last) / 1e6f;
        last = now;
        if (dt > 0.05f)
            dt = 0.05f; // a hitch must not teleport the animations
        const std::size_t count = pad.read(samples);
        const InputFrame input = tracker.update(std::span<const PadSample>(samples, count),
                                                static_cast<std::uint64_t>(now));
        feedback.clear();
        app.update(input, dt, feedback);
        for (const audio::CueEvent &event : feedback.cues)
            sounds.play(mixer, audio::SoundSet::glass, event);
        if (feedback.rumble_strength > 0.0f)
            pad.rumble(feedback.rumble_strength, feedback.rumble_seconds);
        pad.tick(dt);

        frame.reset();
        app.draw(frame);
        renderer.begin();
        renderer.backdrop(frame.backdrop);
        renderer.draw(frame.scene);
        if (frame.glass)
            renderer.glass();
        renderer.draw(frame.overlay);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
        renderer.present(0, display.width(), display.height());
        if (!display.swap())
        {
            sys::log("[ORBIT] fatal: swap failed frame=%llu error=%s",
                     static_cast<unsigned long long>(frames),
                     ps5::egl_error_name(display.last_error()));
            sys::park();
        }
        if (++frames == 1)
            sys::log("[ORBIT] first frame, splash hidden=%d", sys::hide_splash_screen() ? 1 : 0);
    }
}
