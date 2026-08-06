#include "net/mullvad.hpp"

#include <cstdlib>
#include <cstring>

#include "net/json.hpp"
#include "net/tls.hpp"
#include "util/bytes.hpp"
#include "util/clock.hpp"

namespace mullvad {

namespace {

const char kApiBase[] = "https://api.mullvad.net";

bool fail(std::string* err, const std::string& msg) {
    if (err) *err = msg;
    return false;
}

// « 10.64.12.34/32 » ou « 10.64.12.34 » → entier en ordre hôte.
bool parse_ipv4(const std::string& text, uint32_t& out) {
    unsigned a = 0, b = 0, c = 0, d = 0;
    if (std::sscanf(text.c_str(), "%u.%u.%u.%u", &a, &b, &c, &d) != 4) return false;
    if (a > 255 || b > 255 || c > 255 || d > 255) return false;
    out = a << 24 | b << 16 | c << 8 | d;
    return true;
}

bool decode_base64_key(const std::string& text, uint8_t out[32]) {
    auto value_of = [](char ch) -> int {
        if (ch >= 'A' && ch <= 'Z') return ch - 'A';
        if (ch >= 'a' && ch <= 'z') return ch - 'a' + 26;
        if (ch >= '0' && ch <= '9') return ch - '0' + 52;
        if (ch == '+') return 62;
        if (ch == '/') return 63;
        return -1;
    };

    uint32_t acc = 0;
    int bits = 0;
    size_t written = 0;
    for (const char ch : text) {
        if (ch == '=' ) break;
        const int v = value_of(ch);
        if (v < 0) return false;
        acc = acc << 6 | static_cast<uint32_t>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (written >= 32) return false;
            out[written++] = static_cast<uint8_t>(acc >> bits);
        }
    }
    return written == 32;
}

std::string json_escape(const std::string& in) {
    std::string out;
    for (const char c : in) {
        if (c == '"' || c == '\\') out.push_back('\\');
        out.push_back(c);
    }
    return out;
}

// Extrait un message d'erreur lisible d'une réponse d'API.
std::string api_error(const net::HttpResponse& response) {
    json::Value root;
    if (json::parse(response.body, root, nullptr) && root.is_object()) {
        const std::string code = root.string_or("code");
        const std::string detail = root.string_or("detail");
        if (!detail.empty()) return detail;
        if (!code.empty()) return code;
    }
    return "HTTP " + std::to_string(response.status);
}

}  // namespace

bool login(net::Transport& transport, const std::string& account_number, Account& out,
           std::string* err) {
    // Le numéro de compte Mullvad est une suite de 16 chiffres ; on filtre les
    // espaces que les gens collent souvent.
    std::string digits;
    for (const char c : account_number) {
        if (c >= '0' && c <= '9') digits.push_back(c);
    }
    if (digits.size() != 16) return fail(err, "un numéro de compte Mullvad fait 16 chiffres");

    const std::string body = "{\"account_number\":\"" + digits + "\"}";

    net::HttpResponse response;
    if (!net::https_request(transport, std::string(kApiBase) + "/auth/v1/token", "POST", body, "",
                            response, err)) {
        return false;
    }
    if (response.status != 200 && response.status != 201) {
        if (response.status == 400 || response.status == 404) {
            return fail(err, "compte inconnu");
        }
        return fail(err, "authentification refusée : " + api_error(response));
    }

    json::Value root;
    if (!json::parse(response.body, root, nullptr)) return fail(err, "réponse illisible");

    out.number = digits;
    out.access_token = root.string_or("access_token");
    if (out.access_token.empty()) return fail(err, "jeton d'accès absent de la réponse");

    // L'expiration est une date ISO ; on se contente d'une marge fixe, le jeton
    // Mullvad vit largement plus longtemps qu'une session.
    out.token_expiry_ms = util::now_ms() + 6ULL * 3600 * 1000;
    return true;
}

bool create_device(net::Transport& transport, const Account& account,
                   const std::string& public_key_b64, Device& out, std::string* err) {
    if (account.access_token.empty()) return fail(err, "aucun jeton d'accès");

    // hijack_dns=false : c'est nous qui pointons le résolveur interne, pas le
    // serveur qui détourne le trafic.
    const std::string body =
        "{\"pubkey\":\"" + json_escape(public_key_b64) + "\",\"hijack_dns\":false}";

    net::HttpResponse response;
    if (!net::https_request(transport, std::string(kApiBase) + "/accounts/v1/devices", "POST",
                            body, account.access_token, response, err)) {
        return false;
    }
    if (response.status != 200 && response.status != 201) {
        if (response.status == 400) {
            return fail(err,
                        "Mullvad refuse un appareil de plus (5 maximum) — en retirer un depuis "
                        "le site");
        }
        return fail(err, "enregistrement refusé : " + api_error(response));
    }

    json::Value root;
    if (!json::parse(response.body, root, nullptr)) return fail(err, "réponse illisible");

    out.id = root.string_or("id");
    out.name = root.string_or("name");
    out.ipv4_text = root.string_or("ipv4_address");
    if (out.ipv4_text.empty() || !parse_ipv4(out.ipv4_text, out.ipv4)) {
        return fail(err, "adresse attribuée absente de la réponse");
    }
    return true;
}

