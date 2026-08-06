// Abstraction réseau du projet — LA pièce qui rend le killswitch structurel.
//
// Tout le moteur BitTorrent passe par un Transport. Il en existe deux :
//   * BsdTransport   — sockets système libnx, trafic en clair.
//   * WireGuardTransport (M2) — pile lwIP au-dessus du tunnel Mullvad.
//
// Le moteur ne sait pas lequel il utilise et n'a aucun moyen d'ouvrir une socket
// autrement. Quand le VPN est exigé et que le tunnel tombe, ready() renvoie
// false et plus une seule connexion ne peut naître : il n'y a pas de chemin de
// repli à désactiver, il n'y en a simplement pas.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace net {

struct Endpoint {
    uint32_t ipv4 = 0;  // ordre hôte
    uint16_t port = 0;

    bool valid() const { return ipv4 != 0 && port != 0; }
    std::string to_string() const;
    bool operator==(const Endpoint& o) const { return ipv4 == o.ipv4 && port == o.port; }
};

// Codes de retour communs à send/recv.
enum : int {
    kWouldBlock = -1,  // rien à lire / tampon d'émission plein, réessayer plus tard
    kClosed = -2,      // pair déconnecté proprement
    kError = -3,       // socket morte
};

class TcpSocket {
public:
    virtual ~TcpSocket() = default;

    // Lance la connexion sans bloquer. Le résultat s'observe via poll() :
    // « prêt en écriture » = connecté, « erreur » = échec.
    virtual bool start_connect(const Endpoint& ep) = 0;
    virtual bool finish_connect() = 0;  // à appeler quand poll signale l'écriture

    virtual int send(const uint8_t* data, size_t len) = 0;
    virtual int recv(uint8_t* data, size_t len) = 0;
    virtual void close() = 0;
    virtual bool is_open() const = 0;
};

class UdpSocket {
public:
    virtual ~UdpSocket() = default;

    virtual bool open() = 0;
    virtual int send_to(const Endpoint& ep, const uint8_t* data, size_t len) = 0;
    virtual int recv_from(Endpoint& from, uint8_t* data, size_t len) = 0;
    virtual void close() = 0;
    virtual bool is_open() const = 0;
};

// Élément d'attente pour poll(). `want_*` en entrée, `can_*` en sortie.
struct PollItem {
    TcpSocket* tcp = nullptr;
    UdpSocket* udp = nullptr;
    bool want_read = false;
    bool want_write = false;
    bool can_read = false;
    bool can_write = false;
    bool has_error = false;
};

class Transport {
public:
    virtual ~Transport() = default;

    virtual const char* name() const = 0;

    // false = aucune socket ne peut être créée (tunnel absent ou tombé).
    virtual bool ready() const = 0;

    virtual std::unique_ptr<TcpSocket> tcp() = 0;
    virtual std::unique_ptr<UdpSocket> udp() = 0;

    // Résolution DNS. Sur le transport tunnelé, la requête part *dans* le tunnel
    // (vers le résolveur Mullvad) : pas de fuite DNS vers le FAI.
    virtual bool resolve(const std::string& host, uint32_t& ipv4_out) = 0;

    // Renvoie le nombre d'éléments prêts, 0 sur expiration, -1 sur erreur.
    virtual int poll(PollItem* items, size_t count, int timeout_ms) = 0;

    // Adresse publique telle que vue depuis l'extérieur, si connue (affichage).
    virtual std::string public_address() const { return {}; }
};

// Transport en clair, sockets système. Utilisé quand le VPN est désactivé.
std::unique_ptr<Transport> make_bsd_transport();

}  // namespace net
