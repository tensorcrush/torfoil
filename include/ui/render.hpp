// Couche de rendu : SDL2 + SDL2_ttf, polices système de la console.
//
// Pas de fichier de police embarqué : Horizon en expose une (pl:u) qui couvre
// latin, japonais et symboles de manettes. Ça évite un romfs et garantit un
// rendu cohérent avec le reste de la console.
#pragma once

#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>

#include <cstdint>
#include <string>
#include <unordered_map>

#include "util/qr.hpp"

namespace ui {

struct Color {
    uint8_t r = 255, g = 255, b = 255, a = 255;
};

namespace palette {
constexpr Color kBackground{0x14, 0x17, 0x1c, 0xff};
constexpr Color kSurface{0x1e, 0x22, 0x2a, 0xff};
constexpr Color kSurfaceAlt{0x26, 0x2b, 0x35, 0xff};
constexpr Color kSelected{0x2f, 0x4a, 0x52, 0xff};
constexpr Color kAccent{0x3d, 0xd6, 0xc4, 0xff};
constexpr Color kAccentDim{0x1f, 0x6b, 0x63, 0xff};
constexpr Color kText{0xe8, 0xec, 0xf1, 0xff};
constexpr Color kTextDim{0x8a, 0x93, 0xa3, 0xff};
constexpr Color kWarn{0xf2, 0xb1, 0x3c, 0xff};
constexpr Color kError{0xe5, 0x5b, 0x5b, 0xff};
constexpr Color kSuccess{0x63, 0xc9, 0x6a, 0xff};
}  // namespace palette

enum class FontSize { Small, Body, Title, Huge };

class Renderer {
public:
    static constexpr int kWidth = 1280;
    static constexpr int kHeight = 720;

    bool init(std::string* err);
    void shutdown();

    void begin_frame();
    void end_frame();

    void fill(const Color& c);
    void rect(int x, int y, int w, int h, const Color& c);
    void rounded_rect(int x, int y, int w, int h, int radius, const Color& c);
    void line(int x1, int y1, int x2, int y2, const Color& c);

    void text(FontSize size, const std::string& utf8, int x, int y, const Color& c);
    // Tronque avec une ellipse si le texte dépasse `max_w`.
    void text_clipped(FontSize size, const std::string& utf8, int x, int y, int max_w,
                      const Color& c);
    void text_centered(FontSize size, const std::string& utf8, int cx, int y, const Color& c);
    int text_width(FontSize size, const std::string& utf8);
    int line_height(FontSize size) const;

    void progress_bar(int x, int y, int w, int h, float ratio, const Color& fill_color);

    // Interrupteur à glissière. Remplace le « [x] » d'origine : la position du
    // curseur se lit d'un coup d'œil à travers la pièce, là où une croix entre
    // crochets demande de la lire.
    void toggle_switch(int x, int y, int w, int h, bool on, bool focused);

    // Touche de manette dessinée comme une touche : libellé clair sur pastille
    // sombre. Dans une barre d'aide, c'est ce qui sépare « ce sur quoi j'appuie »
    // de « ce que ça fait ».
    void key_badge(FontSize size, const std::string& label, int x, int y, const Color& tint);
    int key_badge_width(FontSize size, const std::string& label);

    // Cartouche de section : filet coloré puis titre en petites capitales.
    void section_header(const std::string& title, int x, int y, const Color& accent);

    // Code QR, zone de silence comprise. Un appareil photo a besoin de cette
    // marge claire autour du motif : sans elle, il ne trouve pas les repères et
    // le code paraît simplement « ne pas marcher ».
    void qr_code(const util::QrCode& code, int x, int y, int scale);
    // Côté total occupé, zone de silence comprise.
    static int qr_extent(const util::QrCode& code, int scale) {
        return code.size > 0 ? (code.size + 2 * kQrQuietZone) * scale : 0;
    }

    static constexpr int kQrQuietZone = 4;  // modules, imposé par la norme

    SDL_Renderer* raw() { return renderer_; }

private:
    struct CacheEntry {
        SDL_Texture* texture = nullptr;
        int w = 0;
        int h = 0;
        uint32_t last_used = 0;
    };

    TTF_Font* font(FontSize size) const;
    CacheEntry* acquire(FontSize size, const std::string& utf8, const Color& c);
    void trim_cache();

    SDL_Window* window_ = nullptr;
    SDL_Renderer* renderer_ = nullptr;
    TTF_Font* fonts_[4] = {nullptr, nullptr, nullptr, nullptr};

    // Le rendu de texte par TTF est coûteux : une texture par (taille, texte,
    // couleur) est conservée d'une image à l'autre.
    std::unordered_map<std::string, CacheEntry> cache_;
    uint32_t frame_ = 0;
};

}  // namespace ui
