// Test « en vrai » du VPN : mêmes fichiers que la Switch, compilés pour PC.
//
// Ce que ça prouve : l'API Mullvad, la poignée de main WireGuard contre un
// vrai relais, la pile lwIP, le DNS interne, et que le trafic sort bien par
// Mullvad. Ce que ça ne prouve pas : la couche libnx (sockets Horizon) et
// l'IHM — seule la console peut les tester.
//
//   ./vpn_live <numéro_de_compte_16_chiffres> [pays]
//
// Le numéro n'est jamais affiché en entier ni écrit sur le disque.

#include <cinttypes>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <mbedtls/version.h>
#if MBEDTLS_VERSION_MAJOR >= 3
#include <psa/crypto.h>
#endif

#include "net/mullvad.hpp"
#include "net/tls.hpp"
#include "net/transport.hpp"
#include "net/wg/tunnel.hpp"
#include "net/wg/wireguard.hpp"
#include "util/clock.hpp"

namespace {

int g_failures = 0;

void step(const char* title) {
    std::printf("\n\033[1m== %s\033[0m\n", title);
}

void ok(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    std::printf("  \033[32mOK\033[0m   ");
    std::vprintf(fmt, args);
    std::printf("\n");
    va_end(args);
}

void fail(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    std::printf("  \033[31mÉCHEC\033[0m ");
    std::vprintf(fmt, args);
    std::printf("\n");
    va_end(args);
    ++g_failures;
}

void info(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    std::printf("       ");
    std::vprintf(fmt, args);
    std::printf("\n");
    va_end(args);
}

std::string ip_text(uint32_t ip) {
    char buf[24];
    std::snprintf(buf, sizeof(buf), "%u.%u.%u.%u", (ip >> 24) & 0xff, (ip >> 16) & 0xff,
                  (ip >> 8) & 0xff, ip & 0xff);
    return buf;
}

// Extrait une valeur d'un JSON plat, sans dépendre du parseur maison (on veut
// une mesure indépendante de ce qu'on teste).
std::string json_field(const std::string& body, const std::string& key) {
    const std::string needle = "\"" + key + "\"";
    size_t pos = body.find(needle);
    if (pos == std::string::npos) return {};
    pos = body.find(':', pos + needle.size());
    if (pos == std::string::npos) return {};
    ++pos;
    while (pos < body.size() && (body[pos] == ' ' || body[pos] == '"')) ++pos;
    size_t end = pos;
    while (end < body.size() && body[end] != '"' && body[end] != ',' && body[end] != '}') ++end;
    return body.substr(pos, end - pos);
}

// am.i.mullvad.net dit ce que le monde extérieur voit de nous. C'est la seule
// réponse qui tranche vraiment la question « suis-je dans le tunnel ? ».
bool who_am_i(net::Transport& transport, std::string& ip_out, bool& is_mullvad, std::string& err) {
    net::HttpResponse response;
    if (!net::https_request(transport, "https://am.i.mullvad.net/json", "GET", "", "", response,
                            &err)) {
        return false;
    }
    if (response.status != 200) {
        err = "HTTP " + std::to_string(response.status);
        return false;
    }
    const std::string& body = response.body;
    ip_out = json_field(body, "ip");
    is_mullvad = json_field(body, "mullvad_exit_ip") == "true";
    return !ip_out.empty();
}

}  // namespace

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IOLBF, 0);

#if MBEDTLS_VERSION_MAJOR >= 3
    psa_crypto_init();
