#include "net/wg/wireguard.hpp"

#include <cstring>

#include "net/wg/blake2s.h"
#include "net/wg/chacha20poly1305.h"
#include "net/wg/x25519.h"
#include "util/bytes.hpp"

namespace wg {

namespace {

const char kConstruction[] = "Noise_IKpsk2_25519_ChaChaPoly_BLAKE2s";
const char kIdentifier[] = "WireGuard v1 zx2c4 Jason@zx2c4.com";
const char kLabelMac1[] = "mac1----";
const char kLabelCookie[] = "cookie--";

void hash(uint8_t out[32], const void* a, size_t a_len, const void* b, size_t b_len) {
    blake2s_state s;
    blake2s_init(&s, 32);
    if (a_len) blake2s_update(&s, a, a_len);
    if (b_len) blake2s_update(&s, b, b_len);
    blake2s_final(&s, out);
}

void mix_hash(uint8_t h[32], const void* data, size_t len) {
    uint8_t tmp[32];
    hash(tmp, h, 32, data, len);
    std::memcpy(h, tmp, 32);
}

// KDF de WireGuard : HMAC-BLAKE2s chaîné, jusqu'à trois sorties.
void kdf(const uint8_t chaining_key[32], const uint8_t* input, size_t input_len, uint8_t* out1,
         uint8_t* out2, uint8_t* out3) {
    uint8_t prk[32];
    blake2s_hmac(prk, chaining_key, 32, input, input_len);

    uint8_t t1[32];
    const uint8_t one = 0x01;
    blake2s_hmac(t1, prk, 32, &one, 1);
    if (out1) std::memcpy(out1, t1, 32);

    if (out2 || out3) {
        uint8_t buffer[33];
        std::memcpy(buffer, t1, 32);
        buffer[32] = 0x02;

        uint8_t t2[32];
        blake2s_hmac(t2, prk, 32, buffer, sizeof(buffer));
        if (out2) std::memcpy(out2, t2, 32);

        if (out3) {
            std::memcpy(buffer, t2, 32);
            buffer[32] = 0x03;
            uint8_t t3[32];
            blake2s_hmac(t3, prk, 32, buffer, sizeof(buffer));
            std::memcpy(out3, t3, 32);
            std::memset(t3, 0, sizeof(t3));
        }
        std::memset(t2, 0, sizeof(t2));
        std::memset(buffer, 0, sizeof(buffer));
    }

    std::memset(prk, 0, sizeof(prk));
    std::memset(t1, 0, sizeof(t1));
}

// mac1 : BLAKE2s tronqué à 128 bits, en mode clé natif (et non HMAC).
void compute_mac1(uint8_t out[16], const uint8_t peer_public[32], const uint8_t* msg,
                  size_t len) {
    uint8_t key[32];
    blake2s_state s;
    blake2s_init(&s, 32);
    blake2s_update(&s, kLabelMac1, sizeof(kLabelMac1) - 1);
    blake2s_update(&s, peer_public, 32);
    blake2s_final(&s, key);

    blake2s(out, 16, key, 32, msg, len);
    std::memset(key, 0, sizeof(key));
}

void write_le32(uint8_t* p, uint32_t v) {
    p[0] = static_cast<uint8_t>(v);
    p[1] = static_cast<uint8_t>(v >> 8);
    p[2] = static_cast<uint8_t>(v >> 16);
    p[3] = static_cast<uint8_t>(v >> 24);
}

uint32_t read_le32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | static_cast<uint32_t>(p[1]) << 8 |
           static_cast<uint32_t>(p[2]) << 16 | static_cast<uint32_t>(p[3]) << 24;
}

void write_le64(uint8_t* p, uint64_t v) {
    for (int i = 0; i < 8; ++i) p[i] = static_cast<uint8_t>(v >> (8 * i));
}

uint64_t read_le64(const uint8_t* p) {
    uint64_t v = 0;
    for (int i = 7; i >= 0; --i) v = v << 8 | p[i];
    return v;
}

// TAI64N : 8 octets de secondes (avec le décalage TAI) puis 4 de nanosecondes,
// en gros-boutiste. Le serveur s'en sert pour refuser les initiations rejouées.
void tai64n(uint8_t out[12], uint64_t unix_ns) {
    const uint64_t seconds = unix_ns / 1000000000ULL;
    const uint32_t nanos = static_cast<uint32_t>(unix_ns % 1000000000ULL);
    util::wr_be64(out, 0x400000000000000AULL + seconds);
    util::wr_be32(out + 8, nanos);
}

const char kBase64Alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

}  // namespace

