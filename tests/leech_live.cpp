// Téléchargement réel de bout en bout, sur PC, avec le moteur embarqué.
//
//   ./leech_live <lien magnet | fichier .torrent> [secondes]
//
// C'est le test qui manquait : trouver des pairs ne prouve pas qu'on sait leur
// parler. Celui-ci exerce la chaîne complète — DHT/trackers, connexion aux
// pairs, poignée de main, métadonnées (BEP 9), choix des pièces, vérification
// SHA-1, écriture disque. Tout sauf libnx, l'affichage et l'installation.

#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

#include "bt/session.hpp"
#include "net/transport.hpp"
#include "util/bytes.hpp"
#include "util/clock.hpp"
#include "util/log.hpp"

namespace {

std::string human(uint64_t bytes) {
    char buf[32];
    if (bytes >= 1024ull * 1024 * 1024) {
        std::snprintf(buf, sizeof(buf), "%.2f Go", bytes / (1024.0 * 1024 * 1024));
    } else if (bytes >= 1024 * 1024) {
        std::snprintf(buf, sizeof(buf), "%.1f Mo", bytes / (1024.0 * 1024));
    } else if (bytes >= 1024) {
        std::snprintf(buf, sizeof(buf), "%.1f ko", bytes / 1024.0);
    } else {
        std::snprintf(buf, sizeof(buf), "%" PRIu64 " o", bytes);
    }
    return buf;
}

}  // namespace

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IOLBF, 0);

    if (argc < 2) {
        std::printf("usage: %s <lien magnet | fichier .torrent> [secondes]\n", argv[0]);
        return 2;
    }

    const std::string source = argv[1];
    const int duration_s = (argc > 2) ? std::atoi(argv[2]) : 120;
    const std::string dir = "/tmp/torfoil-leech";

    std::printf("source   : %s\n", source.c_str());
    std::printf("dossier  : %s\n", dir.c_str());
    std::printf("durée    : %d s\n\n", duration_s);

    // Même journal que sur console : c'est lui qu'on relit quand quelque chose
    // échoue, autant l'éprouver ici.
    util::log_open(dir + "-torfoil.log");
    std::printf("journal   : %s-torfoil.log\n\n", dir.c_str());

    bt::Session session;
    std::string err;
    if (!session.start(dir, &err)) {
        std::printf("\033[31mdémarrage impossible : %s\033[0m\n", err.c_str());
        return 1;
    }

    const bool added = (source.rfind("magnet:", 0) == 0)
                           ? session.add_magnet(source, &err)
                           : session.add_torrent_file(source, &err);
    if (!added) {
        std::printf("\033[31m%s\033[0m\n", err.c_str());
        session.stop();
        return 1;
    }

    // Ce qu'on cherche à observer, dans l'ordre où ça doit arriver.
    bool saw_peer = false;
    bool saw_metadata = false;
    bool saw_bytes = false;
    uint64_t best_downloaded = 0;
    uint32_t best_peers = 0;

    const uint64_t started = util::now_ms();
    uint64_t last_report = 0;

    while (util::now_ms() - started < static_cast<uint64_t>(duration_s) * 1000) {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));

        const auto snapshot = session.snapshot();
        if (snapshot.empty()) continue;
        const bt::TorrentStatus& t = snapshot.front();

        if (t.peers_connected > 0) saw_peer = true;
        if (t.state != bt::TorrentState::FetchingMetadata) saw_metadata = true;
        if (t.downloaded > 0) saw_bytes = true;
        best_downloaded = std::max(best_downloaded, t.downloaded);
        best_peers = std::max(best_peers, t.peers_connected);

        const uint64_t now = util::now_ms();
        if (now - last_report < 5000) continue;
        last_report = now;

        std::printf("[%3" PRIu64 " s] %-16s %5.1f%%  %s  ↓%s/s  pairs %u (%u nous étranglent) "
                    "/ %u connus  blocs en vol %u%s%s\n",
                    (now - started) / 1000, bt::state_label(t.state), t.progress * 100.0f,
                    human(t.downloaded).c_str(), human(t.rate_down).c_str(), t.peers_connected,
                    t.peers_choking, t.peers_known, t.blocks_in_flight,
                    t.message.empty() ? "" : "  — ", t.message.c_str());

        if (t.state == bt::TorrentState::Completed || t.state == bt::TorrentState::Seeding) {
            std::printf("\nterminé avant la fin du chrono.\n");
            break;
        }
    }

    session.stop();
    util::log_close();

    std::printf("\n--- bilan ---\n");
    std::printf("  %s pairs connectés          (max %u)\n", saw_peer ? "\033[32mOK   \033[0m"
                                                                     : "\033[31mÉCHEC\033[0m",
                best_peers);
    std::printf("  %s métadonnées récupérées\n",
                saw_metadata ? "\033[32mOK   \033[0m" : "\033[31mÉCHEC\033[0m");
    std::printf("  %s données reçues et vérifiées (%s)\n",
                saw_bytes ? "\033[32mOK   \033[0m" : "\033[31mÉCHEC\033[0m",
                human(best_downloaded).c_str());

    if (saw_peer && saw_metadata && saw_bytes) {
        std::printf("\n\033[32mLa chaîne complète fonctionne : pairs, protocole, pièces, "
                    "écriture disque.\033[0m\n");
        return 0;
    }
    if (!saw_peer) {
        std::printf("\nAucune connexion établie. Les pairs sont trouvés mais injoignables :\n"
                    "TCP sortant filtré, ou bug de connexion.\n");
    } else if (!saw_metadata) {
        std::printf("\nConnecté mais aucune métadonnée : voir la poignée de main étendue\n"
                    "(BEP 10) et ut_metadata (BEP 9).\n");
    } else {
        std::printf("\nMétadonnées obtenues mais aucun bloc reçu : voir le choix des pièces\n"
                    "et les requêtes de blocs.\n");
    }
    return 1;
}
