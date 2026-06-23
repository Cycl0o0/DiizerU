#include "platform.h"

#include <SDL2/SDL.h>
#include <curl/curl.h>

#include <cstdio>

#ifdef __WIIU__
#include <nn/ac.h>
#endif

namespace platform {

static SDL_SpinLock s_net_lock = 0;
static bool s_net_inited = false;

void ensure_network_once() {
    SDL_AtomicLock(&s_net_lock);
    bool claim = !s_net_inited;
    if (claim) s_net_inited = true; // claim before the slow init so others skip
    SDL_AtomicUnlock(&s_net_lock);
    if (!claim) return;

#ifdef __WIIU__
    // nn::ac Auto Connect: required before BSD sockets / libcurl on the Wii U.
    ACInitialize();
    if (!NNResult_IsSuccess(ACConnect())) {
        std::printf("[platform] ACConnect failed (no network profile?)\n");
    }
#endif
    curl_global_init(CURL_GLOBAL_DEFAULT);
}

void shutdown_network() {
    if (!s_net_inited) return;
    curl_global_cleanup();
#ifdef __WIIU__
    ACFinalize();
#endif
}

Video init_video() {
    Video v;
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER | SDL_INIT_AUDIO) != 0) {
        std::printf("[platform] SDL_Init failed: %s\n", SDL_GetError());
        return v;
    }

    // On the Wii U the SDL2 port mirrors the renderer to TV + GamePad.
    v.window = SDL_CreateWindow("DiizerU", SDL_WINDOWPOS_UNDEFINED,
                                SDL_WINDOWPOS_UNDEFINED, kLogicalW, kLogicalH,
                                SDL_WINDOW_SHOWN);
    if (!v.window) {
        std::printf("[platform] CreateWindow failed: %s\n", SDL_GetError());
        return v;
    }

    v.renderer = SDL_CreateRenderer(
        v.window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!v.renderer) {
        std::printf("[platform] CreateRenderer failed: %s\n", SDL_GetError());
        return v;
    }

    // Logical size -> the same layout scales cleanly to 1080p and the GamePad.
    SDL_RenderSetLogicalSize(v.renderer, kLogicalW, kLogicalH);
    v.ok = true;
    return v;
}

SDL_GameController* open_first_controller() {
    for (int i = 0; i < SDL_NumJoysticks(); ++i) {
        if (SDL_IsGameController(i)) {
            return SDL_GameControllerOpen(i);
        }
    }
    return nullptr;
}

void shutdown(Video& v) {
    if (v.renderer) SDL_DestroyRenderer(v.renderer);
    if (v.window) SDL_DestroyWindow(v.window);
    SDL_Quit();
    v = Video{};
}

} // namespace platform
