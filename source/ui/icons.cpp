#include "ui/icons.hpp"

#include <algorithm>
#include <cstring>
#include <vector>

#define NANOSVG_IMPLEMENTATION
#define NANOSVGRAST_IMPLEMENTATION
#include "nanosvg.h"
#include "nanosvgrast.h"

namespace ui {

namespace {

struct Extension {
    const char* ext;
    IconKind kind;
};

// L'ordre n'a pas d'importance, la liste est parcourue en entier. Elle couvre ce
// qu'on trouve réellement dans un dossier de téléchargements, pas la totalité
// des formats existants : au-delà, l'icône générique est une meilleure réponse
// qu'une association approximative.
const Extension kExtensions[] = {
    {"mkv", IconKind::Video},      {"mp4", IconKind::Video},
    {"avi", IconKind::Video},      {"mov", IconKind::Video},
    {"webm", IconKind::Video},     {"m4v", IconKind::Video},
    {"ts", IconKind::Video},       {"wmv", IconKind::Video},

    {"mp3", IconKind::Audio},      {"flac", IconKind::Audio},
    {"opus", IconKind::Audio},     {"ogg", IconKind::Audio},
    {"wav", IconKind::Audio},      {"m4a", IconKind::Audio},
    {"aac", IconKind::Audio},

    {"png", IconKind::Image},      {"jpg", IconKind::Image},
    {"jpeg", IconKind::Image},     {"gif", IconKind::Image},
    {"webp", IconKind::Image},     {"bmp", IconKind::Image},

    {"zip", IconKind::Archive},    {"7z", IconKind::Archive},
    {"rar", IconKind::Archive},    {"tar", IconKind::Archive},
    {"gz", IconKind::Archive},     {"xz", IconKind::Archive},
    {"iso", IconKind::Archive},    {"nsp", IconKind::Archive},
    {"xci", IconKind::Archive},    {"nsz", IconKind::Archive},
    {"xcz", IconKind::Archive},

    {"pdf", IconKind::Document},   {"txt", IconKind::Document},
    {"epub", IconKind::Document},  {"cbz", IconKind::Document},
    {"docx", IconKind::Document},  {"md", IconKind::Document},
    {"nfo", IconKind::Document},   {"srt", IconKind::Document},

    {"exe", IconKind::Executable}, {"msi", IconKind::Executable},
    {"apk", IconKind::Executable}, {"deb", IconKind::Executable},
    {"appimage", IconKind::Executable},

    {"nro", IconKind::Game},       {"nca", IconKind::Game},
    {"nsо", IconKind::Game},       {"z64", IconKind::Game},
    {"gba", IconKind::Game},       {"nds", IconKind::Game},
};

const char* svg_name_for(IconKind kind) {
    switch (kind) {
        case IconKind::Folder: return "folder";
        case IconKind::Video: return "movie";
        case IconKind::Audio: return "music_note";
        case IconKind::Image: return "image";
        case IconKind::Archive: return "folder_zip";
        case IconKind::Document: return "description";
        case IconKind::Executable: return "terminal";
        case IconKind::Game: return "sports_esports";
        case IconKind::Downloading: return "downloading";
        case IconKind::Paused: return "pause_circle";
        case IconKind::Done: return "check_circle";
        case IconKind::Unknown: break;
    }
    return "help";
}

const char* svg_for(IconKind kind) {
    const char* wanted = svg_name_for(kind);
    for (size_t i = 0; i < kIconSourceCount; ++i) {
        if (std::strcmp(kIconSources[i].name, wanted) == 0) return kIconSources[i].svg;
    }
    return kIconSources[0].svg;
}

}  // namespace

IconKind icon_kind_for(const std::string& filename) {
    const size_t dot = filename.rfind('.');
    if (dot == std::string::npos || dot + 1 >= filename.size()) return IconKind::Unknown;

    std::string ext = filename.substr(dot + 1);
    for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

    for (const Extension& entry : kExtensions) {
        if (ext == entry.ext) return entry.kind;
    }
    return IconKind::Unknown;
}

IconSet::~IconSet() {
    clear();
}

void IconSet::clear() {
    for (auto& entry : cache_) {
        if (entry.second) SDL_DestroyTexture(entry.second);
    }
    cache_.clear();
}

SDL_Texture* IconSet::texture_for(IconKind kind, int size) {
    const uint32_t key = (static_cast<uint32_t>(kind) << 16) | static_cast<uint32_t>(size);
    auto it = cache_.find(key);
    if (it != cache_.end()) return it->second;

    SDL_Texture* texture = nullptr;

    // nsvgParse modifie la chaîne qu'on lui donne : on lui passe une copie, pas
    // le littéral compilé en lecture seule.
    std::string source = svg_for(kind);
    if (NSVGimage* image = nsvgParse(&source[0], "px", 96.0f)) {
        if (NSVGrasterizer* rast = nsvgCreateRasterizer()) {
            std::vector<uint8_t> pixels(static_cast<size_t>(size) * size * 4, 0);
            const float scale = image->width > 0 ? static_cast<float>(size) / image->width : 1.0f;
            nsvgRasterize(rast, image, 0, 0, scale, pixels.data(), size, size, size * 4);
            nsvgDeleteRasterizer(rast);

            // Le SVG est noir ; on n'en garde que la couverture, et la couleur
            // vient de la teinte à l'affichage. Sans ça, une icône noire sur
            // fond sombre serait invisible et il faudrait une texture par
            // couleur.
            for (size_t i = 0; i < pixels.size(); i += 4) {
                pixels[i] = 0xff;
                pixels[i + 1] = 0xff;
                pixels[i + 2] = 0xff;
            }

            if (SDL_Surface* surface = SDL_CreateRGBSurfaceWithFormatFrom(
                    pixels.data(), size, size, 32, size * 4, SDL_PIXELFORMAT_ABGR8888)) {
                texture = SDL_CreateTextureFromSurface(renderer_, surface);
                SDL_FreeSurface(surface);
            }
            if (texture) SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);
        }
        nsvgDelete(image);
    }

    cache_[key] = texture;
    return texture;
}

void IconSet::draw(IconKind kind, int x, int y, int size, SDL_Color tint) {
    if (!renderer_ || size <= 0) return;
    SDL_Texture* texture = texture_for(kind, size);
    if (!texture) return;

    SDL_SetTextureColorMod(texture, tint.r, tint.g, tint.b);
    SDL_SetTextureAlphaMod(texture, tint.a);
    const SDL_Rect dst{x, y, size, size};
    SDL_RenderCopy(renderer_, texture, nullptr, &dst);
}

}  // namespace ui
