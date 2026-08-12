// Accès distant : la même liste de torrents, depuis un navigateur du réseau.
//
// Différence avec ui::Phone, qui sert lui aussi une page : Phone crée son propre
// point d'accès, ce qui coupe Internet et donc les téléchargements. Ici la
// console reste sur son réseau habituel et continue de télécharger pendant
// qu'on la pilote depuis un PC.
//
// Le serveur ne répond qu'aux adresses privées, et l'API exige un jeton tiré au
// démarrage. Sans lui, tout voisin de réseau pourrait supprimer les fichiers
// d'un torrent.
#pragma once

#include <atomic>
#include <cstdint>
#include <string>

#include "bt/session.hpp"
#include "net/http_server.hpp"

namespace ui {

class Remote {
public:
    bool start(bt::Session& session, uint16_t port, std::string* err);
    void stop();

    bool running() const { return server_.running(); }
    // « http://192.168.1.42:8080/?k=3f9c1a » — jeton compris, c'est l'adresse à
    // recopier telle quelle.
    std::string url() const;
    // Sans le jeton, pour l'affichage sur deux lignes.
    std::string base_url() const { return server_.url(); }
    const std::string& token() const { return token_; }
    uint32_t requests_served() const { return server_.requests_served(); }
    std::string last_error() const { return server_.last_error(); }

private:
    net::HttpReply handle(const net::HttpRequest& request);
    net::HttpReply api_state() const;
    net::HttpReply api_add(const net::HttpRequest& request);
    net::HttpReply api_action(const net::HttpRequest& request);
    bool authorized(const net::HttpRequest& request) const;

    bt::Session* session_ = nullptr;
    net::HttpServer server_;
    std::string token_;
};

// Page servie par le serveur, sortie ici pour être testable sans socket.
const char* remote_page();

}  // namespace ui
