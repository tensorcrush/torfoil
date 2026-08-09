// Import depuis un téléphone : point d'accès + page web + accusés de réception.
//
// Le chemin complet, du côté de l'utilisateur : il ouvre l'onglet, la console
// lève son propre réseau Wi-Fi et affiche un code QR d'identifiants ; il le
// vise avec l'appareil photo, le téléphone rejoint le réseau ; un second code
// QR donne l'adresse de la page ; il y colle ses liens ou y dépose ses fichiers
// .torrent, et chaque arrivée s'affiche sur l'écran de la console.
//
// Cet écho sur la console n'est pas décoratif. Un envoi depuis un téléphone
// échoue en silence bien plus souvent qu'on ne croit — mauvaise page laissée
// ouverte, réseau qui a basculé sur les données mobiles — et sans confirmation
// visible on ne distingue pas « ça n'est pas parti » de « ça n'a pas marché ».
#pragma once

#include <atomic>
#include <mutex>
#include <string>
#include <vector>

#include "bt/session.hpp"
#include "net/http_server.hpp"
#include "net/softap.hpp"
#include "util/qr.hpp"

namespace ui {

class Phone {
public:
    // Ce que l'écran doit montrer, dans l'ordre où l'utilisateur le vit.
    enum class Step {
        Off,       // rien de levé
        JoinWifi,  // code QR des identifiants Wi-Fi
        OpenPage,  // réseau prêt, code QR de l'adresse
        Failed,
    };

    bool start(bt::Session& session, std::string* err);
    void stop();

    Step step() const { return step_; }
    const std::string& error() const { return error_; }

    const net::SoftAp& ap() const { return ap_; }
    std::string url() const { return server_.url(); }

    // Le code QR à afficher maintenant : identifiants Wi-Fi, puis adresse.
    const util::QrCode& qr() const { return qr_; }

    // L'utilisateur a vu le premier code et dit que son téléphone est connecté.
    // On ne peut pas le deviner : lp2p signale bien l'arrivée d'un membre, mais
    // un téléphone qui rejoint un réseau ne « rejoint » aucun groupe au sens de
    // lp2p — il obtient seulement une adresse par DHCP.
    void confirm_joined();

    // Messages destinés à l'écran, consommés par l'appelant.
    std::vector<std::string> take_events();

    uint32_t imported() const { return imported_; }

private:
    net::HttpReply handle(const net::HttpRequest& request);
    std::string page() const;
    void note(const std::string& message);
    bool set_qr(const std::string& payload);

    bt::Session* session_ = nullptr;
    net::SoftAp ap_;
    net::HttpServer server_;
    util::QrCode qr_;

    Step step_ = Step::Off;
    std::string error_;
    std::atomic<uint32_t> imported_{0};

    mutable std::mutex events_mutex_;
    std::vector<std::string> events_;
};

}  // namespace ui