void generate_private_key(uint8_t out[kKeyLen]) {
    util::random_bytes(out, kKeyLen);
    // Bridage RFC 7748, appliqué aussi à la génération pour que la clé stockée
    // soit déjà sous forme canonique.
    out[0] &= 248;
    out[31] &= 127;
    out[31] |= 64;
}

void derive_public_key(uint8_t out[kKeyLen], const uint8_t private_key[kKeyLen]) {
    x25519_public_key(out, private_key);
}

std::string key_to_base64(const uint8_t key[kKeyLen]) {
    std::string out;
    out.reserve(44);
    for (size_t i = 0; i < kKeyLen; i += 3) {
        const uint32_t triple = static_cast<uint32_t>(key[i]) << 16 |
                                (i + 1 < kKeyLen ? static_cast<uint32_t>(key[i + 1]) << 8 : 0) |
                                (i + 2 < kKeyLen ? static_cast<uint32_t>(key[i + 2]) : 0);
        out.push_back(kBase64Alphabet[(triple >> 18) & 0x3f]);
        out.push_back(kBase64Alphabet[(triple >> 12) & 0x3f]);
        out.push_back(i + 1 < kKeyLen ? kBase64Alphabet[(triple >> 6) & 0x3f] : '=');
        out.push_back(i + 2 < kKeyLen ? kBase64Alphabet[triple & 0x3f] : '=');
    }
    return out;
}

bool key_from_base64(const std::string& text, uint8_t out[kKeyLen]) {
    auto value_of = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;
    };

    uint32_t accumulator = 0;
    int bits = 0;
    size_t written = 0;

    for (const char c : text) {
        if (c == '=' || c == '\n' || c == '\r' || c == ' ') continue;
        const int v = value_of(c);
        if (v < 0) return false;

        accumulator = accumulator << 6 | static_cast<uint32_t>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (written >= kKeyLen) return false;
            out[written++] = static_cast<uint8_t>(accumulator >> bits);
        }
    }
    return written == kKeyLen;
}

void initial_chaining_key(uint8_t out[kKeyLen]) {
    hash(out, kConstruction, sizeof(kConstruction) - 1, nullptr, 0);
}

// ---------------------------------------------------------------------------

void Peer::configure(const uint8_t private_key[kKeyLen], const uint8_t peer_public[kKeyLen],
                     const uint8_t* preshared) {
    std::memcpy(static_private_, private_key, kKeyLen);
    derive_public_key(static_public_, static_private_);
    std::memcpy(peer_public_, peer_public, kKeyLen);

    if (preshared) {
        std::memcpy(preshared_, preshared, kKeyLen);
    } else {
        std::memset(preshared_, 0, kKeyLen);
    }

    reset_handshake();
}

void Peer::reset_handshake() {
    established_ = false;
    handshake_in_progress_ = false;
    send_counter_ = 0;
    have_recv_ = false;
    recv_highest_ = 0;
    recv_window_ = 0;
    std::memset(send_key_, 0, kKeyLen);
    std::memset(recv_key_, 0, kKeyLen);
}

