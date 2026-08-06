// DHT Kademlia (BEP 5) — trouver des pairs sans tracker.
//
// Pourquoi c'est indispensable : un lien magnet ne contient qu'un info_hash et,
// au mieux, quelques trackers. Sur un client PC, la plupart des pairs viennent
// en réalité du DHT et de PEX ; c'est ce qui fait qu'un torrent « marche » même
// quand ses trackers sont morts ou bloqués. Sans DHT, un tracker injoignable
// signifie zéro pair.
//
// Implémentation volontairement réduite : on cherche des pairs (get_peers), on
// s'annonce, on répond au strict minimum pour rester joignable. Pas de stockage
// de valeurs pour les autres : la console n'est pas un nœud d'infrastructure.
#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "net/transport.hpp"
#include "util/bytes.hpp"

namespace bt {

class Dht {
public:
    Dht();
    ~Dht();

    // Le transport change quand le VPN monte ou tombe : les requêtes DHT
    // doivent suivre, sans quoi elles fuiraient hors du tunnel.
    void set_transport(const std::shared_ptr<net::Transport>& transport);

    // Recherche des pairs pour cet info_hash. Appels répétés sans risque.
    void find_peers(const util::Hash160& info_hash);
    void forget(const util::Hash160& info_hash);

    void fill_poll(net::PollItem& item);
    void on_poll(const net::PollItem& item, uint64_t now_ms);
    void tick(uint64_t now_ms);

    // Pairs découverts depuis le dernier appel, puis vidés.
    std::vector<std::pair<util::Hash160, std::vector<net::Endpoint>>> take_results();

    bool ready() const { return sock_ != nullptr; }
    size_t node_count() const { return nodes_.size(); }

    // Compteurs de diagnostic : sans eux, « 0 pair » ne dit pas si les requêtes
    // partent, si les réponses reviennent, ou si elles sont mal comprises.
    struct Stats {
        uint32_t queries_sent = 0;
        uint32_t datagrams_in = 0;
        uint32_t replies_matched = 0;
        uint32_t replies_unknown_tid = 0;
        uint32_t errors_in = 0;
        uint32_t nodes_returned = 0;
        uint32_t nodes_kept = 0;
        uint32_t peers_returned = 0;
    };
    const Stats& stats() const { return stats_; }

    // Port UDP local, à annoncer aux pairs (message PORT, BEP 5).
    uint16_t port() const { return local_port_; }

private:
    struct Node {
        util::Hash160 id{};
        net::Endpoint endpoint;
        uint64_t last_seen_ms = 0;
    };

    struct Search {
        util::Hash160 target{};
        uint64_t started_ms = 0;
        uint64_t last_step_ms = 0;
        // Nœuds déjà interrogés : sans ça une recherche boucle sur elle-même.
        std::vector<uint32_t> queried;
        std::vector<Node> candidates;
        std::vector<net::Endpoint> found;
        bool active = false;
    };

    void bootstrap(uint64_t now_ms);
    void send_ping(const net::Endpoint& ep);
    void send_get_peers(const net::Endpoint& ep, const util::Hash160& target);
    void send_query(const net::Endpoint& ep, const std::string& query,
                    const std::string& args_bencode);
    void handle_datagram(const net::Endpoint& from, const uint8_t* data, size_t len,
                         uint64_t now_ms);
    void remember_node(const util::Hash160& id, const net::Endpoint& ep, uint64_t now_ms);
    void step_search(Search& search, uint64_t now_ms);

    std::shared_ptr<net::Transport> transport_;
    std::unique_ptr<net::UdpSocket> sock_;
    uint16_t local_port_ = 0;

    util::Hash160 my_id_{};
    std::vector<Node> nodes_;
    std::map<std::string, Search> searches_;  // clé = info_hash hexadécimal

    // Corrélation requête → réponse : le champ « t » du protocole.
    struct Pending {
        std::string query;
        util::Hash160 target{};
        uint64_t sent_ms = 0;
    };
    std::map<std::string, Pending> pending_;
    uint32_t next_tid_ = 1;

    uint64_t last_bootstrap_ms_ = 0;
    size_t bootstrap_index_ = 0;
    Stats stats_;

    std::vector<std::pair<util::Hash160, std::vector<net::Endpoint>>> results_;
};

}  // namespace bt
