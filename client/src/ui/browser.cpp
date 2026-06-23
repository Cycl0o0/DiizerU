#include "browser.h"

#include <SDL2/SDL_image.h>
#include <curl/curl.h>

#include <algorithm>
#include <string>

#include "../platform/platform.h"

#ifdef __WIIU__
#include "cacert_pem.h"
#endif

namespace {
size_t art_write(char* p, size_t s, size_t n, void* ud) {
    auto* v = static_cast<std::string*>(ud);
    v->append(p, s * n);
    return s * n;
}
} // namespace

namespace ui {

namespace {
constexpr SDL_Color kBg = {15, 15, 19, 255};
constexpr SDL_Color kPanel = {26, 26, 33, 255};
constexpr SDL_Color kRowSel = {44, 44, 54, 255};
constexpr SDL_Color kAccent = {162, 56, 255, 255}; // Deezer purple
constexpr SDL_Color kText = {235, 235, 235, 255};
constexpr SDL_Color kMuted = {150, 150, 160, 255};

constexpr int kRowH = 62;
constexpr int kListTop = 120;
constexpr int kVisibleRows = 7;
constexpr int kBarH = 104;
// keyboard grid geometry (shared by render + touch hit-testing)
constexpr int KB_GX = 80, KB_GY = 230, KB_CW = 96, KB_CH = 84;

enum { FETCH_PLAYLISTS = 1, FETCH_LIKED, FETCH_PLAYLIST_TRACKS, FETCH_ALBUM_TRACKS, FETCH_SEARCH };
enum { MENU_LIKED = 1, MENU_PLAYLISTS, MENU_SEARCH };

// On-screen keyboard, 10 columns.
const int KB_COLS = 10;
const char* KB_KEYS[] = {
    "A", "B", "C", "D", "E", "F", "G", "H", "I", "J",
    "K", "L", "M", "N", "O", "P", "Q", "R", "S", "T",
    "U", "V", "W", "X", "Y", "Z", "0", "1", "2", "3",
    "4", "5", "6", "7", "8", "9", "SPACE", "DEL", "GO"};
const int KB_N = 39;

void fill(SDL_Renderer* r, SDL_Color c, int x, int y, int w, int h) {
    SDL_SetRenderDrawColor(r, c.r, c.g, c.b, c.a);
    SDL_Rect rect{x, y, w, h};
    SDL_RenderFillRect(r, &rect);
}
const char* repeat_label(int rp) { return rp == 0 ? "OFF" : (rp == 1 ? "ALL" : "ONE"); }
} // namespace

Browser::~Browser() {
    if (fetch_thread_) SDL_WaitThread(fetch_thread_, nullptr);
    if (play_thread_) SDL_WaitThread(play_thread_, nullptr);
    if (art_thread_) SDL_WaitThread(art_thread_, nullptr);
    if (auto* s = (SDL_Surface*)art_surface_.exchange(nullptr)) SDL_FreeSurface(s);
    if (art_tex_) SDL_DestroyTexture(art_tex_);
    if (mtx_) SDL_DestroyMutex(mtx_);
}

void Browser::start() {
    mtx_ = SDL_CreateMutex();
    IMG_Init(IMG_INIT_JPG | IMG_INIT_PNG);
    push_menu();
}

void Browser::push_menu() {
    Page p;
    p.title = "DiizerU";
    p.rows.push_back({"Search", "find any track", RowKind::Menu, "", "", MENU_SEARCH});
    p.rows.push_back({"Liked Songs", "your saved tracks", RowKind::Menu, "", "", MENU_LIKED});
    p.rows.push_back({"Playlists", "your playlists", RowKind::Menu, "", "", MENU_PLAYLISTS});
    stack_.push_back(std::move(p));
}

void Browser::back() {
    if (stack_.size() > 1) stack_.pop_back();
}

// ---------------- input ----------------
void Browser::handle_button(Uint8 b) {
    if (kb_active_) {
        switch (b) {
            case SDL_CONTROLLER_BUTTON_DPAD_LEFT: if (kb_sel_ > 0) kb_sel_--; break;
            case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: if (kb_sel_ < KB_N - 1) kb_sel_++; break;
            case SDL_CONTROLLER_BUTTON_DPAD_UP: if (kb_sel_ >= KB_COLS) kb_sel_ -= KB_COLS; break;
            case SDL_CONTROLLER_BUTTON_DPAD_DOWN: if (kb_sel_ + KB_COLS < KB_N) kb_sel_ += KB_COLS; break;
            case SDL_CONTROLLER_BUTTON_A: kb_press(); break;
            case SDL_CONTROLLER_BUTTON_B: kb_active_ = false; break;
            default: break;
        }
        return;
    }

    // global transport
    switch (b) {
        case SDL_CONTROLLER_BUTTON_Y: toggle_pause(); return;
        case SDL_CONTROLLER_BUTTON_LEFTSHOULDER: prev_track(); return;
        case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: next_track(true); return;
        case SDL_CONTROLLER_BUTTON_BACK: cycle_repeat(); return; // minus
        case SDL_CONTROLLER_BUTTON_X: relink_.store(true); return;
        case SDL_CONTROLLER_BUTTON_DPAD_LEFT: if (has_track_) { do_seek(-10000); return; } break;
        case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: if (has_track_) { do_seek(10000); return; } break;
        default: break;
    }

    if (loading_.load() || stack_.empty()) return;
    Page& pg = stack_.back();
    switch (b) {
        case SDL_CONTROLLER_BUTTON_DPAD_UP: if (pg.sel > 0) pg.sel--; break;
        case SDL_CONTROLLER_BUTTON_DPAD_DOWN: if (pg.sel + 1 < (int)pg.rows.size()) pg.sel++; break;
        case SDL_CONTROLLER_BUTTON_A: select_current(); break;
        case SDL_CONTROLLER_BUTTON_B: back(); break;
        default: break;
    }
}

void Browser::handle_touch(int x, int y) {
    const int W = platform::kLogicalW, H = platform::kLogicalH;
    // keyboard keys
    if (kb_active_) {
        for (int i = 0; i < KB_N; ++i) {
            int kx = KB_GX + (i % KB_COLS) * KB_CW, ky = KB_GY + (i / KB_COLS) * KB_CH;
            if (x >= kx && x <= kx + KB_CW - 12 && y >= ky && y <= ky + KB_CH - 12) {
                kb_sel_ = i;
                kb_press();
                return;
            }
        }
        return;
    }
    // mini-player: lower strip = seek, upper = play/pause
    int by = H - kBarH;
    if (y >= by) {
        if (has_track_ && duration_ms_ > 0 && y >= by + 70) {
            int px = 40, pw = W - 80;
            double frac = (double)(x - px) / (double)pw;
            frac = frac < 0 ? 0 : (frac > 1 ? 1 : frac);
            double target = frac * duration_ms_;
            elapsed_ms_ = target;
            backend_.clear();
            client_.seek((long)target);
        } else {
            toggle_pause();
        }
        return;
    }
    // list rows
    if (loading_.load() || stack_.empty()) return;
    Page& pg = stack_.back();
    for (int i = 0; i < kVisibleRows; ++i) {
        int idx = pg.scroll + i;
        if (idx >= (int)pg.rows.size()) break;
        int ry = kListTop + i * kRowH;
        if (y >= ry && y <= ry + kRowH - 6 && x >= 36 && x <= W - 36) {
            pg.sel = idx;
            select_current();
            return;
        }
    }
}

void Browser::select_current() {
    Page& pg = stack_.back();
    if (pg.sel < 0 || pg.sel >= (int)pg.rows.size()) return;
    const Row row = pg.rows[pg.sel];
    switch (row.kind) {
        case RowKind::Menu:
            if (row.menu_action == MENU_PLAYLISTS) start_fetch(FETCH_PLAYLISTS, "", "Playlists");
            else if (row.menu_action == MENU_LIKED) start_fetch(FETCH_LIKED, "", "Liked Songs");
            else if (row.menu_action == MENU_SEARCH) { kb_active_ = true; kb_sel_ = 0; }
            break;
        case RowKind::Playlist: start_fetch(FETCH_PLAYLIST_TRACKS, row.id, row.title); break;
        case RowKind::Album: start_fetch(FETCH_ALBUM_TRACKS, row.id, row.title); break;
        case RowKind::Track: play_track_list(pg.rows, pg.sel); break;
    }
}

// ---------------- keyboard ----------------
void Browser::kb_press() {
    std::string k = KB_KEYS[kb_sel_];
    if (k == "DEL") { if (!query_.empty()) query_.pop_back(); }
    else if (k == "SPACE") { query_ += ' '; }
    else if (k == "GO") {
        if (!query_.empty()) { kb_active_ = false; start_fetch(FETCH_SEARCH, query_, "Search"); }
    } else { query_ += k[0]; }
}

// ---------------- fetch worker ----------------
void Browser::start_fetch(int kind, const std::string& arg, const std::string& title) {
    if (loading_.exchange(true)) return;
    fetch_kind_ = kind;
    fetch_arg_ = arg;
    fetch_title_ = title;
    fetch_done_.store(false);
    if (fetch_thread_) { SDL_WaitThread(fetch_thread_, nullptr); fetch_thread_ = nullptr; }
    fetch_thread_ = SDL_CreateThread(&Browser::fetch_thunk, "fetch", this);
}
int Browser::fetch_thunk(void* self) { static_cast<Browser*>(self)->fetch_worker(); return 0; }

void Browser::fetch_worker() {
    platform::ensure_network_once();
    Page p;
    p.title = fetch_title_;
    auto add_tracks = [&](const std::vector<core::Track>& ts) {
        for (auto& t : ts)
            p.rows.push_back({t.name, t.artist_line(), RowKind::Track, "", t.uri, 0, t.duration_ms,
                              t.artwork_url});
    };
    switch (fetch_kind_) {
        case FETCH_PLAYLISTS:
            if (auto v = client_.playlists())
                for (auto& pl : *v)
                    p.rows.push_back({pl.name, std::to_string(pl.track_count) + " tracks",
                                      RowKind::Playlist, pl.id, "", 0});
            break;
        case FETCH_LIKED: if (auto v = client_.favorites()) add_tracks(*v); break;
        case FETCH_PLAYLIST_TRACKS: if (auto v = client_.playlist_tracks(fetch_arg_)) add_tracks(*v); break;
        case FETCH_ALBUM_TRACKS: if (auto v = client_.album_tracks(fetch_arg_)) add_tracks(*v); break;
        case FETCH_SEARCH:
            if (auto v = client_.search(fetch_arg_)) add_tracks(v->tracks);
            break;
        default: break;
    }
    SDL_LockMutex(mtx_);
    fetched_ = std::move(p);
    SDL_UnlockMutex(mtx_);
    fetch_done_.store(true);
    loading_.store(false);
}

// ---------------- player ----------------
void Browser::play_track_list(const std::vector<Row>& rows, int sel) {
    queue_.clear();
    int qidx = 0, n = 0;
    for (int i = 0; i < (int)rows.size(); ++i) {
        if (rows[i].kind != RowKind::Track) continue;
        core::Track t;
        t.name = rows[i].title;
        t.uri = rows[i].uri;
        t.album_name = rows[i].subtitle; // artist line stashed for display
        t.duration_ms = rows[i].dur_ms;
        t.artwork_url = rows[i].art_url;
        queue_.push_back(t);
        if (i == sel) qidx = n;
        n++;
    }
    if (!queue_.empty()) play_index(qidx);
}

void Browser::play_index(int i) {
    if (i < 0 || i >= (int)queue_.size()) return;
    q_index_ = i;
    const core::Track& t = queue_[i];
    now_title_ = t.name;
    now_artist_ = t.album_name; // artist line stashed here
    duration_ms_ = (double)t.duration_ms; // 0 if unknown -> no auto-next
    elapsed_ms_ = 0;
    has_track_ = true;
    pending_uri_ = t.uri;
    start_art(t.artwork_url);
    play_loading_.store(true);
    if (play_thread_) { SDL_WaitThread(play_thread_, nullptr); play_thread_ = nullptr; }
    play_thread_ = SDL_CreateThread(&Browser::play_thunk, "play", this);
}
int Browser::play_thunk(void* self) { static_cast<Browser*>(self)->play_worker(); return 0; }

void Browser::play_worker() {
    platform::ensure_network_once();
    streamer_.stop();
    backend_.clear();
    client_.play_uri(pending_uri_); // relay fetches+decrypts+decodes the track
    streamer_.start(relay_url_, client_.bearer(), "adpcm_ima");
    play_done_.store(true);
    play_loading_.store(false);
}

void Browser::toggle_pause() {
    if (!has_track_ || play_loading_.load()) return;
    playing_ = !playing_;
    backend_.pause(!playing_);
}

void Browser::next_track(bool user) {
    if (queue_.empty()) return;
    int idx = q_index_ + 1;
    if (idx >= (int)queue_.size()) {
        if (repeat_ == Repeat::All) idx = 0;
        else if (user) idx = (int)queue_.size() - 1;
        else { playing_ = false; return; }
    }
    play_index(idx);
}

void Browser::prev_track() {
    if (queue_.empty()) return;
    int idx = q_index_ - 1;
    if (idx < 0) idx = (repeat_ == Repeat::All) ? (int)queue_.size() - 1 : 0;
    play_index(idx);
}

void Browser::cycle_repeat() {
    repeat_ = (repeat_ == Repeat::Off) ? Repeat::All
              : (repeat_ == Repeat::All) ? Repeat::One
                                         : Repeat::Off;
}

void Browser::do_seek(long delta_ms) {
    if (!has_track_ || play_loading_.load() || duration_ms_ <= 0) return;
    double t = elapsed_ms_ + (double)delta_ms;
    if (t < 0) t = 0;
    if (t > duration_ms_) t = duration_ms_;
    elapsed_ms_ = t;
    backend_.clear();             // drop buffered audio at the old position
    client_.seek((long)t);        // relay seeks the decoded source
}

// ---------------- album art ----------------
void Browser::start_art(const std::string& url) {
    if (url.empty() || url == art_url_) return;
    art_url_ = url;
    art_pending_ = url;
    if (art_thread_) { SDL_WaitThread(art_thread_, nullptr); art_thread_ = nullptr; }
    art_thread_ = SDL_CreateThread(&Browser::art_thunk, "art", this);
}
int Browser::art_thunk(void* self) { static_cast<Browser*>(self)->art_worker(); return 0; }

void Browser::art_worker() {
    platform::ensure_network_once();
    std::string body;
    CURL* c = curl_easy_init();
    if (!c) return;
    curl_easy_setopt(c, CURLOPT_URL, art_pending_.c_str());
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, art_write);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &body);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 15L);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
#ifdef __WIIU__
    static const struct curl_blob ca = {(void*)cacert_pem, (size_t)cacert_pem_size, CURL_BLOB_NOCOPY};
    curl_easy_setopt(c, CURLOPT_CAINFO_BLOB, &ca);