bool Peer::make_initiation(uint8_t out[kInitiationSize], uint64_t now_ms, uint64_t unix_ns) {
    std::memset(out, 0, kInitiationSize);

    // Ci = HASH(CONSTRUCTION) ; Hi = HASH(Ci || IDENTIFIER || Spub_r)
    initial_chaining_key(chaining_key_);
    hash(hash_, chaining_key_, 32, kIdentifier, sizeof(kIdentifier) - 1);
    mix_hash(hash_, peer_public_, 32);

    generate_private_key(ephemeral_private_);
    derive_public_key(ephemeral_public_, ephemeral_private_);

    util::random_bytes(reinterpret_cast<uint8_t*>(&local_index_), sizeof(local_index_));

    out[0] = kTypeInitiation;
    write_le32(out + 4, local_index_);
    std::memcpy(out + 8, ephemeral_public_, 32);

    // Ci = KDF1(Ci, Epub_i)
    kdf(chaining_key_, ephemeral_public_, 32, chaining_key_, nullptr, nullptr);
    mix_hash(hash_, ephemeral_public_, 32);

    // (Ci, k) = KDF2(Ci, DH(Epriv_i, Spub_r)) ; chiffre notre clé statique
    uint8_t shared[32];
    x25519_scalarmult(shared, ephemeral_private_, peer_public_);
    if (x25519_is_zero(shared)) {
        last_error_ = "clé publique du serveur dégénérée";
        return false;
    }

    uint8_t key[32];
    kdf(chaining_key_, shared, 32, chaining_key_, key, nullptr);

    uint8_t nonce[12];
    chacha20poly1305_nonce(nonce, 0);
    chacha20poly1305_encrypt(out + 40, out + 40 + 32, static_public_, 32, hash_, 32, key, nonce);
    mix_hash(hash_, out + 40, 48);

    // (Ci, k) = KDF2(Ci, DH(Spriv_i, Spub_r)) ; chiffre l'horodatage
    x25519_scalarmult(shared, static_private_, peer_public_);
    if (x25519_is_zero(shared)) {
        last_error_ = "secret statique dégénéré";
        return false;
    }
    kdf(chaining_key_, shared, 32, chaining_key_, key, nullptr);

    uint8_t timestamp[12];
    tai64n(timestamp, unix_ns);
    chacha20poly1305_nonce(nonce, 0);
    chacha20poly1305_encrypt(out + 88, out + 88 + 12, timestamp, 12, hash_, 32, key, nonce);
    mix_hash(hash_, out + 88, 28);

    // mac1 couvre tout ce qui précède ; mac2 reste nul tant qu'aucun cookie
    // n'a été reçu (le serveur ne l'exige que sous charge).
    compute_mac1(out + 116, peer_public_, out, 116);

    std::memset(shared, 0, sizeof(shared));
    std::memset(key, 0, sizeof(key));

    handshake_in_progress_ = true;
    last_handshake_attempt_ms_ = now_ms;
    return true;
}

bool Peer::consume_response(const uint8_t* msg, size_t len, uint64_t now_ms) {
    if (len != kResponseSize) {
        last_error_ = "taille de réponse inattendue";
        return false;
    }
    if (msg[0] != kTypeResponse) {
        last_error_ = "type de message inattendu";
        return false;
    }
    if (!handshake_in_progress_) {
        last_error_ = "réponse sans initiation en cours";
        return false;
    }
    if (read_le32(msg + 8) != local_index_) {
        last_error_ = "réponse destinée à une autre session";
        return false;
    }

    remote_index_ = read_le32(msg + 4);
    const uint8_t* peer_ephemeral = msg + 12;

    // Cr = KDF1(Cr, Epub_r)
    kdf(chaining_key_, peer_ephemeral, 32, chaining_key_, nullptr, nullptr);
    mix_hash(hash_, peer_ephemeral, 32);

    uint8_t shared[32];

    // Cr = KDF1(Cr, DH(Epriv_i, Epub_r))
    x25519_scalarmult(shared, ephemeral_private_, peer_ephemeral);
    if (x25519_is_zero(shared)) {
        last_error_ = "éphémère du serveur dégénéré";
        return false;
    }
    kdf(chaining_key_, shared, 32, chaining_key_, nullptr, nullptr);

    // Cr = KDF1(Cr, DH(Spriv_i, Epub_r))
    x25519_scalarmult(shared, static_private_, peer_ephemeral);
    if (x25519_is_zero(shared)) {
        last_error_ = "secret croisé dégénéré";
        return false;
    }
    kdf(chaining_key_, shared, 32, chaining_key_, nullptr, nullptr);

    // (Cr, τ, k) = KDF3(Cr, Q)
    uint8_t tau[32];
    uint8_t key[32];
    kdf(chaining_key_, preshared_, 32, chaining_key_, tau, key);
    mix_hash(hash_, tau, 32);

    uint8_t nonce[12];
    chacha20poly1305_nonce(nonce, 0);
    if (!chacha20poly1305_decrypt(nullptr, msg + 44, 0, msg + 44, hash_, 32, key, nonce)) {
        last_error_ = "authentification de la réponse échouée";
        std::memset(key, 0, sizeof(key));
        return false;
    }
    mix_hash(hash_, msg + 44, 16);

    std::memset(shared, 0, sizeof(shared));
    std::memset(tau, 0, sizeof(tau));
    std::memset(key, 0, sizeof(key));

    if (!derive_transport_keys(/*initiator=*/true)) return false;

    established_ = true;
    handshake_in_progress_ = false;
    established_at_ms_ = now_ms;
    last_send_ms_ = now_ms;
    last_recv_ms_ = now_ms;
    return true;
}

