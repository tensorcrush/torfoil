// Petits utilitaires binaires partagés par le moteur BitTorrent et la couche réseau.
// Volontairement sans dépendance à libnx pour rester compilable sur PC (tests unitaires).
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace util {

using Bytes = std::vector<uint8_t>;
using Hash160 = std::array<uint8_t, 20>;

// --- lecture/écriture big-endian (l'ordre réseau, celui du protocole BitTorrent) ---

inline uint16_t rd_be16(const uint8_t* p) {
    return static_cast<uint16_t>(static_cast<uint16_t>(p[0]) << 8 | p[1]);
}

inline uint32_t rd_be32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) << 24 | static_cast<uint32_t>(p[1]) << 16 |
           static_cast<uint32_t>(p[2]) << 8 | static_cast<uint32_t>(p[3]);
}

inline uint64_t rd_be64(const uint8_t* p) {
    return static_cast<uint64_t>(rd_be32(p)) << 32 | rd_be32(p + 4);
}

inline void wr_be16(uint8_t* p, uint16_t v) {
    p[0] = static_cast<uint8_t>(v >> 8);
    p[1] = static_cast<uint8_t>(v);
}

inline void wr_be32(uint8_t* p, uint32_t v) {
    p[0] = static_cast<uint8_t>(v >> 24);
    p[1] = static_cast<uint8_t>(v >> 16);
    p[2] = static_cast<uint8_t>(v >> 8);
    p[3] = static_cast<uint8_t>(v);
}

inline void wr_be64(uint8_t* p, uint64_t v) {
    wr_be32(p, static_cast<uint32_t>(v >> 32));
    wr_be32(p + 4, static_cast<uint32_t>(v));
}

// --- encodages ---

std::string to_hex(const uint8_t* data, size_t len);

template <size_t N>
inline std::string to_hex(const std::array<uint8_t, N>& a) {
    return to_hex(a.data(), N);
}

// Décode `hex` (2*out_len caractères, casse indifférente) dans `out`.
bool from_hex(const std::string& hex, uint8_t* out, size_t out_len);

// Base32 RFC 4648 sans padding — utilisé par les magnets « btih » 32 caractères.
bool from_base32(const std::string& in, uint8_t* out, size_t out_len);

// Percent-encoding strict (RFC 3986) : tout sauf [A-Za-z0-9-_.~] est échappé.
// Indispensable pour info_hash/peer_id dans une annonce tracker HTTP.
std::string url_encode(const uint8_t* data, size_t len);
std::string url_encode(const std::string& s);

// Décode un %XX ; laisse les octets invalides tels quels.
std::string url_decode(const std::string& s);

// CSRNG de la console (randomGet) sur Switch, std::random_device ailleurs.
void random_bytes(uint8_t* out, size_t len);

// --- affichage ---

std::string human_size(uint64_t bytes);            // "1,42 Go"
std::string human_rate(uint64_t bytes_per_sec);    // "3,10 Mo/s"
std::string human_duration(uint64_t seconds);      // "1 h 04 min"

// --- chaînes ---

std::string trim(const std::string& s);
bool starts_with(const std::string& s, const std::string& prefix);
bool iequals(const std::string& a, const std::string& b);
std::vector<std::string> split(const std::string& s, char sep);

// Remplace tout caractère interdit par FAT32/exFAT afin qu'un nom de fichier
// issu d'un torrent puisse atterrir tel quel sur la carte SD.
std::string sanitize_filename(const std::string& name);

}  // namespace util
