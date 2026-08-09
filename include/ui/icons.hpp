// Icônes de type de fichier, rendues depuis du SVG.
//
// Elles servent à répondre d'un coup d'œil à « c'est quoi, ça ? » sur une liste
// de torrents dont les noms sont souvent illisibles — soixante caractères de
// balises entre crochets. Une icône de film ou de dossier compressé dit en un
// dixième de seconde ce qu'une lecture du nom ne dit pas toujours.
//
// Le jeu vient de Material Symbols (Google, licence Apache 2.0), figé dans
// source/ui/icons_data.cpp par tools/make_icons.py, et tramé au démarrage par
// nanosvg (licence zlib). Aucun fichier à côté du .nro : ce qui manque à
// l'exécution est ce qui casse chez les autres.
//
// Les icônes sont tramées en blanc et teintées à l'affichage. Une même icône
// sert donc en gris quand l'élément est inerte et en couleur d'accent quand il
// est actif, sans multiplier les textures.
#pragma once

#include <SDL2/SDL.h>

#include <cstddef>
#include <string>
#include <unordered_map>

namespace ui {

// Familles reconnues. Volontairement peu nombreuses : un utilisateur ne
// distingue pas trente icônes, il distingue « vidéo » de « archive ».
enum class IconKind {
    Folder,
    Video,
    Audio,
    Image,
    Archive,
    Document,
    Executable,
    Game,
    Downloading,  // torrent dont le contenu n'est pas encore connu
    Unknown,
    Paused,
    Done,
};

// Famille déduite du nom d'un fichier. Sur le chemin complet comme sur le seul
// nom : seule l'extension compte.
IconKind icon_kind_for(const std::string& filename);

struct IconSource {
    const char* name;
    const char* svg;
};

extern const IconSource kIconSources[];
extern const size_t kIconSourceCount;

// Fabrique et garde les textures. Une par couple (famille, taille) : l'interface
// n'en emploie que deux ou trois tailles, et une icône tramée puis agrandie se
// voit tout de suite.
class IconSet {
public:
    ~IconSet();

    void attach(SDL_Renderer* renderer) { renderer_ = renderer; }
    void clear();

    // Dessine l'icône dans un carré de `size` pixels, teintée.
    void draw(IconKind kind, int x, int y, int size, SDL_Color tint);

private:
    SDL_Texture* texture_for(IconKind kind, int size);

    SDL_Renderer* renderer_ = nullptr;
    std::unordered_map<uint32_t, SDL_Texture*> cache_;  // (famille << 16) | taille
};

}  // namespace ui
