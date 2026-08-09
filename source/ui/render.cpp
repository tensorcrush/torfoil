#include "ui/render.hpp"

#include <switch.h>

#include <algorithm>
#include <cstdio>

namespace ui {

namespace {

int pixel_size(FontSize size) {
    switch (size) {
        case FontSize::Small: return 20;
        case FontSize::Body: return 25;
        case FontSize::Title: return 32;
        case FontSize::Huge: return 42;
    }
    return 25;
}

SDL_Color to_sdl(const Color& c) {
    return SDL_Color{c.r, c.g, c.b, c.a};
}

}  // namespace

bool Renderer::init(std::string* err) {
    auto fail = [&](const char* what) {
        if (err) *err = std::string(what) + " : " + SDL_GetError();
        return false;
    };

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER) != 0) return fail("SDL_Init");

    window_ = SDL_CreateWindow("Torfoil", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, kWidth,
                               kHeight, 0);
    if (!window_) return fail("SDL_CreateWindow");

    renderer_ = SDL_CreateRenderer(window_, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!renderer_) {
        // Repli logiciel. Sur console il ne devrait jamais servir, mais un
        // refus du moteur accéléré rendrait l'application impossible à démarrer
        // pour une raison purement graphique, alors qu'elle passe l'essentiel
        // de son temps à télécharger. Sur PC, où l'application tourne sans
        // affichage pour les essais scriptés, c'est le seul moteur disponible.
        renderer_ = SDL_CreateRenderer(window_, -1, SDL_RENDERER_SOFTWARE);
    }
    if (!renderer_) return fail("SDL_CreateRenderer");

    SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);

    if (TTF_Init() != 0) {
        if (err) *err = std::string("TTF_Init : ") + TTF_GetError();
        return false;
    }

    // Police système de la console : rien à embarquer, et les glyphes des
    // boutons de manette sont inclus.
    PlFontData font_data;
    const Result rc = plGetSharedFontByType(&font_data, PlSharedFontType_Standard);
    if (R_FAILED(rc)) {
        if (err) *err = "police système indisponible (pl:u)";
        return false;
    }

    const FontSize sizes[] = {FontSize::Small, FontSize::Body, FontSize::Title, FontSize::Huge};
    for (int i = 0; i < 4; ++i) {
        // Un RWops par police : SDL_ttf conserve le flux, on ne peut pas le
        // partager entre plusieurs tailles.
        SDL_RWops* rw = SDL_RWFromConstMem(font_data.address, font_data.size);
        if (!rw) return fail("SDL_RWFromConstMem");
        fonts_[i] = TTF_OpenFontRW(rw, /*freesrc=*/1, pixel_size(sizes[i]));
        if (!fonts_[i]) {
            if (err) *err = std::string("TTF_OpenFontRW : ") + TTF_GetError();
            return false;
        }
    }

    return true;
}

void Renderer::shutdown() {
    for (auto& entry : cache_) {
        if (entry.second.texture) SDL_DestroyTexture(entry.second.texture);
    }
    cache_.clear();

    for (TTF_Font*& f : fonts_) {
        if (f) {
            TTF_CloseFont(f);
            f = nullptr;
        }
    }
    if (TTF_WasInit()) TTF_Quit();

    if (renderer_) {
        SDL_DestroyRenderer(renderer_);
        renderer_ = nullptr;
    }
    if (window_) {
        SDL_DestroyWindow(window_);
        window_ = nullptr;
    }
    SDL_Quit();
}

TTF_Font* Renderer::font(FontSize size) const {
    return fonts_[static_cast<int>(size)];
}

void Renderer::begin_frame() {
    ++frame_;
}

void Renderer::end_frame() {
    SDL_RenderPresent(renderer_);
    if ((frame_ & 0xff) == 0) trim_cache();
}

void Renderer::fill(const Color& c) {
    SDL_SetRenderDrawColor(renderer_, c.r, c.g, c.b, c.a);
    SDL_RenderClear(renderer_);
}

void Renderer::rect(int x, int y, int w, int h, const Color& c) {
    SDL_SetRenderDrawColor(renderer_, c.r, c.g, c.b, c.a);
    const SDL_Rect r{x, y, w, h};
    SDL_RenderFillRect(renderer_, &r);
}

