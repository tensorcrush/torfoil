// Import depuis un téléphone.
//
// Le problème résolu : sur une console, un lien magnet se tape au clavier
// virtuel et un fichier .torrent ne s'ouvre pas du tout. La solution retenue
// évite d'installer quoi que ce soit sur le téléphone — pas d'application, pas
// de compte, pas de service tiers : la console sert une page web sur le réseau
// local, et l'appareil photo du téléphone l'ouvre en visant un code QR affiché
// à l'écran. Safari suffit, et le sélecteur de fichiers d'iOS donne accès à
// Fichiers, iCloud et à tout ce que l'utilisateur a enregistré depuis un lien.
//
// Ce qui entre par là est un NOM de contenu (lien ou .torrent), jamais du
// contenu : rien de ce qui est téléchargé n'emprunte ce chemin, dans aucun
// sens. Le killswitch reste donc entier — voir net/http_server.hpp.
#pragma once

#include <atomic>
#include <mutex>
#include <string>

#include "bt/session.hpp"
#include "net/http_server.hpp"
#include "util/qr.hpp"

namespace ui {

class PhoneBridge {
public:
    // `inbox_dir` reçoit une copie des .torrent déposés : ils ne servent pas au
    // fonctionnement (la reprise passe par le lien reconstruit) mais permettent
    // de retrouver ce qu'on a envoyé, et de le renvoyer ailleurs.
    bool start(bt::Session& session, const std::string& inbox_dir, uint16_t port,
               std::string* err);
    void stop();

    bool running() const { return server_.running(); }
    const std::string& url() const { return url_; }
    const util::QrCode& qr() const { return qr_; }
    bool has_qr() const { return qr_.size > 0; }

    uint32_t added() const { return added_.load(); }
    uint32_t requests() const { return server_.requests_served(); }
    std::string last_error() const;

    // Message destiné à l'IHM de la console, consommé une seule fois : quand un
    // torrent entre par le téléphone, l'écran doit le dire — sinon on ne sait
    // pas si l'envoi est passé sans aller le vérifier dans la liste.
    bool take_notice(std::string& message, bool& error);

private:
    net::HttpReply dispatch(const net::HttpRequest& request);
    net::HttpReply handle_add(const net::HttpRequest& request);
    net::HttpReply handle_action(const net::HttpRequest& request);
    std::string status_json() const;
    void notice(const std::string& message, bool error);
    void save_copy(const std::string& filename, const std::string& blob) const;

    bt::Session* session_ = nullptr;
    std::string inbox_dir_;
    net::HttpServer server_;
    util::QrCode qr_;
    std::string url_;
    std::atomic<uint32_t> added_{0};

    mutable std::mutex mutex_;
    std::string notice_;
    bool notice_error_ = false;
    bool notice_pending_ = false;
};

}  // namespace ui
