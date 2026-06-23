// platform/ — wut init, SD, network, input, and the SDL2 video context.
// Keeps Wii U/SDL specifics out of core/ and ui/ (see ARCHITECTURE §5).
#pragma once

#include <SDL2/SDL.h>

namespace platform {

// Logical render size; SDL scales to TV (1080p) and GamePad. UI lays out in
// these coordinates so the same layout works on both screens (responsive).
constexpr int kLogicalW = 1280;
constexpr int kLogicalH = 720;

struct Video {
    SDL_Window* window = nullptr;
    SDL_Renderer* renderer = nullptr;
    bool ok = false;
};

// Bring up SDL (video + gamepad), open a window/renderer with logical scaling.
Video init_video();

// Open the first attached game controller (Wii U GamePad / Pro Controller).
SDL_GameController* open_first_controller();

// Bring up the Wii U network stack (nn::ac) + curl_global_init, exactly once.
// MUST run before any libcurl/socket use, or the title crashes. Thread-safe and
// idempotent. Call it from a worker (it can block for seconds on ACConnect) —
// never on the render thread before the first frame. No-op off-console for ac.
void ensure_network_once();
void shutdown_network();

void shutdown(Video& v);

} // namespace platform
