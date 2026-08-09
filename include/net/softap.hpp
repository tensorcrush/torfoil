// Point d'accès Wi-Fi hébergé par la console, comme « Envoyer vers un
// smartphone » dans l'Album.
//
// Le principe est celui de Nintendo, et il est meilleur que la solution
// évidente : plutôt que de supposer que le téléphone et la console sont déjà
// sur le même réseau — ce qui n'est vrai ni chez des amis, ni à l'hôtel, ni
// quand le Wi-Fi du logement isole ses clients les uns des autres — la console
// crée son PROPRE réseau et invite le téléphone à s'y joindre. Deux codes QR
// suffisent alors : le premier porte les identifiants Wi-Fi, le second l'adresse
// de la page. Rien à taper, rien à installer, et aucun routeur à convaincre.
//
// Le service qui rend cela possible est lp2p, celui de l'Album justement. À la
// différence de ldn — le mode « communication locale » entre consoles — lp2p
// monte un vrai point d'accès en mode infrastructure, avec DHCP, et depuis le
// firmware 11.0.0 il accepte le WPA2-PSK standard. C'est cette combinaison qui
// le rend joignable par un appareil qui n'est pas une Switch : ldn, lui, parle
// des trames Nintendo qu'aucun téléphone ne comprendra jamais.
//
// UNE CONSÉQUENCE À CONNAÎTRE : la console n'a qu'une radio. Tant que le point
// d'accès est levé, elle n'est plus connectée à Internet et les téléchargements
// s'arrêtent. C'est aussi vrai pour l'Album. L'import se fait donc par un
// aller-retour court : on lève le point d'accès, on dépose ses liens, on le
// referme, et le moteur repart.
#pragma once

#include <cstdint>
#include <string>

namespace net {

class SoftAp {
public:
    SoftAp() = default;
    ~SoftAp();

    SoftAp(const SoftAp&) = delete;
    SoftAp& operator=(const SoftAp&) = delete;

    // Lève le point d'accès. Le nom du réseau et le mot de passe sont tirés au
    // sort à chaque fois : un mot de passe fixe finirait écrit dans un forum, et
    // ce réseau donne le droit d'ajouter des torrents.
    bool start(std::string* err);
    void stop();

    bool running() const { return running_; }

    const std::string& ssid() const { return ssid_; }
    const std::string& passphrase() const { return passphrase_; }

    // Adresse IPv4 de la console SUR ce réseau, en ordre hôte. 0 tant que le
    // point d'accès n'est pas prêt.
    uint32_t address() const { return address_; }

    // « WIFI:T:WPA;S:…;P:…;; » — la syntaxe que les appareils photo d'Android et
    // d'iOS reconnaissent pour proposer de rejoindre un réseau.
    std::string wifi_qr_payload() const;

private:
    bool running_ = false;
    bool service_open_ = false;
    std::string ssid_;
    std::string passphrase_;
    uint32_t address_ = 0;
};

}  // namespace net
