// Installation d'un NSP/XCI dans le stockage de la console.
//
// Déroulé : lire le CNMT → recopier chaque NCA dans un « placeholder » ncm →
// l'enregistrer → écrire l'entrée de métadonnées → pousser l'enregistrement
// applicatif auprès de ns, ce qui fait apparaître la tuile sur le menu HOME.
#pragma once

#include <cstdint>
#include <functional>
#include <string>

namespace install {

enum class Target { Sd, Nand };

struct Progress {
    std::string step;
    uint64_t done = 0;
    uint64_t total = 0;
    // Renvoyer false depuis le rappel annule l'installation.
};

using ProgressFn = std::function<bool(const Progress&)>;

// Volontairement pas nommé « Result » : libnx occupe déjà ce nom pour ses codes
// d'erreur, et la confusion serait garantie dans ce fichier.
struct Outcome {
    bool ok = false;
    std::string message;
    uint64_t title_id = 0;
    std::string title_id_hex;
};

// `path` : un .nsp ou un .xci sur la carte SD.
Outcome install_package(const std::string& path, Target target, const ProgressFn& progress);

// Rejoue toute la chaîne d'installation SANS rien écrire dans la mémoire
// système : clés, conteneur, déchiffrement du NCA meta, CNMT, présence et
// taille de chaque contenu, hachage SHA-256 de chacun s'il est demandé.
//
// C'est la façon de savoir si un paquet téléchargé est sain avant de lui
// laisser toucher le stockage de la console — et, quand une installation
// échoue, de distinguer un paquet corrompu d'un bug d'installation.
Outcome verify_package(const std::string& path, bool deep, const ProgressFn& progress);

// Même chose avec un jeu de clés fourni : c'est cette forme que les tests
// utilisent, avec des clés fabriquées pour l'occasion.
class KeySet;
Outcome verify_package_with_keys(const std::string& path, const KeySet& keys, bool deep,
                                 const ProgressFn& progress);

// Vérifie la présence de prod.keys sans rien installer.
bool keys_available(std::string* err);

}  // namespace install
