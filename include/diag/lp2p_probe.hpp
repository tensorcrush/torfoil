// Banc d'essai du point d'accès Wi-Fi (lp2p).
//
// Séparé du reste du diagnostic parce qu'il ne répond pas à « est-ce que ça
// marche » mais à « pourquoi ça ne marche pas » : il crée puis détruit
// plusieurs réseaux d'affilée, ce qu'on ne veut pas voir tourner dans un
// auto-test ordinaire.
#pragma once

#include <string>
#include <vector>

namespace diag {

struct Lp2pProbeStep {
    std::string label;
    bool ok = false;
    std::string detail;
};

struct Lp2pProbeReport {
    std::vector<Lp2pProbeStep> steps;
};

// Essaie la matrice complète et rend un compte rendu, une ligne par essai.
// Aucun réseau ne survit à l'appel.
Lp2pProbeReport probe_lp2p();

}  // namespace diag