bool Peer::derive_transport_keys(bool initiator) {
    uint8_t first[32];
    uint8_t second[32];
    kdf(chaining_key_, nullptr, 0, first, second, nullptr);

    if (initiator) {
        std::memcpy(send_key_, first, 32);
        std::memcpy(recv_key_, second, 32);
    } else {
        std::memcpy(recv_key_, first, 32);
        std::memcpy(send_key_, second, 32);
    }

    send_counter_ = 0;
    have_recv_ = false;
    recv_highest_ = 0;
    recv_window_ = 0;

    std::memset(first, 0, sizeof(first));
    std::memset(second, 0, sizeof(second));
    std::memset(chaining_key_, 0, sizeof(chaining_key_));
    std::memset(ephemeral_private_, 0, sizeof(ephemeral_private_));
    return true;
}

bool Peer::needs_handshake(uint64_t now_ms) const {
    if (!established_) {
        // On ne réessaie pas plus vite que le délai de la spécification.
        return now_ms - last_handshake_attempt_ms_ >= kRekeyTimeoutMs ||
               last_handshake_attempt_ms_ == 0;
    }
    return now_ms - established_at_ms_ >= kRekeyAfterMs;
}

bool Peer::needs_keepalive(uint64_t now_ms) const {
    if (!established_) return false;
    return now_ms - last_send_ms_ >= kKeepaliveMs;
}

int Peer::encapsulate(const uint8_t* packet, size_t len, uint8_t* out, size_t out_capacity,
                      uint64_t now_ms) {
    if (!established_) {
        last_error_ = "tunnel non établi";
        return -1;
    }
    if (now_ms - established_at_ms_ >= kRejectAfterMs) {
        last_error_ = "clés expirées";
        established_ = false;
        return -1;
    }

    const size_t total = kDataHeaderSize + len + kTagLen;
    if (out_capacity < total) return -1;

    std::memset(out, 0, kDataHeaderSize);
    out[0] = kTypeData;
    write_le32(out + 4, remote_index_);
    write_le64(out + 8, send_counter_);

    uint8_t nonce[12];
    chacha20poly1305_nonce(nonce, send_counter_);
    chacha20poly1305_encrypt(out + kDataHeaderSize, out + kDataHeaderSize + len, packet, len,
                             nullptr, 0, send_key_, nonce);

    ++send_counter_;
    last_send_ms_ = now_ms;
    return static_cast<int>(total);
}

int Peer::decapsulate(const uint8_t* msg, size_t len, uint8_t* out, size_t out_capacity,
                      uint64_t now_ms) {
    if (!established_) return -1;
    if (len < kDataHeaderSize + kTagLen) return -1;
    if (msg[0] != kTypeData) return -1;

    const uint64_t counter = read_le64(msg + 8);

    // Anti-rejeu : fenêtre glissante de 64 messages.
    if (have_recv_) {
        if (counter + 64 <= recv_highest_) {
            last_error_ = "paquet trop ancien";
            return -1;
        }
        if (counter <= recv_highest_) {
            const uint64_t shift = recv_highest_ - counter;
            if ((recv_window_ >> shift) & 1) {
                last_error_ = "paquet rejoué";
                return -1;
            }
        }
    }

    const size_t payload_len = len - kDataHeaderSize - kTagLen;
    if (out_capacity < payload_len) return -1;

    uint8_t nonce[12];
    chacha20poly1305_nonce(nonce, counter);
    if (!chacha20poly1305_decrypt(out, msg + kDataHeaderSize, payload_len,
                                  msg + kDataHeaderSize + payload_len, nullptr, 0, recv_key_,
                                  nonce)) {
        last_error_ = "authentification du paquet échouée";
        return -1;
    }

    // Le compteur n'est intégré à la fenêtre qu'une fois l'authenticité prouvée :
    // sinon n'importe qui pourrait faire glisser la fenêtre avec du bruit.
    if (!have_recv_) {
        have_recv_ = true;
        recv_highest_ = counter;
        recv_window_ = 1;
    } else if (counter > recv_highest_) {
        const uint64_t shift = counter - recv_highest_;
        recv_window_ = shift >= 64 ? 1 : ((recv_window_ << shift) | 1);
        recv_highest_ = counter;
    } else {
        recv_window_ |= 1ULL << (recv_highest_ - counter);
    }

    last_recv_ms_ = now_ms;
    return static_cast<int>(payload_len);
}

