// Chargement de prod.keys.
//
// Déchiffrer l'en-tête d'un NCA et sa zone de clés demande deux familles de
// clés que seule la console possède. On les lit dans le prod.keys déjà présent
// sur la carte SD (tout CFW en a un) plutôt que de les redériver via spl : le
// chemin est explicite et vérifiable.
#pragma once

#include <cstdint>
#include <string>

namespace install {

class KeySet {
public:
    static constexpr int kGenerations = 0x20;

    // Cherche prod.keys aux emplacements habituels d'un CFW.
    bool load(std::string* err = nullptr);
    // Charge un fichier de clés précis. Sert aux tests, qui fabriquent un jeu
    // de clés connu pour vérifier tout le déchiffrement sans prod.keys réel.
    bool load_from(const std::string& path, std::string* err = nullptr);
    bool loaded() const { return loaded_; }
    const std::string& source_path() const { return source_path_; }

    // 32 octets : deux clés AES-128 pour le XTS de l'en-tête NCA.
    const uint8_t* header_key() const { return has_header_key_ ? header_key_ : nullptr; }

    // 16 octets, indexée par génération de clé maître.
    const uint8_t* key_area_key_application(uint8_t generation) const;
    const uint8_t* titlekek(uint8_t generation) const;

private:
    bool parse_file(const std::string& path);

    bool loaded_ = false;
    std::string source_path_;

    uint8_t header_key_[32]{};
    bool has_header_key_ = false;

    uint8_t kaek_app_[kGenerations][16]{};
    bool has_kaek_app_[kGenerations]{};

    uint8_t titlekek_[kGenerations][16]{};
    bool has_titlekek_[kGenerations]{};
};

}  // namespace install
