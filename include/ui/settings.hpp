// Réglages de confidentialité, persistés sur la carte.
//
// Chacun est un compromis explicite entre discrétion et débit. Rien n'est activé
// « pour la forme » : une option qui ne change rien n'aurait pas sa place ici, et
// une option qui protège coûte toujours quelque chose.
#pragma once

#include <string>
#include <vector>

#include "ui/lang.hpp"

namespace ui {

struct Settings {
    // « auto » suit la langue de la console, tout autre code la fige. Stocké en
    // texte plutôt qu'en indice : un fichier de réglages doit rester lisible et
    // réparable à la main, et un numéro d'énumération ne survivrait pas à
    // l'ajout d'une langue au milieu de la liste.
    std::string language = "auto";

    // Langue réellement à appliquer, « auto » résolu.
    Lang effective_language() const;

    // Aucune connexion si le tunnel n'est pas debout. C'est le killswitch rendu
    // obligatoire : sans lui, couper le VPN laisse le trafic repartir en clair.
    bool require_vpn = false;

    // Interdit les trackers en http:// simple. Une annonce en clair révèle
    // l'info_hash — donc ce qu'on télécharge — à quiconque observe la ligne.
    bool https_trackers_only = false;

    // Le DHT trouve des pairs sans tracker, mais ses requêtes UDP sont en clair
    // et diffusées largement. Le couper rend plus discret et bien plus lent.
    bool enable_dht = true;

    // PEX échange des carnets d'adresses avec les pairs : très efficace, mais
    // fait savoir à chacun d'eux ce que l'on connaît de l'essaim.
    bool enable_pex = true;

    // Ne rien partager. Réduit la visibilité, au prix d'être servi plus
    // rarement — les pairs favorisent ceux qui rendent la pareille.
    bool no_upload = false;

    // Serveur web de réseau local, pour envoyer liens et .torrent depuis un
    // téléphone. Activé par défaut : c'est la seule façon commode de faire
    // entrer un fichier .torrent dans la console, et il n'expose rien au-delà
    // du réseau local. Se coupe d'un geste pour qui préfère.
    bool phone_import = true;

    bool load(const std::string& path);
    bool save(const std::string& path) const;
};

// Description d'une case pour l'affichage : libellé, explication, et accès à la
// valeur. Les textes sont des clés, pas des chaînes : changer de langue ne doit
// pas demander de reconstruire cette liste.
struct Toggle {
    Str label;
    Str effect;  // ce que l'option coûte, en une ligne
    bool Settings::*field;
    bool inverted;  // vrai si cocher signifie « désactiver »
};

const std::vector<Toggle>& toggles();

}  // namespace ui
