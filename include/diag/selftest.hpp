// Auto-diagnostic exécuté sur la console.
//
// Trois choses ne peuvent être vérifiées que là : qu'une carte SD accepte un
// fichier de plus de 4 Go, que les services d'installation répondent, et que le
// trafic sort réellement par le tunnel. Aucun test sur PC, aucun émulateur ne
// s'y substitue.
//
// Plutôt que de laisser ces zones d'ombre, l'application se teste elle-même :
// chaque épreuve est courte, non destructrice, et nettoie derrière elle.
#pragma once

#include <functional>
#include <string>
#include <vector>

namespace bt {
class Session;
}
namespace vpn {
class Manager;
}

namespace diag {

struct Check {
    std::string name;
    bool ok = false;
    bool ran = false;
    std::string detail;
};

struct Report {
    std::vector<Check> checks;
    bool all_ok() const {
        for (const Check& c : checks) {
            if (c.ran && !c.ok) return false;
        }
        return true;
    }
};

// Rappel d'avancement : reçoit le nom de l'épreuve en cours.
using StepFn = std::function<void(const std::string&)>;

// Vérifie que la carte accepte un fichier dépassant la limite de FAT32, en
// écrivant puis relisant au-delà de 4 Go. Le fichier est supprimé ensuite.
Check check_large_file(const std::string& dir, const StepFn& step);

// Réserve un emplacement ncm, y écrit, relit, puis le supprime. Rien n'est
// enregistré : aucune trace ne subsiste, mais on sait si l'installation
// pourrait écrire.
Check check_install_services(const StepFn& step);

// Interroge am.i.mullvad.net à travers le transport actif. C'est Mullvad qui
// répond, pas nous : la réponse tranche la question du tunnel.
Check check_vpn_exit(bt::Session& session, const StepFn& step);

Report run_all(const std::string& download_dir, bt::Session& session, const StepFn& step);

}  // namespace diag
