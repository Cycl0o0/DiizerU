#include "pairing_screen.h"

#include "../platform/platform.h"

namespace ui {

namespace {
constexpr SDL_Color kBg = {18, 18, 18, 255};
constexpr SDL_Color kPanel = {24, 24, 24, 255};
constexpr SDL_Color kAccent = {162, 56, 255, 255};
constexpr SDL_Color kText = {235, 235, 235, 255};
constexpr SDL_Color kMuted = {150, 150, 150, 255};
constexpr SDL_Color kError = {235, 90, 90, 255};

void fill(SDL_Renderer* r, SDL_Color c, int x, int y, int w, int h) {
    SDL_SetRenderDrawColor(r, c.r, c.g, c.b, c.a);
    SDL_Rect rect{x, y, w, h};
    SDL_RenderFillRect(r, &rect);
}
} // namespace

PairingScreen::~PairingScreen() {
    if (mtx_) {
        SDL_LockMutex(mtx_);
        stop_ = true;
        SDL_UnlockMutex(mtx_);
    }
    if (thread_) SDL_WaitThread(thread_, nullptr);
    if (mtx_) SDL_DestroyMutex(mtx_);
}

void PairingScreen::start() {
    mtx_ = SDL_CreateMutex();
    thread_ = SDL_CreateThread(&PairingScreen::worker_thunk, "pairing", this);
}

int PairingScreen::worker_thunk(void* self) {
    static_cast<PairingScreen*>(self)->worker();
    return 0;
}

void PairingScreen::worker() {
    // Bring up the network here (off the render thread) before any relay call.
    platform::ensure_network_once();
    core::Pairing pairing(client_, store_);
    if (!pairing.begin("Wii U")) {
        SDL_LockMutex(mtx_);
        snap_.state = core::Pairing::State::Failed;
        snap_.error = pairing.error();
        SDL_UnlockMutex(mtx_);
        return;
    }
    {
        SDL_LockMutex(mtx_);
        snap_.state = core::Pairing::State::AwaitingUser;
        snap_.user_code = pairing.user_code();
        snap_.verify_url = pairing.verify_url();
        SDL_UnlockMutex(mtx_);
    }

    const int interval_ms = pairing.poll_interval() * 1000;
    while (true) {
        // sleep in small slices so a cancel is responsive
        for (int waited = 0; waited < interval_ms; waited += 100) {
            SDL_Delay(100);
            SDL_LockMutex(mtx_);
            bool stop = stop_;
            SDL_UnlockMutex(mtx_);
            if (stop) return;
        }
        pairing.poll();
        SDL_LockMutex(mtx_);
        snap_.state = pairing.state();
        snap_.error = pairing.error();
        bool done = snap_.state == core::Pairing::State::Paired ||
                    snap_.state == core::Pairing::State::Failed;
        SDL_UnlockMutex(mtx_);
        if (done) return;
    }
}

void PairingScreen::handle_event(const SDL_Event&) {
    // No input needed during pairing; cancellation is handled by destruction.
}

PairingScreen::Result PairingScreen::result() {
    if (!mtx_) return Result::InProgress;
    SDL_LockMutex(mtx_);
    core::Pairing::State s = snap_.state;
    SDL_UnlockMutex(mtx_);
    if (s == core::Pairing::State::Paired) return Result::Paired;
    if (s == core::Pairing::State::Failed) return Result::Failed;
    return Result::InProgress;
}

void PairingScreen::render(SDL_Renderer* r) {
    Snapshot s;
    if (mtx_) {
        SDL_LockMutex(mtx_);
        s = snap_;
        SDL_UnlockMutex(mtx_);
    }

    const int W = platform::kLogicalW;
    SDL_SetRenderDrawColor(r, kBg.r, kBg.g, kBg.b, kBg.a);
    SDL_RenderClear(r);

    fill(r, kAccent, 0, 0, W, 14); // top accent bar
    text_.draw_centered(r, "DiizerU", W, 70, kAccent, Size::Large);
    text_.draw_centered(r, "Link your Wii U", W, 150, kText, Size::Medium);

    if (s.state == core::Pairing::State::AwaitingUser) {
        text_.draw_centered(r, "On your phone, open", W, 250, kMuted, Size::Small);
        text_.draw_centered(r, s.verify_url, W, 285, kText, Size::Medium);
        text_.draw_centered(r, "enter this code + your Deezer ARL:", W, 345, kMuted, Size::Small);

        // big code panel
        int cw = text_.measure(s.user_code, Size::Huge);
        int px = (W - cw) / 2 - 40;
        fill(r, kPanel, px, 400, cw + 80, 140);
        text_.draw_centered(r, s.user_code, W, 420, kAccent, Size::Huge);

        text_.draw_centered(r, "The page shows how to find your ARL.", W, 575, kMuted, Size::Small);
        text_.draw_centered(r, "Waiting for you to link Deezer...", W, 610, kMuted, Size::Small);
    } else if (s.state == core::Pairing::State::Failed) {
        text_.draw_centered(r, "Pairing failed", W, 320, kError, Size::Large);
        text_.draw_centered(r, s.error.empty() ? "unknown error" : s.error, W, 400, kMuted,
                            Size::Medium);
        text_.draw_centered(r, "Relaunch to try again.", W, 470, kMuted, Size::Small);
    } else if (s.state == core::Pairing::State::Paired) {
        text_.draw_centered(r, "Linked!", W, 340, kAccent, Size::Large);
    } else {
        text_.draw_centered(r, "Connecting to relay...", W, 340, kMuted, Size::Medium);
    }

    // attribution
    const int H = platform::kLogicalH;
    text_.draw_centered(r, "Made with <3 by Cycl0o0", W, H - 90, kMuted, Size::Small);
    text_.draw_centered(r, "Not affiliated with Deezer or Nintendo", W, H - 58, kMuted, Size::Small);

    SDL_RenderPresent(r);
}

} // namespace ui
