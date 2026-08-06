// Annonces tracker : HTTP (BEP 3, mode compact BEP 23) et UDP (BEP 15).
//
// Une annonce est ponctuelle et rare (toutes les ~30 min). Elle s'exécute donc
// de façon bloquante sur le thread de session plutôt qu'en machine à états, ce
// qui garde le code lisible ; le moteur pair-à-pair, lui, reste non bloquant.
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "net/transport.hpp"
#include "util/bytes.hpp"

namespace bt {

enum class AnnounceEvent { None, Started, Completed, Stopped };

struct AnnounceRequest {
    util::Hash160 info_hash{};
    std::array<uint8_t, 20> peer_id{};
    uint16_t port = 6881;
    uint64_t uploaded = 0;
    uint64_t downloaded = 0;
    uint64_t left = 0;
    AnnounceEvent event = AnnounceEvent::None;
    uint32_t num_want = 60;
    uint32_t key = 0;  // identifie le client entre deux changements d'IP
};

struct AnnounceResult {
    bool ok = false;
    std::string failure;
    uint32_t interval_s = 1800;
    uint32_t seeders = 0;
    uint32_t leechers = 0;
    std::vector<net::Endpoint> peers;
};

class TrackerClient {
public:
    explicit TrackerClient(net::Transport& transport) : transport_(transport) {}

    // `url` accepte http://, https:// (via TLS quand disponible) et udp://.
    AnnounceResult announce(const std::string& url, const AnnounceRequest& req,
                            int timeout_ms = 15000);

private:
    AnnounceResult announce_http(const std::string& url, const AnnounceRequest& req,
                                 int timeout_ms);
    AnnounceResult announce_udp(const std::string& url, const AnnounceRequest& req,
                                int timeout_ms);

    net::Transport& transport_;
};

// Génère un peer_id style Azureus : « -TF0100- » + 12 octets aléatoires.
std::array<uint8_t, 20> make_peer_id();

}  // namespace bt