#endif

    if (argc < 2) {
        std::fprintf(stderr,
                     "usage: %s <numéro_de_compte_mullvad> [pays]\n"
                     "  ex : %s 1234567890123456 France\n",
                     argv[0], argv[0]);
        return 2;
    }

    std::string account_number;
    for (const char* p = argv[1]; *p; ++p) {
        if (*p >= '0' && *p <= '9') account_number.push_back(*p);
    }
    const std::string wanted_country = (argc > 2) ? argv[2] : "";
    const bool keep_device = std::getenv("TORFOIL_KEEP_DEVICE") != nullptr;

    if (account_number.size() != 16) {
        std::fprintf(stderr, "numéro de compte invalide (%zu chiffres, 16 attendus)\n",
                     account_number.size());
        return 2;
    }

    auto direct = net::make_bsd_transport();

    // ---------------------------------------------------------------------
    step("1. Adresse publique sans VPN (référence)");
    std::string clear_ip;
    bool clear_is_mullvad = false;
    std::string err;
    if (who_am_i(*direct, clear_ip, clear_is_mullvad, err)) {
        ok("IP en clair : %s%s", clear_ip.c_str(),
           clear_is_mullvad ? "  (déjà derrière Mullvad !)" : "");
        if (clear_is_mullvad) {
            info("un VPN Mullvad tourne déjà sur cette machine : la comparaison");
            info("finale sera moins parlante. Coupe-le pour un test net.");
        }
    } else {
        fail("impossible de joindre am.i.mullvad.net : %s", err.c_str());
        info("sans référence, le test continue mais la comparaison sautera.");
    }

    // ---------------------------------------------------------------------
    step("2. Authentification Mullvad");
    mullvad::Account account;
    if (!mullvad::login(*direct, account_number, account, &err)) {
        fail("login : %s", err.c_str());
        return 1;
    }
    ok("jeton obtenu (%zu octets)", account.access_token.size());

    // ---------------------------------------------------------------------
    step("3. Enregistrement d'un appareil");
    uint8_t private_key[32];
    uint8_t public_key[32];
    wg::generate_private_key(private_key);
    wg::derive_public_key(public_key, private_key);
    ok("clé publique : %s", wg::key_to_base64(public_key).c_str());

    mullvad::Device device;
    if (!mullvad::create_device(*direct, account, wg::key_to_base64(public_key), device, &err)) {
        fail("create_device : %s", err.c_str());
        info("si Mullvad répond « too many devices », supprime-en un sur le site.");
        return 1;
    }
    ok("appareil %s, IP attribuée %s", device.id.c_str(), ip_text(device.ipv4).c_str());

    // Quoi qu'il arrive ensuite, on rend l'emplacement : Mullvad en limite le
    // nombre à 5 et un test ne doit pas en consommer un définitivement.
    struct Cleanup {
        net::Transport* transport;
        const mullvad::Account* account;
        std::string device_id;
        bool keep;
        ~Cleanup() {
            if (keep || device_id.empty()) {
                if (keep) std::printf("\n       appareil %s conservé\n", device_id.c_str());
                return;
            }
            std::string e;
            if (mullvad::remove_device(*transport, *account, device_id, &e)) {
                std::printf("\n       appareil de test supprimé (%s)\n", device_id.c_str());
            } else {
                std::printf("\n       \033[33mappareil %s NON supprimé : %s\033[0m\n",
                            device_id.c_str(), e.c_str());
                std::printf("       supprime-le depuis mullvad.net/account/devices\n");
            }
        }
    } cleanup{direct.get(), &account, device.id, keep_device};

    // ---------------------------------------------------------------------
    step("4. Liste des relais");
    std::vector<mullvad::Relay> relays;
    if (!mullvad::list_relays(*direct, relays, &err)) {
        fail("list_relays : %s", err.c_str());
        return 1;
    }
    ok("%zu relais WireGuard", relays.size());

    const mullvad::Relay* chosen = nullptr;
    if (!wanted_country.empty()) {
        for (const mullvad::Relay& relay : relays) {
            if (relay.country == wanted_country || relay.country_code == wanted_country) {
                chosen = &relay;
                break;
            }
        }
        if (!chosen) fail("aucun relais pour « %s », on prend le premier", wanted_country.c_str());
    }
    if (!chosen) {
        if (relays.empty()) {
            fail("liste vide");
            return 1;
        }
        chosen = &relays.front();
    }
    ok("relais : %s (%s, %s) %s:%u", chosen->hostname.c_str(), chosen->city.c_str(),
       chosen->country.c_str(), ip_text(chosen->ipv4).c_str(), chosen->port);

    // ---------------------------------------------------------------------
    step("5. Poignée de main WireGuard");
    wg::TunnelConfig config;
    config.endpoint_ip = chosen->ipv4;
    config.endpoint_port = chosen->port;
    std::memcpy(config.server_public, chosen->public_key, 32);
    config.server_label = chosen->hostname;
    std::memcpy(config.private_key, private_key, 32);
    config.assigned_ip = device.ipv4;
    config.netmask = 0xffffffffu;
    config.dns_ip = mullvad::kDnsServer;

    const uint64_t started = util::now_ms();
    auto tunnel = wg::make_tunnel(config, &err);
    if (!tunnel) {
        fail("make_tunnel : %s", err.c_str());
        return 1;
    }

    const uint64_t deadline = started + 15000;
    while (util::now_ms() < deadline && tunnel->state() != wg::TunnelState::Up) {
        net::PollItem dummy;
        tunnel->poll(&dummy, 0, 100);
    }

    if (tunnel->state() != wg::TunnelState::Up) {
        fail("pas de réponse du relais en 15 s (état : %s)", tunnel->status_text().c_str());
        info("le relais ignore les clés inconnues : si l'étape 3 a réussi, c'est");
        info("plutôt un souci de crypto, de format de paquet, ou d'UDP bloqué.");
        tunnel->stop();
        return 1;
    }
    ok("tunnel établi en %" PRIu64 " ms", util::now_ms() - started);
    ok("transport actif : %s, prêt : %s", tunnel->name(), tunnel->ready() ? "oui" : "non");

    // ---------------------------------------------------------------------
    step("6. Trafic réel dans le tunnel");
    std::string tunnel_ip;
    bool tunnel_is_mullvad = false;
    if (!who_am_i(*tunnel, tunnel_ip, tunnel_is_mullvad, err)) {
        fail("requête HTTPS dans le tunnel : %s", err.c_str());
        info("la poignée de main passe mais pas les données : regarder lwIP,");
        info("la MTU (1420) ou le DNS interne 10.64.0.1.");
    } else {
        ok("IP vue de l'extérieur : %s", tunnel_ip.c_str());
        if (tunnel_is_mullvad) {
            ok("Mullvad confirme : le trafic sort par un de ses serveurs");
        } else {
            fail("Mullvad ne reconnaît pas cette sortie — trafic hors tunnel ?");
        }
        if (!clear_ip.empty()) {
            if (tunnel_ip != clear_ip) {
                ok("l'IP a changé (%s → %s)", clear_ip.c_str(), tunnel_ip.c_str());
            } else {
                fail("IP identique à la référence : rien n'a été tunnelé");
            }
        }
        info("octets chiffrés envoyés : %" PRIu64 ", reçus : %" PRIu64, tunnel->bytes_sent(),
             tunnel->bytes_received());
    }

    // ---------------------------------------------------------------------
    step("7. DNS interne");
    uint32_t resolved = 0;
    if (tunnel->resolve("api.mullvad.net", resolved) && resolved != 0) {
        ok("résolu via 10.64.0.1 : api.mullvad.net = %s", ip_text(resolved).c_str());
    } else {
        fail("le résolveur du tunnel ne répond pas");
        info("conséquence réelle : les requêtes DNS fuiraient hors du tunnel.");
    }

    // ---------------------------------------------------------------------
    step("8. Killswitch");
    tunnel->stop();
    if (tunnel->ready()) {
        fail("ready() reste vrai après stop() : des sockets pourraient naître");
    } else {
        ok("ready() est faux : plus aucune socket ne peut être créée");
    }
    auto orphan = tunnel->tcp();
    if (orphan && orphan->start_connect({chosen->ipv4, 443})) {
        fail("une connexion a pu partir alors que le tunnel est mort");
    } else {
        ok("tentative de connexion refusée, tunnel arrêté");
    }

    // ---------------------------------------------------------------------
    std::printf("\n\033[1m%s\033[0m\n",
                g_failures == 0 ? "\033[32mTout est passé : le VPN fonctionne.\033[0m"
                                : "\033[31mDes étapes ont échoué (voir ci-dessus).\033[0m");
    return g_failures == 0 ? 0 : 1;
}
