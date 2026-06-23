#include "text.h"

#include <cstdio>

#ifdef __WIIU__
// Font is embedded in the binary (the .wuhb /vol/content mount is not reachable
// via fopen on Wii U). bin2o generates these symbols from data/font.ttf.
#include "font_ttf.h"
#endif

namespace ui {

bool Text::load(const char* font_path) {
    if (TTF_Init() != 0) {
        std::printf("[ui] TTF_Init failed: %s\n", TTF_GetError());
        return false;
    }
#ifdef __WIIU__
    (void)font_path;
    auto open = [](int pt) -> TTF_Font* {
        SDL_RWops* rw = SDL_RWFromConstMem(font_ttf, (int)font_ttf_size);
        return rw ? TTF_OpenFontRW(rw, 1 /*freesrc*/, pt) : nullptr;
    };
    small_ = open(22);
    med_ = open(30);
    large_ = open(48);
    huge_ = open(96);
#else
    small_ = TTF_OpenFont(font_path, 22);
    med_ = TTF_OpenFont(font_path, 30);
    large_ = TTF_OpenFont(font_path, 48);
    huge_ = TTF_OpenFont(font_path, 96);
#endif
    if (!small_ || !med_ || !large_ || !huge_) {
        std::printf("[ui] font open failed: %s\n", TTF_GetError());
        return false;
    }
    return true;
}

void Text::close() {
    for (TTF_Font** f : {&small_, &med_, &large_, &huge_}) {
        if (*f) {
            TTF_CloseFont(*f);
            *f = nullptr;
        }
    }
    TTF_Quit();
}

TTF_Font* Text::font_for(Size sz) {
    switch (sz) {
        case Size::Small: return small_;
        case Size::Medium: return med_;
        case Size::Large: return large_;
        case Size::Huge: return huge_;
    }
    return med_;
}

int Text::draw(SDL_Renderer* r, const std::string& s, int x, int y, SDL_Color c, Size sz) {
    TTF_Font* f = font_for(sz);
    if (!f || s.empty()) return 0;
    SDL_Surface* surf = TTF_RenderUTF8_Blended(f, s.c_str(), c);
    if (!surf) return 0;
    SDL_Texture* tex = SDL_CreateTextureFromSurface(r, surf);
    int w = surf->w, h = surf->h;
    SDL_FreeSurface(surf);
    if (!tex) return 0;
    SDL_Rect dst{x, y, w, h};
    SDL_RenderCopy(r, tex, nullptr, &dst);
    SDL_DestroyTexture(tex);
    return w;
}

void Text::draw_centered(SDL_Renderer* r, const std::string& s, int view_w, int y, SDL_Color c, Size sz) {
    int w = measure(s, sz);
    draw(r, s, (view_w - w) / 2, y, c, sz);
}

int Text::measure(const std::string& s, Size sz) {
    TTF_Font* f = font_for(sz);
    if (!f || s.empty()) return 0;
    int w = 0, h = 0;
    TTF_SizeUTF8(f, s.c_str(), &w, &h);
    return w;
}

} // namespace ui