void Renderer::rounded_rect(int x, int y, int w, int h, int radius, const Color& c) {
    if (radius <= 0 || w < 2 * radius || h < 2 * radius) {
        rect(x, y, w, h, c);
        return;
    }

    SDL_SetRenderDrawColor(renderer_, c.r, c.g, c.b, c.a);

    // Corps central + deux bandes, puis les quatre coins tracés ligne par ligne.
    const SDL_Rect middle{x, y + radius, w, h - 2 * radius};
    SDL_RenderFillRect(renderer_, &middle);

    for (int dy = 0; dy < radius; ++dy) {
        // Décalage horizontal du coin pour cette ligne (cercle de rayon `radius`).
        const int dx = radius - static_cast<int>(
                                    SDL_sqrt(static_cast<double>(radius * radius - (radius - dy) * (radius - dy))));
        const SDL_Rect top{x + dx, y + dy, w - 2 * dx, 1};
        SDL_RenderFillRect(renderer_, &top);
        const SDL_Rect bottom{x + dx, y + h - 1 - dy, w - 2 * dx, 1};
        SDL_RenderFillRect(renderer_, &bottom);
    }
}

void Renderer::line(int x1, int y1, int x2, int y2, const Color& c) {
    SDL_SetRenderDrawColor(renderer_, c.r, c.g, c.b, c.a);
    SDL_RenderDrawLine(renderer_, x1, y1, x2, y2);
}

Renderer::CacheEntry* Renderer::acquire(FontSize size, const std::string& utf8, const Color& c) {
    if (utf8.empty()) return nullptr;

    char key_prefix[24];
    std::snprintf(key_prefix, sizeof(key_prefix), "%d|%02x%02x%02x|", static_cast<int>(size), c.r,
                  c.g, c.b);
    const std::string key = std::string(key_prefix) + utf8;

    auto it = cache_.find(key);
    if (it != cache_.end()) {
        it->second.last_used = frame_;
        return &it->second;
    }

    SDL_Surface* surface = TTF_RenderUTF8_Blended(font(size), utf8.c_str(), to_sdl(c));
    if (!surface) return nullptr;

    CacheEntry entry;
    entry.texture = SDL_CreateTextureFromSurface(renderer_, surface);
    entry.w = surface->w;
    entry.h = surface->h;
    entry.last_used = frame_;
    SDL_FreeSurface(surface);

    if (!entry.texture) return nullptr;
    return &cache_.emplace(key, entry).first->second;
}

void Renderer::trim_cache() {
    // Purge ce qui n'a pas servi depuis longtemps : les libellés de torrents
    // changent (débits, pourcentages) et la carte grossirait sans fin.
    for (auto it = cache_.begin(); it != cache_.end();) {
        if (frame_ - it->second.last_used > 240) {
            if (it->second.texture) SDL_DestroyTexture(it->second.texture);
            it = cache_.erase(it);
        } else {
            ++it;
        }
    }
}

void Renderer::text(FontSize size, const std::string& utf8, int x, int y, const Color& c) {
    CacheEntry* entry = acquire(size, utf8, c);
    if (!entry) return;
    const SDL_Rect dst{x, y, entry->w, entry->h};
    SDL_RenderCopy(renderer_, entry->texture, nullptr, &dst);
}

void Renderer::text_clipped(FontSize size, const std::string& utf8, int x, int y, int max_w,
                            const Color& c) {
    if (text_width(size, utf8) <= max_w) {
        text(size, utf8, x, y, c);
        return;
    }

    // Coupe en respectant les frontières UTF-8 (un octet de continuation
    // commence par 0b10xxxxxx).
    std::string cut = utf8;
    while (!cut.empty()) {
        do {
            cut.pop_back();
        } while (!cut.empty() && (static_cast<unsigned char>(cut.back()) & 0xc0) == 0x80);

        if (text_width(size, cut + "…") <= max_w) break;
    }
    text(size, cut + "…", x, y, c);
}

void Renderer::text_centered(FontSize size, const std::string& utf8, int cx, int y,
                             const Color& c) {
    text(size, utf8, cx - text_width(size, utf8) / 2, y, c);
}

int Renderer::text_width(FontSize size, const std::string& utf8) {
    if (utf8.empty()) return 0;
    int w = 0;
    int h = 0;
    TTF_SizeUTF8(font(size), utf8.c_str(), &w, &h);
    return w;
}

int Renderer::line_height(FontSize size) const {
    return TTF_FontHeight(font(size));
}

void Renderer::progress_bar(int x, int y, int w, int h, float ratio, const Color& fill_color) {
    ratio = std::max(0.0f, std::min(1.0f, ratio));
    rounded_rect(x, y, w, h, h / 2, palette::kSurfaceAlt);
    const int filled = static_cast<int>(static_cast<float>(w) * ratio);
    if (filled > h) rounded_rect(x, y, filled, h, h / 2, fill_color);
    else if (filled > 0) rect(x, y, filled, h, fill_color);
}

}  // namespace ui
