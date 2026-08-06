// Transport tunnelé : pile lwIP au-dessus d'un tunnel WireGuard.
//
// C'est l'implémentation « VPN » de net::Transport. Le moteur BitTorrent ne
// change pas d'une ligne : il demande des sockets, elles sortent chiffrées.
// Si le tunnel tombe, ready() renvoie false et plus aucune socket ne naît.
#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "net/transport.hpp"

namespace wg {

struct TunnelConfig {
    // Relais Mullvad
    uint32_t endpoint_ip = 0;  // ordre hôte
    uint16_t endpoint_port = 51820;
    uint8_t server_public[32]{};
    std::string server_label;

    // Notre identité et l'adresse attribuée par Mullvad
    uint8_t private_key[32]{};
    uint32_t assigned_ip = 0;
    uint32_t netmask = 0xffffffffu;
    uint32_t dns_ip = 0;  // résolveur Mullvad, typiquement 10.64.0.1
};

enum class TunnelState {
    Down,
    Handshaking,
    Up,
    Failed,
};

class Tunnel : public net::Transport {
public:
    virtual ~Tunnel() = default;

    virtual bool start(std::string* err) = 0;
    virtual void stop() = 0;

    virtual TunnelState state() const = 0;
    virtual std::string status_text() const = 0;
    virtual uint64_t bytes_sent() const = 0;
    virtual uint64_t bytes_received() const = 0;
    virtual uint64_t last_handshake_age_ms() const = 0;
};

// Une seule instance à la fois : lwIP en mode NO_SYS est un état global.
std::shared_ptr<Tunnel> make_tunnel(const TunnelConfig& config, std::string* err);

}  // namespace wg
