// Serveur HTTP de réseau local — la porte d'entrée depuis un téléphone.
//
// Pourquoi il n'utilise PAS net::Transport, contrairement à tout le reste :
// Transport est le chemin de sortie du moteur, celui que le VPN remplace. Ce
// serveur, lui, ne sort de nulle part. Il écoute sur l'interface Wi-Fi de la
// console et ne parle qu'à ce qui se connecte à lui, depuis le même réseau. Le
// faire passer par le tunnel n'aurait aucun sens : le tunnel mène chez Mullvad,
// pas dans le salon.
//
// Il ne transporte aucun octet de torrent, dans aucun sens. La seule chose qui
// entre par là, ce sont des liens et des fichiers .torrent, c'est-à-dire des
// noms de contenus — jamais du contenu. Le killswitch reste donc entier : ce
// qu'on télécharge continue de ne pouvoir sortir que par le Transport actif.
//
// Deux garde-fous, parce qu'un serveur qui accepte d'ajouter des torrents ne
// doit pas être ouvert à toute la planète :
//   * seules les adresses privées (RFC 1918, lien-local) sont servies ;
//   * le corps d'une requête est plafonné.
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "net/http_parse.hpp"

namespace net {

struct HttpReply {
    int status = 200;
    std::string content_type = "text/html; charset=utf-8";
    std::string body;
    std::string extra_headers;  // lignes complètes, terminées par \r\n
};

using HttpHandler = std::function<HttpReply(const HttpRequest&)>;

class HttpServer {
public:
    HttpServer() = default;
    ~HttpServer();

    HttpServer(const HttpServer&) = delete;
    HttpServer& operator=(const HttpServer&) = delete;

    // Écoute sur `port`. Si le port est déjà pris, essaie les suivants (jusqu'à
    // 8) : mieux vaut un port inattendu — que le code QR porte de toute façon —
    // qu'un refus de démarrer.
    bool start(uint16_t port, HttpHandler handler, std::string* err);
    void stop();

    bool running() const { return running_.load(); }
    uint16_t port() const { return port_; }
    uint32_t requests_served() const { return served_.load(); }
    // Dernière erreur d'acceptation, pour l'affichage.
    std::string last_error() const;

    // Adresse IPv4 de la console sur son réseau local, 0 si le Wi-Fi est absent.
    static uint32_t local_ipv4();
    // Force l'adresse annoncée dans url(). Nécessaire quand la console vient de
    // créer son propre point d'accès : gethostid() décrit l'interface d'avant,
    // et le code QR enverrait le téléphone vers une adresse qui n'existe plus
    // sur le réseau qu'il vient de rejoindre. 0 rétablit la détection.
    void set_address(uint32_t ipv4_host_order) { address_ = ipv4_host_order; }
    // « http://192.168.1.42:8080/ », vide sans réseau.
    std::string url() const;

    // Plafond du corps d'une requête. Un .torrent de 8 Mo décrirait un contenu
    // de plusieurs téraoctets : personne n'en verra jamais.
    static constexpr size_t kMaxBody = 8 * 1024 * 1024;

private:
    void serve();
    void handle_client(int fd, uint32_t peer_ip);

    int listen_fd_ = -1;
    uint32_t address_ = 0;
    uint16_t port_ = 0;
    HttpHandler handler_;
    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<uint32_t> served_{0};

    mutable std::mutex error_mutex_;
    std::string last_error_;
};

// Vrai pour 10/8, 172.16/12, 192.168/16, 169.254/16 et 127/8 (ordre hôte).
bool is_private_ipv4(uint32_t addr);

}  // namespace net