bool device_exists(net::Transport& transport, const Account& account, const std::string& device_id,
                   bool& exists_out, std::string* err) {
    exists_out = false;
    if (device_id.empty()) return fail(err, "identifiant d'appareil vide");

    net::HttpResponse response;
    if (!net::https_request(transport,
                            std::string(kApiBase) + "/accounts/v1/devices/" + device_id, "GET", "",
                            account.access_token, response, err)) {
        // Question non posée : on ne conclut rien, surtout pas que l'appareil a
        // disparu. Supprimer un appareil valide sur un simple hoquet réseau
        // consommerait un emplacement à chaque tentative.
        return false;
    }

    if (response.status == 200) {
        exists_out = true;
        return true;
    }
    if (response.status == 404) {
        exists_out = false;
        return true;
    }
    return fail(err, "réponse inattendue de Mullvad (" + std::to_string(response.status) + ")");
}

bool remove_device(net::Transport& transport, const Account& account, const std::string& device_id,
                   std::string* err) {
    if (device_id.empty()) return fail(err, "identifiant d'appareil vide");

    net::HttpResponse response;
    if (!net::https_request(transport,
                            std::string(kApiBase) + "/accounts/v1/devices/" + device_id, "DELETE",
                            "", account.access_token, response, err)) {
        return false;
    }
    return response.status == 200 || response.status == 204;
}

bool list_relays(net::Transport& transport, std::vector<Relay>& out, std::string* err) {
    net::HttpResponse response;
    if (!net::https_request(transport, std::string(kApiBase) + "/app/v1/relays", "GET", "", "",
                            response, err, 40000)) {
        return false;
    }
    if (response.status != 200) return fail(err, "liste des relais indisponible");

    json::Value root;
    if (!json::parse(response.body, root, nullptr)) return fail(err, "liste des relais illisible");

    // Structure : { locations: { "fr-par": {country, city, ...} },
    //               wireguard: { port_ranges: [[a,b]], relays: [...] } }
    const json::Value* locations = root.find("locations");
    const json::Value* wireguard = root.find("wireguard");
    if (!wireguard) return fail(err, "section wireguard absente");

    uint16_t default_port = 51820;
    if (const json::Value* ranges = wireguard->find("port_ranges")) {
        if (ranges->is_array() && !ranges->array.empty() && ranges->array[0].is_array() &&
            !ranges->array[0].array.empty()) {
            const double first = ranges->array[0].array[0].number;
            if (first > 0 && first < 65536) default_port = static_cast<uint16_t>(first);
        }
    }

    const json::Value* relays = wireguard->find("relays");
    if (!relays || !relays->is_array()) return fail(err, "aucun relais listé");

    out.clear();
    out.reserve(relays->array.size());

    for (const json::Value& entry : relays->array) {
        Relay relay;
        relay.hostname = entry.string_or("hostname");
        relay.owned = entry.find("owned") && entry.find("owned")->boolean;
        relay.port = default_port;

        const std::string ip_text = entry.string_or("ipv4_addr_in");
        const std::string key_text = entry.string_or("public_key");
        if (relay.hostname.empty() || ip_text.empty() || key_text.empty()) continue;
        if (!parse_ipv4(ip_text, relay.ipv4)) continue;
        if (!decode_base64_key(key_text, relay.public_key)) continue;

        // Les relais actifs seulement.
        if (const json::Value* active = entry.find("active")) {
            if (active->type == json::Value::Type::Bool && !active->boolean) continue;
        }

        // Le pays et la ville vivent dans « locations », indexés par code.
        const std::string location = entry.string_or("location");
        relay.country_code = location;
        if (locations && !location.empty()) {
            if (const json::Value* place = locations->find(location)) {
                relay.country = place->string_or("country");
                relay.city = place->string_or("city");
            }
        }

        out.push_back(std::move(relay));
    }

    if (out.empty()) return fail(err, "aucun relais WireGuard exploitable");
    return true;
}

}  // namespace mullvad
