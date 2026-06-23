// ui/browser — DiizerU browse + player UI.
// Page-stack lists (menu / search results / playlists / liked / tracks), an
// on-screen keyboard for search, and a player with a queue, now-playing bar,
// progress, prev/next, play/pause and repeat (off/one/all). Network + track
// fetch/decode run on worker threads; the 60fps loop never blocks.
#pragma once

#include <SDL2/SDL.h>

#include <atomic>
#include <string>
#include <vector>

#include "../audio/sdl_audio_backend.h"
#include "../audio/stream_player.h"
#include "../core/models.h"
#include "../core/music_client.h"
#include "text.h"

namespace ui {

class Browser {
public:
    Browser(core::IMusicClient& client, Text& text, audio::SdlAudioBackend& backend,
            audio::StreamPlayer& streamer, std::string relay_url)
        : client_(client), text_(text), backend_(backend), streamer_(streamer),
          relay_url_(std::move(relay_url)) {}
    ~Browser();

    void start();
    void handle_button(Uint8 button);
    void handle_touch(int x, int y); // GamePad touchscreen tap (logical coords)
    void update(float dt_ms); // advance progress / auto-next
    void render(SDL_Renderer* r);

private:
    enum class RowKind { Menu, Playlist, Album, Track };
    struct Row {
        std::string title, subtitle;
        RowKind kind = RowKind::Menu;
        std::string id, uri;
        int menu_action = 0;
        long dur_ms = 0;
        std::string art_url;
    };
    struct Page {
        std::string title;
        std::vector<Row> rows;
        int sel = 0, scroll = 0;
    };
    enum class Repeat { Off, All, One };

    void push_menu();
    void select_current();
    void back();

    // fetch worker (browse)
    void start_fetch(int kind, const std::string& arg, const std::string& title);
    static int fetch_thunk(void* self);
    void fetch_worker();

    // player
    void play_track_list(const std::vector<Row>& rows, int sel);
    void play_index(int i);
    static int play_thunk(void* self);
    void play_worker();
    void toggle_pause();
    void next_track(bool user);
    void prev_track();
    void cycle_repeat();
    void do_seek(long delta_ms);

    // album art (async download -> surface -> texture on render thread)
    void start_art(const std::string& url);
    static int art_thunk(void* self);
    void art_worker();

    // keyboard
    void kb_press();
    void render_keyboard(SDL_Renderer* r);
    void render_credits(SDL_Renderer* r);

    bool credits_active_ = false;

    core::IMusicClient& client_;
    Text& text_;
    audio::SdlAudioBackend& backend_;
    audio::StreamPlayer& streamer_;
    std::string relay_url_;

    std::vector<Page> stack_;

    // fetch
    SDL_Thread* fetch_thread_ = nullptr;
    SDL_mutex* mtx_ = nullptr;
    std::atomic<bool> loading_{false};
    std::atomic<bool> fetch_done_{false};
    int fetch_kind_ = 0;
    std::string fetch_arg_, fetch_title_;
    Page fetched_;

    // keyboard
    bool kb_active_ = false;
    std::string query_;
    int kb_sel_ = 0;

    // player
    std::vector<core::Track> queue_;
    int q_index_ = -1;
    Repeat repeat_ = Repeat::Off;
    bool playing_ = false;
    bool has_track_ = false;
    std::string now_title_, now_artist_;
    double elapsed_ms_ = 0;
    double duration_ms_ = 0;
    SDL_Thread* play_thread_ = nullptr;
    std::atomic<bool> play_loading_{false};
    std::atomic<bool> play_done_{false};
    std::string pending_uri_;
    std::string now_art_url_;

    // album art
    SDL_Texture* art_tex_ = nullptr;
    std::atomic<void*> art_surface_{nullptr}; // SDL_Surface* handed from worker
    SDL_Thread* art_thread_ = nullptr;
    std::string art_url_;     // currently loaded
    std::string art_pending_; // worker target
};

} // namespace ui
