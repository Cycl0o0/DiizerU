// DiizerU Wii U client — M4: first-run device-code pairing end-to-end.
//
// Flow: boot -> load relay URL from SD (config, never compiled in) -> if no
// saved session token, run the pairing screen (shows the TV code; user signs in
// on their phone) -> once paired, show the home placeholder. Real browse/player
// UI arrives in later milestones. No business logic lives in this file; it just
// wires platform + core + ui.

#include <SDL2/SDL.h>
#include <curl/curl.h>

#include <cstdio>
#include <memory>
#include <string>

#include "audio/sdl_audio_backend.h"
#include "audio/stream_player.h"
#include "core/deezer_client.h"
#include "core/relay_client.h"
#include "core/session_store.h"
#include "platform/platform.h"
#include "ui/browser.h"
#include "ui/pairing_screen.h"
#include "ui/text.h"

namespace {

constexpr SDL_Color kBg = {18, 18, 18, 255};
constexpr SDL_Color kPanel = {24, 24, 24, 255};
constexpr SDL_Color kAccent = {162, 56, 255, 255};
constexpr SDL_Color kText = {235, 235, 235, 255};
constexpr SDL_Color kMuted = {150, 150, 150, 255};

// Default relay for the central beta. Written to SD on first run; edit
// sd:/diizeru/relay.cfg to point at a self-hosted relay (PRIME DIRECTIVE).
const char* kDefaultRelayUrl = "https://your-domain.example/v1";
const char* kFontPath = "/vol/content/font.ttf";

void fill(SDL_Renderer* r, SDL_Color c, int x, int y, int w, int h) {
    SDL_SetRenderDrawColor(r, c.r, c.g, c.b, c.a);
    SDL_Rect rect{x, y, w, h};
    SDL_RenderFillRect(r, &rect);
}

} // namespace

int main(int /*argc*/, char** /*argv*/) {
    platform::Video v = platform::init_video();
    if (!v.ok) {
        platform::shutdown(v);
        return 1;
    }
    SDL_GameController* pad = platform::open_first_controller();
    // Network is brought up lazily off the render thread (see ensure_network_once)
    // so ACConnect never blocks the first frame.

    ui::Text text;
    if (!text.load(kFontPath)) {
        std::printf("[main] font load failed (%s)\n", kFontPath);
        // continue: shapes still render, just no text
    }

#ifdef __WIIU__
    core::SessionStore store("fs:/vol/external01/diizeru"); // SD root on Wii U
#else
    core::SessionStore store("sd:/diizeru");
#endif

    // Native (relay-optional) path: if the user dropped their Deezer ARL at
    // sd:/diizeru/arl.txt, the console talks to Deezer directly — login, browse,
    // decrypt and MP3-decode all happen here and the relay is never used.
    std::unique_ptr<core::RelayClient> relay;
    std::unique_ptr<core::DeezerClient> deezer;
    core::IMusicClient* music = nullptr;
    bool native = false;
    std::string relay_url;

    if (auto arl = store.load_arl()) {
        platform::ensure_network_once(); // login() needs the network up
        deezer = std::make_unique<core::DeezerClient>(*arl);
        if (deezer->login()) {
            native = true;
            music = deezer.get();
            std::printf("[main] native Deezer mode (user %s)\n", deezer->user_id().c_str());
        } else {
            std::printf("[main] ARL login failed; falling back to relay mode\n");
            deezer.reset();
        }
    }

    if (!native) {
        relay_url = store.load_relay_url().value_or("");
        // Migrate stale saved URLs from the old domain to the current default.
        if (relay_url.empty() || relay_url.find("your-domain.example") != std::string::npos) {
            relay_url = kDefaultRelayUrl;
            store.save_relay_url(relay_url);
        }
        relay = std::make_unique<core::RelayClient>(relay_url);
        music = relay.get();
    }

    enum class Mode { Pairing, Home };
    Mode mode;
    if (native) {
        mode = Mode::Home; // no pairing in native mode
    } else if (auto tok = store.load_token()) {
        relay->set_bearer(*tok);
        mode = Mode::Home;
    } else {
        mode = Mode::Pairing;
    }

    // Pairing screen only exists in relay mode.
    std::unique_ptr<ui::PairingScreen> pairing;
    if (!native) {
        pairing = std::make_unique<ui::PairingScreen>(*relay, store, text);
        if (mode == Mode::Pairing) pairing->start();
    }

    // Audio backend. Native path decodes MP3 -> 44100 Hz stereo s16; relay path
    // sends ADPCM that decodes to 22050 Hz (raw PCM is too heavy for the Wii U).
    audio::SdlAudioBackend backend;
    audio::AudioFormat afmt;
    afmt.sample_rate = native ? 44100 : 22050;
    // Shallow prebuffer: a deep queue makes the Wii U audio drain too fast at
    // the start of a track; the native path self-paces to keep it shallow.
    afmt.prebuffer_ms = native ? 800 : 1000;
    bool audio_ready = backend.init(afmt);
    audio::StreamPlayer streamer(backend);
    (void)audio_ready;

    std::unique_ptr<ui::Browser> browser;
    if (mode == Mode::Home) {
        browser = std::make_unique<ui::Browser>(*music, text, backend, streamer, relay_url);
        browser->start();
    }

    bool running = true;
    Uint32 last_ticks = SDL_GetTicks();
    while (running) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) running = false;
            if (mode == Mode::Pairing) pairing->handle_event(e);
            else if (browser && e.type == SDL_CONTROLLERBUTTONDOWN)
                browser->handle_button(e.cbutton.button);
            else if (browser && e.type == SDL_FINGERDOWN)
                browser->handle_touch((int)(e.tfinger.x * platform::kLogicalW),
                                      (int)(e.tfinger.y * platform::kLogicalH));
        }

        if (mode == Mode::Pairing) {
            pairing->render(v.renderer);
            if (pairing->result() == ui::PairingScreen::Result::Paired) {
                mode = Mode::Home;
                browser = std::make_unique<ui::Browser>(*music, text, backend, streamer, relay_url);
                browser->start();
                last_ticks = SDL_GetTicks();
            }
        } else if (browser) {
            Uint32 now = SDL_GetTicks();
            browser->update((float)(now - last_ticks));
            last_ticks = now;
            browser->render(v.renderer);
            // Re-link only applies to relay mode (native uses the SD-stored ARL).
            if (browser->wants_relink() && !native) {
                streamer.stop();
                backend.clear();
                store.clear_token();
                relay->set_bearer("");
                browser.reset();
                pairing = std::make_unique<ui::PairingScreen>(*relay, store, text);
                pairing->start();
                mode = Mode::Pairing;
            }
        }
    }

    streamer.stop();
    backend.shutdown();
    if (pad) SDL_GameControllerClose(pad);
    text.close();
    platform::shutdown_network();
    platform::shutdown(v);
    return 0;
}