#endif
    CURLcode rc = curl_easy_perform(c);
    curl_easy_cleanup(c);
    if (rc != CURLE_OK || body.empty()) return;
    SDL_RWops* rw = SDL_RWFromConstMem(body.data(), (int)body.size());
    SDL_Surface* surf = rw ? IMG_Load_RW(rw, 1) : nullptr;
    if (surf) {
        // replace any pending un-consumed surface
        if (auto* old = (SDL_Surface*)art_surface_.exchange(surf)) SDL_FreeSurface(old);
    }
}

// ---------------- update ----------------
void Browser::update(float dt_ms) {
    if (play_done_.exchange(false)) {
        playing_ = true;
        elapsed_ms_ = 0;
        backend_.pause(false);
    }
    if (play_loading_.load() || !playing_ || !has_track_) return;
    elapsed_ms_ += dt_ms;
    if (duration_ms_ > 0 && elapsed_ms_ >= duration_ms_) {
        if (repeat_ == Repeat::One) play_index(q_index_);
        else next_track(false);
    }
}

// ---------------- render ----------------
void Browser::render(SDL_Renderer* r) {
    if (fetch_done_.exchange(false)) {
        SDL_LockMutex(mtx_);
        Page p = std::move(fetched_);
        SDL_UnlockMutex(mtx_);
        if (p.rows.empty()) p.rows.push_back({"(empty)", "", RowKind::Menu, "", "", 0});
        stack_.push_back(std::move(p));
    }

    const int W = platform::kLogicalW;
    const int H = platform::kLogicalH;
    SDL_SetRenderDrawColor(r, kBg.r, kBg.g, kBg.b, kBg.a);
    SDL_RenderClear(r);
    fill(r, kAccent, 0, 0, W, 8);

    if (kb_active_) { render_keyboard(r); }
    else {
        const Page& pg = stack_.back();
        text_.draw(r, pg.title, 48, 34, kAccent, Size::Large);
        if (loading_.load()) {
            text_.draw_centered(r, "Loading...", W, 340, kMuted, Size::Large);
        } else {
            Page& mp = stack_.back();
            if (mp.sel < mp.scroll) mp.scroll = mp.sel;
            if (mp.sel >= mp.scroll + kVisibleRows) mp.scroll = mp.sel - kVisibleRows + 1;
            for (int i = 0; i < kVisibleRows; ++i) {
                int idx = mp.scroll + i;
                if (idx >= (int)pg.rows.size()) break;
                int y = kListTop + i * kRowH;
                const Row& row = pg.rows[idx];
                if (idx == mp.sel) {
                    fill(r, kRowSel, 36, y, W - 72, kRowH - 6);
                    fill(r, kAccent, 36, y, 5, kRowH - 6);
                }
                text_.draw(r, row.title, 60, y + 6, kText, Size::Medium);
                if (!row.subtitle.empty())
                    text_.draw(r, row.subtitle, 60, y + 36, kMuted, Size::Small);
            }
        }
    }

    // consume a downloaded album-art surface -> texture (render thread only)
    if (auto* surf = (SDL_Surface*)art_surface_.exchange(nullptr)) {
        if (art_tex_) SDL_DestroyTexture(art_tex_);
        art_tex_ = SDL_CreateTextureFromSurface(r, surf);
        SDL_FreeSurface(surf);
    }

    // mini-player
    const int barH = kBarH, by = H - barH;
    fill(r, kPanel, 0, by, W, barH);
    if (has_track_) {
        int tx = 40;
        if (art_tex_) {
            SDL_Rect dst{40, by + 14, 76, 76};
            SDL_RenderCopy(r, art_tex_, nullptr, &dst);
            tx = 130;
        }
        std::string st = play_loading_.load() ? "Loading"
                         : (playing_ ? "Playing" : "Paused");
        text_.draw(r, now_title_, tx, by + 14, kText, Size::Medium);
        text_.draw(r, now_artist_, tx, by + 46, kMuted, Size::Small);
        // repeat + state, right side
        char info[48];
        std::snprintf(info, sizeof(info), "%s  repeat:%s", st.c_str(), repeat_label((int)repeat_));
        text_.draw(r, info, W - 360, by + 14, kAccent, Size::Small);
        // progress
        double frac = duration_ms_ > 0 ? std::min(1.0, elapsed_ms_ / duration_ms_) : 0.0;
        int px = 40, pw = W - 80, py = by + 90;
        fill(r, kMuted, px, py, pw, 5);
        fill(r, kAccent, px, py, (int)(pw * frac), 5);
    } else {
        text_.draw(r, "A: open/play   Y: play/pause   L/R: prev/next   (-): repeat   X: re-link",
                   40, by + 40, kMuted, Size::Small);
    }
    SDL_RenderPresent(r);
}

