// Protocole WireGuard en espace utilisateur (poignée de main Noise IKpsk2 +
// transport), côté initiateur uniquement — c'est tout ce dont un client a
// besoin.
//
// Cette couche ne connaît ni sockets ni IP : on lui donne des octets reçus, elle
// rend des octets à émettre. C'est ce qui la rend testable sur PC.
#pragma once

#include <cstdint>
#include <cstring>
#include <string>

namespace wg {

constexpr size_t kKeyLen = 32;
constexpr size_t kInitiationSize = 148;
constexpr size_t kResponseSize = 92;
constexpr size_t kCookieReplySize = 64;
constexpr size_t kDataHeaderSize = 16;  // type + réservé + index + compteur
constexpr size_t kTagLen = 16;

enum MessageType : uint8_t {
    kTypeInitiation = 1,
    kTypeResponse = 2,
    kTypeCookieReply = 3,
    kTypeData = 4,
};

// Minuteries de la spécification.
constexpr uint64_t kRekeyAfterMs = 120 * 1000;
constexpr uint64_t kRejectAfterMs = 180 * 1000;
constexpr uint64_t kKeepaliveMs = 25 * 1000;
constexpr uint64_t kRekeyTimeoutMs = 5 * 1000;

class Peer {
public:
    // `preshared` peut être nul (Mullvad n'en utilise pas par défaut).
    void configure(const uint8_t private_key[kKeyLen], const uint8_t peer_public[kKeyLen],
                   const uint8_t* preshared);

    // --- poignée de main ---

    // Prépare un message d'initiation. `now_ms` sert aux minuteries, `unix_ns`
    // à l'horodatage TAI64N (protection contre le rejeu côté serveur).
    bool make_initiation(uint8_t out[kInitiationSize], uint64_t now_ms, uint64_t unix_ns);

    // Consomme la réponse du serveur et dérive les clés de transport.
    bool consume_response(const uint8_t* msg, size_t len, uint64_t now_ms);

    bool established() const { return established_; }
    bool needs_handshake(uint64_t now_ms) const;
    bool needs_keepalive(uint64_t now_ms) const;
    void note_handshake_sent(uint64_t now_ms) { last_handshake_attempt_ms_ = now_ms; }

    // --- transport ---

    // Encapsule un paquet IP. Renvoie la taille écrite, ou -1.
    int encapsulate(const uint8_t* packet, size_t len, uint8_t* out, size_t out_capacity,
                    uint64_t now_ms);

    // Désencapsule un message reçu. Renvoie la taille du paquet IP restitué,
    // 0 pour un keepalive, -1 si le message est invalide ou rejoué.
    int decapsulate(const uint8_t* msg, size_t len, uint8_t* out, size_t out_capacity,
                    uint64_t now_ms);

    uint32_t local_index() const { return local_index_; }
    const char* last_error() const { return last_error_; }

    // Exposés pour les tests : permettent de faire dialoguer deux instances.
    bool consume_initiation_for_test(const uint8_t* msg, size_t len);
    bool make_response_for_test(uint8_t out[kResponseSize]);

private:
    void reset_handshake();
    bool derive_transport_keys(bool initiator);

    uint8_t static_private_[kKeyLen]{};
    uint8_t static_public_[kKeyLen]{};
    uint8_t peer_public_[kKeyLen]{};
    uint8_t preshared_[kKeyLen]{};

    // État de la poignée de main en cours.
    uint8_t chaining_key_[kKeyLen]{};
    uint8_t hash_[kKeyLen]{};
    uint8_t ephemeral_private_[kKeyLen]{};
    uint8_t ephemeral_public_[kKeyLen]{};
    // Utilisé uniquement par le chemin « répondeur » des tests.
    uint8_t initiator_ephemeral_[kKeyLen]{};
    uint32_t local_index_ = 0;
    uint32_t remote_index_ = 0;

    // Clés de transport.
    uint8_t send_key_[kKeyLen]{};
    uint8_t recv_key_[kKeyLen]{};
    uint64_t send_counter_ = 0;

    // Fenêtre anti-rejeu. `have_recv_` est indispensable : sans lui, le tout
    // premier paquet (compteur 0) serait indiscernable de l'état initial et
    // pourrait être rejoué.
    bool have_recv_ = false;
    uint64_t recv_highest_ = 0;
    uint64_t recv_window_ = 0;

    bool established_ = false;
    bool handshake_in_progress_ = false;
    uint64_t established_at_ms_ = 0;
    uint64_t last_send_ms_ = 0;
    uint64_t last_recv_ms_ = 0;
    uint64_t last_handshake_attempt_ms_ = 0;

    const char* last_error_ = "";
};

// Utilitaires exposés pour les tests.
void generate_private_key(uint8_t out[kKeyLen]);
void derive_public_key(uint8_t out[kKeyLen], const uint8_t private_key[kKeyLen]);
std::string key_to_base64(const uint8_t key[kKeyLen]);
bool key_from_base64(const std::string& text, uint8_t out[kKeyLen]);

// Clé de chaînage initiale, constante du protocole. Sert de test de cohérence.
void initial_chaining_key(uint8_t out[kKeyLen]);

}  // namespace wg
