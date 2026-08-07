// Langues de l'interface.
//
// Sept langues, une table unique (ui/strings.def), et une règle : au premier
// lancement on prend celle de la console. Un homebrew qui démarre en français
// sur une console réglée en allemand donne l'impression d'être cassé avant même
// d'avoir servi à quelque chose.
//
// Les textes du moteur (erreurs de stockage, refus d'un paquet NCA, échecs
// d'annonce) restent en français : ils partent dans le journal et servent aux
// rapports de bug, pas à l'usage courant. Ce qui s'affiche dans l'interface,
// lui, est intégralement traduit.
#pragma once

#include <cstdint>
#include <string>

namespace ui {

enum class Lang : uint8_t { De, En, Es, Fr, Ja, Ru, Zh, kCount };

// Une clé par texte affichable ; la liste et les traductions vivent ensemble.
enum class Str : uint16_t {
#define X(name, ...) name,
#include "ui/strings.def"
#undef X
    kCount
};

// Texte dans la langue active. Ne renvoie jamais nullptr.
const char* tr(Str key);

// Même chose, mais avec les marqueurs de format remplis. Les traductions
// gardent les mêmes marqueurs dans le même ordre, ce qui rend l'appel sûr d'une
// langue à l'autre.
std::string trf(Str key, ...);

void set_language(Lang lang);
Lang language();

// Langue réglée dans les paramètres système de la console. Renvoie l'anglais
// quand la console répond une langue qu'on ne parle pas — plutôt que le
// français, qui n'aiderait qu'une personne sur sept.
Lang console_language();

// Codes courts persistés dans settings.cfg : « de », « en »… et « auto ».
const char* code_of(Lang lang);
bool lang_from_code(const std::string& code, Lang& out);

// Nom de la langue écrit dans cette langue — c'est ainsi qu'on retrouve la
// sienne dans une liste qu'on ne sait pas lire.
const char* endonym(Lang lang);

}  // namespace ui
