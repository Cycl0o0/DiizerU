// ui/pairing_screen — first-run device-code pairing screen.
//
// Runs the network pairing on a worker thread (SDL_Thread) so the 60fps render
// loop never blocks. The UI only reads a snapshot guarded by a mutex; all relay
// I/O lives in core::Pairing (UI-agnostic, host-tested).
#pragma once

#include <SDL2/SDL.h>

#include <string>

#include "../core/pairing.h"
#include "../core/relay_client.h"
#include "../core/session_store.h"
#include "text.h"

namespace ui {

class PairingScreen {
public:
    enum class Result { InProgress, Paired, Failed };

    PairingScreen(core::RelayClient& client, core::SessionStore& store, Text& text)
        : client_(client), store_(store), text_(text) {}
    ~PairingScreen();

    void start();                 // spawn the worker
    void handle_event(const SDL_Event& e);
    void render(SDL_Renderer* r); // draw current snapshot
    Result result();

private:
    struct Snapshot {
        core::Pairing::State state = core::Pairing::State::Idle;
        std::string user_code;
        std::string verify_url;
        std::string error;
    };

    static int worker_thunk(void* self);
    void worker();

    core::RelayClient& client_;
    core::SessionStore& store_;
    Text& text_;

    SDL_Thread* thread_ = nullptr;
    SDL_mutex* mtx_ = nullptr;
    Snapshot snap_;     // guarded by mtx_
    bool stop_ = false; // guarded by mtx_
};

} // namespace ui