void Browser::render_keyboard(SDL_Renderer* r) {
    const int W = platform::kLogicalW;
    text_.draw(r, "Search", 48, 34, kAccent, Size::Large);
    // query box
    fill(r, kPanel, 48, 110, W - 96, 70);
    text_.draw(r, query_.empty() ? "type your search..." : query_, 64, 126,
               query_.empty() ? kMuted : kText, Size::Medium);

    for (int i = 0; i < KB_N; ++i) {
        int col = i % KB_COLS, rowi = i / KB_COLS;
        int x = KB_GX + col * KB_CW, y = KB_GY + rowi * KB_CH;
        std::string lbl = KB_KEYS[i];
        if (i == kb_sel_) fill(r, kAccent, x, y, KB_CW - 12, KB_CH - 12);
        else fill(r, kPanel, x, y, KB_CW - 12, KB_CH - 12);
        SDL_Color c = (i == kb_sel_) ? SDL_Color{20, 20, 20, 255} : kText;
        const char* show = (lbl == "SPACE") ? "SPC" : lbl.c_str();
        text_.draw(r, show, x + 14, y + 18, c, Size::Small);
    }
    text_.draw(r, "Tap keys (touch) or D-pad + A.  B cancel.  GO to search.", 80,
               KB_GY + 4 * KB_CH + 6, kMuted, Size::Small);
}

} // namespace ui
