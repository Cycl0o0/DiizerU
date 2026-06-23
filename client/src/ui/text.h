// ui/text — minimal SDL2_ttf wrapper. Loads one font at a few sizes and draws
// UTF-8 strings. Pure presentation; no business logic.
#pragma once

#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>

#include <string>

namespace ui {

enum class Size { Small, Medium, Large, Huge };

class Text {
public:
    bool load(const char* font_path);
    void close();

    // Draw left-aligned at (x,y). Returns the rendered width (px).
    int draw(SDL_Renderer* r, const std::string& s, int x, int y, SDL_Color c,
             Size sz = Size::Medium);
    // Draw horizontally centered within [0,view_w).
    void draw_centered(SDL_Renderer* r, const std::string& s, int view_w, int y,
                       SDL_Color c, Size sz = Size::Medium);

    int measure(const std::string& s, Size sz);
    bool loaded() const { return small_ && med_ && large_ && huge_; }

private:
    TTF_Font* font_for(Size sz);
    TTF_Font* small_ = nullptr;
    TTF_Font* med_ = nullptr;
    TTF_Font* large_ = nullptr;
    TTF_Font* huge_ = nullptr;
};

} // namespace ui
