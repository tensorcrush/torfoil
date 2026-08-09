#include "net/softap.hpp"

#include <cstdio>
#include <cstring>
#include <vector>

#ifdef __SWITCH__
#include <switch.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#endif

#include "util/bytes.hpp"
#include "util/log.hpp"

namespace net {

namespace {

// Alphabet volontairement amputé : ni 0/O, ni 1/l/I. Ce mot de passe est lu à
// l'écran par quelqu'un dont l'appareil photo n'a pas voulu du code QR, et une
// confusion à ce moment-là ne se diagnostique pas — le téléphone dit seulement
// « impossible de se connecter ».
const char kAlphabet[] = "abcdefghijkmnpqrstuvwxyz23456789";

std::string random_string(size_t len) {
    std::string out(len, '?');
    std::vector<uint8_t> raw(len);
    util::random_bytes(raw.data(), raw.size());
    for (size_t i = 0; i < len; ++i) {
        out[i] = kAlphabet[raw[i] % (sizeof(kAlphabet) - 1)];
    }
    return out;
}

#ifdef __SWITCH__
std::string result_code(Result rc) {
    char buf[24];
    std::snprintf(buf, sizeof(buf), "0x%08x", rc);
    return buf;
}
#endif

}  // namespace

SoftAp::~SoftAp() {
    stop();
}

std::string SoftAp::wifi_qr_payload() const {
    if (ssid_.empty()) return {};
    // Les caractères réservés de cette syntaxe sont \ ; , : et " . L'alphabet
    // ci-dessus n'en produit aucun, mais l'échappement reste : le jour où
    // quelqu'un change l'alphabet, le code QR ne doit pas devenir silencieusement
    // faux.
    auto escape = [](const std::string& s) {
        std::string out;
        for (char c : s) {
            if (c == '\\' || c == ';' || c == ',' || c == ':' || c == '"') out.push_back('\\');
            out.push_back(c);
        }
        return out;
    };
    return "WIFI:T:WPA;S:" + escape(ssid_) + ";P:" + escape(passphrase_) + ";;";
}

#ifndef __SWITCH__

// Sur PC il n'y a pas de lp2p, et rien à créer : la machine est déjà sur un
// réseau. On fait donc semblant d'avoir levé un point d'accès, sans en lever
// aucun, et on laisse l'adresse à zéro pour que le serveur annonce celle de la
// machine. Tout le reste du chemin devient éprouvable depuis un vrai téléphone
// ou un navigateur : les deux codes QR, la page, l'envoi, les accusés de
// réception. Seul le point d'accès lui-même reste non vérifié — c'est
// précisément ce qu'aucune machine de développement ne peut trancher.
bool SoftAp::start(std::string* err) {
    (void)err;
    ssid_ = "torfoil-" + random_string(4);
    passphrase_ = random_string(10);
    address_ = 0;
    running_ = true;
    return true;
}

void SoftAp::stop() {
    running_ = false;
}

#else

bool SoftAp::start(std::string* err) {
    if (running_) return true;

    ssid_ = "torfoil-" + random_string(4);
    passphrase_ = random_string(10);
    address_ = 0;

    // lp2p:app plutôt que lp2p:sys : c'est le guichet ouvert aux applications,
    // et il n'exige pas la priorité système.
    Result rc = lp2pInitialize(Lp2pServiceType_App);
    if (R_FAILED(rc)) {
        if (err) {
            *err = "lp2p indisponible [" + result_code(rc) +
                   "] — firmware antérieur à 11.0.0 ?";
        }
        return false;
    }
    service_open_ = true;

    Lp2pGroupInfo info;
    lp2pCreateGroupInfo(&info);
    lp2pGroupInfoSetServiceName(&info, ssid_.c_str());

    // Le drapeau 0 et le type de sécurité 3 sont ce qui bascule le groupe du
    // chiffrement maison de Nintendo vers du WPA2-PSK ordinaire. Sans les deux,
    // le réseau existe mais aucun téléphone ne sait s'y authentifier : il
    // apparaît dans la liste et refuse le mot de passe.
    s8 flags[1] = {0};
    lp2pGroupInfoSetFlags(&info, flags, 1);
    info.security_type = 3;

    rc = lp2pGroupInfoSetPassphrase(&info, passphrase_.c_str());
    if (R_FAILED(rc)) {
        if (err) *err = "mot de passe refusé [" + result_code(rc) + "]";
        stop();
        return false;
    }

    // 2,4 GHz, canal laissé au système : la portée compte plus que le débit
    // pour transporter quelques kilo-octets de liens, et de vieux téléphones
    // ignorent encore le 5 GHz.
    lp2pGroupInfoSetFrequencyChannel(&info, 24, 0);
    lp2pGroupInfoSetStealthEnabled(&info, false);
    lp2pGroupInfoSetMemberCountMax(&info, 4);
    info.priority = 90;  // ApplicationPriority, la seule admise par lp2p:app

    rc = lp2pCreateGroup(&info);
    if (R_FAILED(rc)) {
        if (err) {
            // Ne pas suggérer de cause tant qu'on n'en connaît aucune : la
            // première version disait « Wi-Fi coupé ? », ce qui envoyait
            // vérifier une chose qui allait très bien.
            *err = "création du réseau refusée [" + result_code(rc) +
                   "] — lancer torfoil-diag.nro pour savoir pourquoi";
        }
        stop();
        return false;
    }
    running_ = true;

    // L'adresse ne vient pas de gethostid() ici : la console vient de changer
    // d'interface, et c'est lp2p qui sait laquelle elle s'est attribuée sur le
    // réseau qu'elle vient de créer.
    Lp2pIpConfig config;
    std::memset(&config, 0, sizeof(config));
    if (R_SUCCEEDED(lp2pGetIpConfig(&config))) {
        sockaddr_in addr;
        std::memcpy(&addr, config.ip_addr, sizeof(addr));
        address_ = ntohl(addr.sin_addr.s_addr);
    }

    util::log_fmt("point d'accès levé : %s, %u.%u.%u.%u", ssid_.c_str(), (address_ >> 24) & 0xff,
                  (address_ >> 16) & 0xff, (address_ >> 8) & 0xff, address_ & 0xff);
    return true;
}

void SoftAp::stop() {
    if (service_open_) {
        if (running_) lp2pDestroyGroup();
        lp2pExit();
        service_open_ = false;
    }
    if (running_) util::log_line("point d'accès refermé");
    running_ = false;
    address_ = 0;
}

#endif

}  // namespace net