// --- chemin « répondeur », uniquement pour les tests croisés ---

bool Peer::consume_initiation_for_test(const uint8_t* msg, size_t len) {
    if (len != kInitiationSize || msg[0] != kTypeInitiation) return false;

    initial_chaining_key(chaining_key_);
    hash(hash_, chaining_key_, 32, kIdentifier, sizeof(kIdentifier) - 1);
    mix_hash(hash_, static_public_, 32);  // côté répondeur : notre propre clé

    const uint8_t* peer_ephemeral = msg + 8;
    std::memcpy(initiator_ephemeral_, peer_ephemeral, 32);
    kdf(chaining_key_, peer_ephemeral, 32, chaining_key_, nullptr, nullptr);
    mix_hash(hash_, peer_ephemeral, 32);

    uint8_t shared[32];
    uint8_t key[32];
    uint8_t nonce[12];

    x25519_scalarmult(shared, static_private_, peer_ephemeral);
    kdf(chaining_key_, shared, 32, chaining_key_, key, nullptr);

    uint8_t initiator_static[32];
    chacha20poly1305_nonce(nonce, 0);
    if (!chacha20poly1305_decrypt(initiator_static, msg + 40, 32, msg + 72, hash_, 32, key,
                                  nonce)) {
        return false;
    }
    mix_hash(hash_, msg + 40, 48);

    x25519_scalarmult(shared, static_private_, initiator_static);
    kdf(chaining_key_, shared, 32, chaining_key_, key, nullptr);

    uint8_t timestamp[12];
    chacha20poly1305_nonce(nonce, 0);
    if (!chacha20poly1305_decrypt(timestamp, msg + 88, 12, msg + 100, hash_, 32, key, nonce)) {
        return false;
    }
    mix_hash(hash_, msg + 88, 28);

    std::memcpy(peer_public_, initiator_static, 32);
    remote_index_ = read_le32(msg + 4);
    handshake_in_progress_ = true;
    return true;
}

bool Peer::make_response_for_test(uint8_t out[kResponseSize]) {
    if (!handshake_in_progress_) return false;

    std::memset(out, 0, kResponseSize);
    generate_private_key(ephemeral_private_);
    derive_public_key(ephemeral_public_, ephemeral_private_);
    util::random_bytes(reinterpret_cast<uint8_t*>(&local_index_), sizeof(local_index_));

    out[0] = kTypeResponse;
    write_le32(out + 4, local_index_);
    write_le32(out + 8, remote_index_);
    std::memcpy(out + 12, ephemeral_public_, 32);

    kdf(chaining_key_, ephemeral_public_, 32, chaining_key_, nullptr, nullptr);
    mix_hash(hash_, ephemeral_public_, 32);

    // DH avec l'éphémère de l'initiateur, puis avec sa clé statique.
    uint8_t dh[32];
    x25519_scalarmult(dh, ephemeral_private_, initiator_ephemeral_);
    kdf(chaining_key_, dh, 32, chaining_key_, nullptr, nullptr);

    x25519_scalarmult(dh, ephemeral_private_, peer_public_);
    kdf(chaining_key_, dh, 32, chaining_key_, nullptr, nullptr);

    uint8_t tau[32];
    uint8_t key[32];
    kdf(chaining_key_, preshared_, 32, chaining_key_, tau, key);
    mix_hash(hash_, tau, 32);

    uint8_t nonce[12];
    chacha20poly1305_nonce(nonce, 0);
    chacha20poly1305_encrypt(nullptr, out + 44, nullptr, 0, hash_, 32, key, nonce);
    mix_hash(hash_, out + 44, 16);

    compute_mac1(out + 60, peer_public_, out, 60);

    derive_transport_keys(/*initiator=*/false);
    established_ = true;
    handshake_in_progress_ = false;
    return true;
}

}  // namespace wg
