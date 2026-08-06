// SHA-1 autonome.
//
// BitTorrent en dépend partout (info_hash, vérification de chaque pièce), et on
// le veut disponible avant même l'initialisation de mbedtls/du réseau. ~150
// lignes sans dépendance, ça évite de traîner une lib pour une primitive.
#pragma once

#include "util/bytes.hpp"

namespace util {

class Sha1 {
public:
    Sha1() { reset(); }

    void reset();
    void update(const uint8_t* data, size_t len);
    void update(const std::string& s) {
        update(reinterpret_cast<const uint8_t*>(s.data()), s.size());
    }

    // Finalise et renvoie l'empreinte. Appeler reset() pour réutiliser l'objet.
    Hash160 digest();

    static Hash160 of(const uint8_t* data, size_t len);
    static Hash160 of(const std::string& s);

private:
    void transform(const uint8_t block[64]);

    uint32_t h_[5];
    uint64_t total_bits_;
    uint8_t buf_[64];
    size_t buf_len_;
};

}  // namespace util
