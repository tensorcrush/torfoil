// Utilitaires d'E/S bâtis au-dessus de net::Transport.
//
// Les sockets du Transport sont non bloquantes (le moteur pair-à-pair en a
// besoin). Ces aides rétablissent une sémantique bloquante-avec-délai pour les
// échanges ponctuels — annonces tracker, appels à l'API Mullvad — qui tournent
// sur un thread dédié et gagnent à s'écrire linéairement.
#pragma once

#include <string>

#include "net/transport.hpp"

namespace net {

struct Url {
    std::string scheme;  // "http", "https", "udp"
    std::string host;
    std::string path = "/";
    std::string query;
    uint16_t port = 0;

    bool secure() const { return scheme == "https"; }
};

bool parse_url(const std::string& raw, Url& out);

// Résout l'hôte puis connecte, dans la limite de `timeout_ms`.
bool connect_wait(Transport& t, TcpSocket& sock, const Endpoint& ep, int timeout_ms);

// Émet la totalité du tampon ou échoue.
bool send_all(Transport& t, TcpSocket& sock, const uint8_t* data, size_t len, int timeout_ms);

// Lit au plus `len` octets ; renvoie le nombre lu, 0 si le pair a fermé,
// négatif sur erreur ou expiration.
int recv_wait(Transport& t, TcpSocket& sock, uint8_t* data, size_t len, int timeout_ms);

// Lit jusqu'à la fermeture de la connexion (ou `max_bytes`).
bool recv_until_close(Transport& t, TcpSocket& sock, std::string& out, size_t max_bytes,
                      int timeout_ms);

struct HttpResponse {
    int status = 0;
    std::string body;
    std::string content_type;
};

// GET HTTP simple. `https` nécessite la couche TLS (M2) et renvoie une erreur
// explicite tant qu'elle n'est pas branchée.
bool http_get(Transport& t, const std::string& url, HttpResponse& out, std::string* err,
              int timeout_ms = 20000);

}  // namespace net
