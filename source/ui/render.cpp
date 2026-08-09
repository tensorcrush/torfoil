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

    return use_font_for(language(), err);
}

// Le chinois simplifié a sa propre police système ; tout le reste tient dans la
// « Standard ». On ne recharge que si le type change : ouvrir quatre tailles de
// police coûte assez cher pour ne pas le faire à chaque image.
bool Renderer::use_font_for(Lang lang, std::string* err) {
    const int wanted = lang == Lang::Zh ? PlSharedFontType_ChineseSimplified
                                        : PlSharedFontType_Standard;
    if (wanted == font_type_) return true;

    PlFontData font_data;
    Result rc = plGetSharedFontByType(&font_data, static_cast<PlSharedFontType>(wanted));
    if (R_FAILED(rc) && wanted != PlSharedFontType_Standard) {
        // Console dont la police chinoise n'est pas installée : mieux vaut du
        // texte partiellement carré que pas d'interface du tout.
        rc = plGetSharedFontByType(&font_data, PlSharedFontType_Standard);
    }
    if (R_FAILED(rc)) {
        if (err) *err = "police système indisponible (pl:u)";
        return false;
    }

    // Les textures en cache portent l'ancienne police : les garder afficherait
    // un mélange des deux, les mots déjà rendus restant dans l'autre dessin.
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

    const FontSize sizes[] = {FontSize::Small, FontSize::Body, FontSize::Title, FontSize::Huge};
    for (int i = 0; i < 4; ++i) {
        // Un RWops par police : SDL_ttf conserve le flux, on ne peut pas le
        // partager entre plusieurs tailles.
        SDL_RWops* rw = SDL_RWFromConstMem(font_data.address, font_data.size);
        if (!rw) {
            if (err) *err = std::string("SDL_RWFromConstMem : ") + SDL_GetError();
            return false;
        }
        fonts_[i] = TTF_OpenFontRW(rw, /*freesrc=*/1, pixel_size(sizes[i]));
        if (!fonts_[i]) {
            if (err) *err = std::string("TTF_OpenFontRW : ") + TTF_GetError();
            return false;
        }
    }

    font_type_ = wanted;
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

void Renderer::toggle_switch(int x, int y, int w, int h, bool on, bool focused) {
    const int radius = h / 2;

    // La glissière prend la couleur d'accent quand l'option est active. Éteinte,
    // elle reste franchement sombre : deux états qui se distinguent de loin, sans
    // avoir à lire quoi que ce soit.
    const Color track = on ? palette::kAccentDim : palette::kSurfaceAlt;
    rounded_rect(x, y, w, h, radius, track);

    // Liseré sur la ligne survolée : le focus se voit sur la pièce elle-même,
    // pas seulement sur le fond de la ligne.
    if (focused) {
        const Color edge = on ? palette::kAccent : palette::kTextDim;
        line(x + radius, y, x + w - radius, y, edge);
        line(x + radius, y + h - 1, x + w - radius, y + h - 1, edge);
    }

    const int inset = 3;
    const int knob = h - 2 * inset;
    const int knob_x = on ? x + w - inset - knob : x + inset;
    rounded_rect(knob_x, y + inset, knob, knob, knob / 2,
                 on ? palette::kAccent : palette::kTextDim);
}

int Renderer::key_badge_width(FontSize size, const std::string& label) {
    // Un minimum carré : « A » et « ZR » doivent produire des pastilles de
    // hauteurs égales et de largeurs cohérentes, sinon la barre d'aide ondule.
    return std::max(28, text_width(size, label) + 18);
}

void Renderer::key_badge(FontSize size, const std::string& label, int x, int y,
                         const Color& tint) {
    const int w = key_badge_width(size, label);
    const int h = line_height(size) + 6;
    rounded_rect(x, y - 3, w, h, 7, palette::kSurfaceAlt);
    text(size, label, x + (w - text_width(size, label)) / 2, y, tint);
}

void Renderer::section_header(const std::string& title, int x, int y, const Color& accent) {
    rounded_rect(x, y + 4, 4, 18, 2, accent);
    text(FontSize::Small, title, x + 16, y, palette::kTextDim);
}

void Renderer::qr_code(const util::QrCode& code, int x, int y, int scale) {
    if (code.size <= 0 || scale <= 0) return;

    const int extent = qr_extent(code, scale);
    // Fond blanc franc, zone de silence comprise. Un code QR sur fond sombre ne
    // se lit pas : les décodeurs attendent des modules sombres sur clair, et
    // inverser les deux suffit à rendre l'affichage inutile.
    rect(x, y, extent, extent, Color{0xff, 0xff, 0xff, 0xff});

    const int origin_x = x + kQrQuietZone * scale;
    const int origin_y = y + kQrQuietZone * scale;
    const Color dark{0x0d, 0x11, 0x13, 0xff};
    SDL_SetRenderDrawColor(renderer_, dark.r, dark.g, dark.b, dark.a);

    // Les modules sombres consécutifs d'une ligne sont fusionnés en un seul
    // rectangle : 1 681 appels de dessin par image pour un code de version 6,
    // contre environ deux cents ainsi.
    for (int my = 0; my < code.size; ++my) {
        int run_start = -1;
        for (int mx = 0; mx <= code.size; ++mx) {
            const bool on = mx < code.size && code.at(mx, my);
            if (on && run_start < 0) {
                run_start = mx;
            } else if (!on && run_start >= 0) {
                const SDL_Rect r{origin_x + run_start * scale, origin_y + my * scale,
                                 (mx - run_start) * scale, scale};
                SDL_RenderFillRect(renderer_, &r);
                run_start = -1;
            }
        }
    }
}

void Renderer::progress_bar(int x, int y, int w, int h, float ratio, const Color& fill_color) {
    ratio = std::max(0.0f, std::min(1.0f, ratio));
    rounded_rect(x, y, w, h, h / 2, palette::kSurfaceAlt);
    const int filled = static_cast<int>(static_cast<float>(w) * ratio);
    if (filled > h) rounded_rect(x, y, filled, h, h / 2, fill_color);
    else if (filled > 0) rect(x, y, filled, h, fill_color);
}

}  // namespace ui
