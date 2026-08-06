// Client de l'API Mullvad.
//
// Enchaînement : numéro de compte → jeton d'accès → enregistrement d'un
// « device » portant notre clé publique WireGuard → adresse IP attribuée.
// C'est exactement ce que fait l'application officielle.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "net/transport.hpp"

namespace mullvad {

struct Account {
    std::string number;        // 16 chiffres
    std::string access_token;
    uint64_t token_expiry_ms = 0;
};

struct Device {
    std::string id;
    std::string name;
    uint32_t ipv4 = 0;  // adresse attribuée, ordre hôte
    std::string ipv4_text;
};

struct Relay {
    std::string hostname;      // « fr-par-wg-001 »
    std::string country;
    std::string city;
    std::string country_code;
    uint32_t ipv4 = 0;
    uint8_t public_key[32]{};
    uint16_t port = 51820;
    bool owned = false;
};

// Résolveur interne de Mullvad : 10.64.0.1. L'utiliser garantit qu'aucune
// requête DNS ne sort du tunnel.
constexpr uint32_t kDnsServer = (10u << 24) | (64u << 16) | (0u << 8) | 1u;

bool login(net::Transport& transport, const std::string& account_number, Account& out,
           std::string* err);

// `public_key_b64` : clé publique WireGuard encodée en base64.
bool create_device(net::Transport& transport, const Account& account,
                   const std::string& public_key_b64, Device& out, std::string* err);

bool list_relays(net::Transport& transport, std::vector<Relay>& out, std::string* err);

// Vérifie qu'un appareil enregistré existe toujours côté Mullvad. Renvoie false
// si la question elle-même n'a pas pu être posée (réseau) — à distinguer d'une
// réponse « il n'existe plus ».
bool device_exists(net::Transport& transport, const Account& account, const std::string& device_id,
                   bool& exists_out, std::string* err);

// Supprime un device (libère un emplacement : Mullvad en limite le nombre).
bool remove_device(net::Transport& transport, const Account& account, const std::string& device_id,
                   std::string* err);

}  // namespace mullvad
