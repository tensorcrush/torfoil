// Cherche des pairs sur le vrai DHT, depuis un PC, avec le code embarqué.
//
//   ./dht_live "magnet:?xt=urn:btih:..."
//
// Répond à la question qui compte quand un torrent reste à 0 pair : est-ce le
// torrent, le réseau, ou mon code ?

#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <string>

#include "bt/dht.hpp"
#include "bt/magnet.hpp"
#include "net/transport.hpp"
#include "util/clock.hpp"

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IOLBF, 0);

    if (argc < 2) {
        std::printf("usage: %s <lien magnet | info_hash hexadécimal>\n", argv[0]);
        return 2;
    }

    util::Hash160 info_hash{};
    const std::string arg = argv[1];

    // On réaffiche l'argument tel qu'il est arrivé : sous PowerShell, un magnet
    // sans guillemets est coupé au premier « & », et l'erreur qui suit est
    // incompréhensible si on ne voit pas ce qui a réellement été reçu.
    std::printf("reçu     : %s\n", arg.c_str());
    if (arg.rfind("magnet:", 0) == 0 && arg.find("&") == std::string::npos &&
        arg.find("dn=") == std::string::npos) {
        std::printf("           (aucun « & » : le lien est peut-être tronqué —\n");
        std::printf("            entoure-le de guillemets)\n");
    }

    if (arg.rfind("magnet:", 0) == 0) {
        bt::MagnetLink link;
        std::string err;
        if (!bt::parse_magnet(arg, link, &err)) {
            std::printf("\n\033[31mLien refusé : %s\033[0m\n", err.c_str());
            return 1;
        }
        info_hash = link.info_hash;
        std::printf("torrent  : %s\n",
                    link.display_name.empty() ? "(sans nom)" : link.display_name.c_str());
        std::printf("trackers : %zu annoncés dans le magnet\n", link.trackers.size());
    } else if (!util::from_hex(arg, info_hash.data(), info_hash.size())) {
        std::printf("\n\033[31mCe n'est ni un lien magnet ni un info_hash de 40 caractères "
                    "hexadécimaux.\033[0m\n");
        return 2;
    }

    std::printf("info_hash: %s\n\n", util::to_hex(info_hash).c_str());

    std::shared_ptr<net::Transport> transport = net::make_bsd_transport();

    bt::Dht dht;
    dht.set_transport(transport);
    if (!dht.ready()) {
        std::printf("\n\033[31mSocket UDP indisponible.\033[0m\n");
        return 1;
    }
    dht.find_peers(info_hash);

    const uint64_t started = util::now_ms();
    size_t total_peers = 0;
    uint64_t last_report = 0;

    // 60 s : au-delà, si le DHT n'a rien donné, c'est que personne ne partage ce
    // torrent ou que l'UDP sortant est filtré.
    while (util::now_ms() - started < 60000) {
        const uint64_t now = util::now_ms();

        // Même boucle que la session : fill_poll → poll → on_poll → tick.
        net::PollItem items[1];
        dht.fill_poll(items[0]);
        transport->poll(items, 1, 200);
        dht.on_poll(items[0], now);

        // Comme la session : on redemande à chaque tour. Une recherche qui a
        // épuisé ses candidats se met en sommeil ; sans cette relance elle ne
        // repart jamais et le test conclut à tort que le torrent est mort.
        dht.find_peers(info_hash);
        dht.tick(now);

        for (const auto& result : dht.take_results()) {
            if (result.first != info_hash) continue;
            total_peers += result.second.size();
            for (const net::Endpoint& ep : result.second) {
                std::printf("  pair : %s\n", ep.to_string().c_str());
            }
        }

        if (now - last_report >= 5000) {
            last_report = now;
            const bt::Dht::Stats& s = dht.stats();
            std::printf("[%2" PRIu64 " s] %zu nœuds · %zu pairs | envoyées %u, reçues %u, "
                        "appariées %u, tid inconnu %u, erreurs %u, nœuds rendus %u\n",
                        (now - started) / 1000, dht.node_count(), total_peers, s.queries_sent,
                        s.datagrams_in, s.replies_matched, s.replies_unknown_tid, s.errors_in,
                        s.nodes_returned);
            std::printf("        nœuds retenus %u\n", s.nodes_kept);
        }
    }

    std::printf("\n");
    if (total_peers > 0) {
        std::printf("\033[32m%zu pairs trouvés sans aucun tracker : le DHT fonctionne.\033[0m\n",
                    total_peers);
        return 0;
    }
    if (dht.node_count() > 0) {
        std::printf("\033[33mLe DHT répond (%zu nœuds) mais personne ne partage ce torrent.\033[0m\n",
                    dht.node_count());
        std::printf("Le code marche ; c'est le torrent qui est mort.\n");
        return 0;
    }
    std::printf("\033[31mAucun nœud DHT joignable : UDP sortant bloqué, ou bug d'amorçage.\033[0m\n");
    return 1;
}
