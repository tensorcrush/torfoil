#include "bt/dht.hpp"

#include <algorithm>
#include <cstring>

#include "bt/bencode.hpp"
#include "util/clock.hpp"

namespace bt {

namespace {

// Nœuds d'amorçage : les trois que tous les clients utilisent. Ils ne servent
// qu'à entrer dans le réseau, ensuite on marche avec ses propres contacts.
struct Bootstrap {
    const char* host;
    uint16_t port;
};

const Bootstrap kBootstrap[] = {
    {"router.bittorrent.com", 6881},
    {"dht.transmissionbt.com", 6881},
    {"router.utorrent.com", 6881},
    {"dht.libtorrent.org", 25401},
};

constexpr size_t kMaxNodes = 400;
// En dessous, on considère qu'on n'a pas de quoi mener une recherche.
constexpr size_t kMinNodes = 16;
constexpr size_t kMaxQueriedPerSearch = 120;
constexpr uint64_t kSearchStepMs = 900;
constexpr uint64_t kSearchLifetimeMs = 90000;
// Tant qu'on n'a aucun nœud on réessaie vite, en changeant d'hôte à chaque fois.
constexpr uint64_t kBootstrapRetryMs = 2500;
constexpr uint64_t kPendingTimeoutMs = 12000;

// Distance XOR de Kademlia : « proche » veut dire proche de l'info_hash, pas
// proche géographiquement.
int compare_distance(const util::Hash160& a, const util::Hash160& b, const util::Hash160& target) {
    for (size_t i = 0; i < 20; ++i) {
        const uint8_t da = a[i] ^ target[i];
        const uint8_t db = b[i] ^ target[i];
        if (da != db) return da < db ? -1 : 1;
    }
    return 0;
}

std::string hash_to_string(const util::Hash160& h) {
    return std::string(reinterpret_cast<const char*>(h.data()), 20);
}

bool string_to_hash(const std::string& s, util::Hash160& out) {
    if (s.size() != 20) return false;
    std::memcpy(out.data(), s.data(), 20);
    return true;
}

}  // namespace

Dht::Dht() {
    util::random_bytes(my_id_.data(), my_id_.size());
}

Dht::~Dht() = default;

void Dht::set_transport(const std::shared_ptr<net::Transport>& transport) {
    transport_ = transport;
    sock_.reset();
    nodes_.clear();
    pending_.clear();
    last_bootstrap_ms_ = 0;

    // Les recherches en cours repartent de zéro : les nœuds connus l'étaient à
    // travers l'ancien chemin réseau, ils ne valent plus rien ici.
    for (auto& entry : searches_) {
        entry.second.queried.clear();
        entry.second.candidates.clear();
    }

    if (!transport_ || !transport_->ready()) return;

    auto sock = transport_->udp();
    if (sock && sock->open()) {
        sock_ = std::move(sock);
        local_port_ = 6881;
    }
}

void Dht::find_peers(const util::Hash160& info_hash) {
    const std::string key = util::to_hex(info_hash);
    Search& search = searches_[key];
    if (search.active && search.target == info_hash) return;

    search.target = info_hash;
    search.started_ms = util::now_ms();
    search.last_step_ms = 0;
    search.queried.clear();
    search.candidates.clear();
    search.active = true;
}

void Dht::forget(const util::Hash160& info_hash) {
    searches_.erase(util::to_hex(info_hash));
}

void Dht::fill_poll(net::PollItem& item) {
    item.udp = sock_.get();
    item.tcp = nullptr;
    item.want_read = sock_ != nullptr;
    item.want_write = false;
}

void Dht::on_poll(const net::PollItem& item, uint64_t now_ms) {
    if (!sock_ || !item.can_read) return;

    uint8_t buffer[2048];
    for (int i = 0; i < 32; ++i) {
        net::Endpoint from;
        const int n = sock_->recv_from(from, buffer, sizeof(buffer));
        if (n <= 0) break;
        ++stats_.datagrams_in;
        handle_datagram(from, buffer, static_cast<size_t>(n), now_ms);
    }
}

void Dht::tick(uint64_t now_ms) {
    if (!sock_) {
        // Le transport a pu redevenir disponible entre-temps (VPN remonté).
        if (transport_ && transport_->ready()) set_transport(transport_);
        return;
    }

    // Les requêtes sans réponse ne doivent pas s'accumuler indéfiniment.
    for (auto it = pending_.begin(); it != pending_.end();) {
        if (now_ms - it->second.sent_ms > kPendingTimeoutMs) it = pending_.erase(it);
        else ++it;
    }

    // Seuil, pas « table vide » : avec deux contacts on croit être amorcé alors
    // qu'on ne peut plus progresser — ils sont vite tous interrogés et la
    // recherche s'éteint. Tant qu'on est sous le seuil, on continue d'aller
    // chercher des contacts frais chez les routeurs.
    if (nodes_.size() < kMinNodes && now_ms - last_bootstrap_ms_ > kBootstrapRetryMs) {
        bootstrap(now_ms);
    }

    for (auto& entry : searches_) {
        Search& search = entry.second;
        if (!search.active) continue;

        if (now_ms - search.started_ms > kSearchLifetimeMs) {
            // Une recherche terminée n'est pas abandonnée : le DHT bouge, on la
            // relancera plus tard si le torrent manque toujours de pairs.
            search.active = false;
            continue;
        }
        if (now_ms - search.last_step_ms < kSearchStepMs) continue;
        search.last_step_ms = now_ms;
        step_search(search, now_ms);
    }
}

std::vector<std::pair<util::Hash160, std::vector<net::Endpoint>>> Dht::take_results() {
    std::vector<std::pair<util::Hash160, std::vector<net::Endpoint>>> out;
    out.swap(results_);
    return out;
}

void Dht::bootstrap(uint64_t now_ms) {
    last_bootstrap_ms_ = now_ms;
    if (!transport_) return;

    // UN SEUL hôte par passage. resolve() est bloquant et nous tournons sur le
    // thread moteur : enchaîner les quatre gelait tout le reste — pairs, pièces,
    // affichage — le temps qu'un résolveur lent rende la main.
    const size_t count = sizeof(kBootstrap) / sizeof(kBootstrap[0]);
    const Bootstrap& entry = kBootstrap[bootstrap_index_ % count];
    ++bootstrap_index_;

    uint32_t ip = 0;
    if (!transport_->resolve(entry.host, ip) || ip == 0) return;

    send_ping({ip, entry.port});

    // On enchaîne directement sur les recherches en attente : ces nœuds
    // répondront avec des contacts plus proches de nos cibles.
    for (auto& search : searches_) {
        if (search.second.active) send_get_peers({ip, entry.port}, search.second.target);
    }
}

void Dht::send_query(const net::Endpoint& ep, const std::string& query,
                     const std::string& args_bencode) {
    if (!sock_) return;

    char tid[2];
    tid[0] = static_cast<char>(next_tid_ & 0xff);
    tid[1] = static_cast<char>((next_tid_ >> 8) & 0xff);
    ++next_tid_;

    const std::string t(tid, 2);

    std::string message = "d1:ad";
    message += args_bencode;
    message += "e1:q";
    message += std::to_string(query.size()) + ":" + query;
    message += "1:t2:" + t;
    message += "1:y1:qe";

    sock_->send_to(ep, reinterpret_cast<const uint8_t*>(message.data()), message.size());

    ++stats_.queries_sent;
    Pending pending;
    pending.query =query;
    pending.sent_ms = util::now_ms();
    pending_[t] = pending;
}

void Dht::send_ping(const net::Endpoint& ep) {
    const std::string args = "2:id20:" + hash_to_string(my_id_);
    send_query(ep, "ping", args);
}

void Dht::send_get_peers(const net::Endpoint& ep, const util::Hash160& target) {
    std::string args = "2:id20:" + hash_to_string(my_id_);
    args += "9:info_hash20:" + hash_to_string(target);

    if (!sock_) return;

    char tid[2];
    tid[0] = static_cast<char>(next_tid_ & 0xff);
    tid[1] = static_cast<char>((next_tid_ >> 8) & 0xff);
    ++next_tid_;
    const std::string t(tid, 2);

    std::string message = "d1:ad" + args + "e1:q9:get_peers1:t2:" + t + "1:y1:qe";
    sock_->send_to(ep, reinterpret_cast<const uint8_t*>(message.data()), message.size());

    ++stats_.queries_sent;
    Pending pending;
    pending.query ="get_peers";
    pending.target = target;
    pending.sent_ms = util::now_ms();
    pending_[t] = pending;
}

void Dht::remember_node(const util::Hash160& id, const net::Endpoint& ep, uint64_t now_ms) {
    if (!ep.valid()) return;

    for (Node& node : nodes_) {
        if (node.endpoint == ep) {
            node.last_seen_ms = now_ms;
            return;
        }
    }
    if (nodes_.size() >= kMaxNodes) {
        // On évince le plus ancien plutôt que de refuser : le réseau change.
        auto oldest = std::min_element(nodes_.begin(), nodes_.end(),
                                       [](const Node& a, const Node& b) {
                                           return a.last_seen_ms < b.last_seen_ms;
                                       });
        *oldest = Node{id, ep, now_ms};
        return;
    }
    nodes_.push_back(Node{id, ep, now_ms});
}

void Dht::handle_datagram(const net::Endpoint& from, const uint8_t* data, size_t len,
                          uint64_t now_ms) {
    Value root;
    const std::string raw(reinterpret_cast<const char*>(data), len);
    if (!bdecode(raw, root, nullptr) || !root.is_dict()) return;

    const Value* type = root.find_str("y");
    if (!type) return;

    // On ne traite que les réponses : répondre aux requêtes des autres ferait de
    // la console un nœud de service, ce qu'elle n'a pas à être.
    if (type->s != "r") {
        if (type->s == "e") ++stats_.errors_in;
        return;
    }

    const Value* t = root.find_str("t");
    if (!t) return;

    const auto pending = pending_.find(t->s);
    if (pending == pending_.end()) {
        ++stats_.replies_unknown_tid;
        return;
    }
    ++stats_.replies_matched;
    const Pending info = pending->second;
    pending_.erase(pending);

    const Value* body = root.find_dict("r");
    if (!body) return;

    util::Hash160 their_id{};
    if (const Value* id = body->find_str("id")) string_to_hash(id->s, their_id);
    remember_node(their_id, from, now_ms);

    if (info.query != "get_peers") return;

    const std::string key = util::to_hex(info.target);
    const auto search_it = searches_.find(key);

    // « values » : la réponse qu'on espère — des pairs qui ont le torrent.
    if (const Value* values = body->find("values")) {
        std::vector<net::Endpoint> found;
        if (values->is_list()) {
            for (const Value& entry : values->l) {
                if (!entry.is_str() || entry.s.size() != 6) continue;
                const uint8_t* p = reinterpret_cast<const uint8_t*>(entry.s.data());
                net::Endpoint ep;
                ep.ipv4 = util::rd_be32(p);
                ep.port = util::rd_be16(p + 4);
                if (ep.valid()) found.push_back(ep);
            }
        }
        if (!found.empty()) {
            stats_.peers_returned += static_cast<uint32_t>(found.size());
            results_.emplace_back(info.target, found);
            if (search_it != searches_.end()) {
                for (const net::Endpoint& ep : found) search_it->second.found.push_back(ep);
            }
        }
    }

    // « nodes » : pas de pairs ici, mais des contacts plus proches de la cible.
    // C'est le pas de la recherche Kademlia.
    if (const Value* nodes = body->find_str("nodes")) {
        const size_t count = nodes->s.size() / 26;
        stats_.nodes_returned += static_cast<uint32_t>(count);
        const uint8_t* p = reinterpret_cast<const uint8_t*>(nodes->s.data());
        for (size_t i = 0; i < count; ++i) {
            const uint8_t* entry = p + i * 26;
            util::Hash160 id{};
            std::memcpy(id.data(), entry, 20);

            net::Endpoint ep;
            ep.ipv4 = util::rd_be32(entry + 20);
            ep.port = util::rd_be16(entry + 24);
            if (!ep.valid()) continue;

            ++stats_.nodes_kept;
            remember_node(id, ep, now_ms);
            if (search_it != searches_.end()) {
                search_it->second.candidates.push_back(Node{id, ep, now_ms});
            }
        }
    }
}

void Dht::step_search(Search& search, uint64_t now_ms) {
    if (search.queried.size() >= kMaxQueriedPerSearch) {
        search.active = false;
        return;
    }

    // On réinjecte à chaque pas les nœuds appris entre-temps. Se contenter de
    // remplir quand la liste est vide fige la recherche : tous les candidats
    // finissent interrogés et plus rien ne repart, alors que la table, elle,
    // continue de grossir.
    for (const Node& node : nodes_) {
        const bool known = std::any_of(search.candidates.begin(), search.candidates.end(),
                                       [&](const Node& c) {
                                           return c.endpoint == node.endpoint;
                                       });
        if (!known) search.candidates.push_back(node);
    }

    if (search.candidates.empty()) {
        if (now_ms - last_bootstrap_ms_ > kBootstrapRetryMs) bootstrap(now_ms);
        return;
    }

    // Les plus proches de la cible d'abord : c'est tout l'intérêt de Kademlia,
    // on converge en O(log n) au lieu de balayer le réseau.
    std::sort(search.candidates.begin(), search.candidates.end(),
              [&](const Node& a, const Node& b) {
                  return compare_distance(a.id, b.id, search.target) < 0;
              });

    int sent = 0;
    for (const Node& node : search.candidates) {
        if (sent >= 4) break;
        const bool already = std::find(search.queried.begin(), search.queried.end(),
                                       node.endpoint.ipv4) != search.queried.end();
        if (already) continue;

        search.queried.push_back(node.endpoint.ipv4);
        send_get_peers(node.endpoint, search.target);
        ++sent;
    }

    // Plus rien de neuf à interroger : la recherche a convergé. On la met en
    // sommeil plutôt que de tourner à vide ; la session la relancera si le
    // torrent manque encore de pairs, et le DHT aura bougé d'ici là.
    if (sent == 0) search.active = false;

    // On borne la mémoire, mais seulement au-delà de ce qui reste utile.
    if (search.candidates.size() > 256) {
        search.candidates.erase(search.candidates.begin() + 256, search.candidates.end());
    }
}

}  // namespace bt
