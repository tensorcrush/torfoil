#include "util/sha1.hpp"

#include <cstring>

namespace util {

namespace {

inline uint32_t rotl(uint32_t v, int n) {
    return v << n | v >> (32 - n);
}

}  // namespace

void Sha1::reset() {
    h_[0] = 0x67452301u;
    h_[1] = 0xEFCDAB89u;
    h_[2] = 0x98BADCFEu;
    h_[3] = 0x10325476u;
    h_[4] = 0xC3D2E1F0u;
    total_bits_ = 0;
    buf_len_ = 0;
}

void Sha1::transform(const uint8_t block[64]) {
    uint32_t w[80];
    for (int i = 0; i < 16; ++i) w[i] = rd_be32(block + i * 4);
    for (int i = 16; i < 80; ++i) w[i] = rotl(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);

    uint32_t a = h_[0], b = h_[1], c = h_[2], d = h_[3], e = h_[4];

    for (int i = 0; i < 80; ++i) {
        uint32_t f, k;
        if (i < 20) {
            f = (b & c) | (~b & d);
            k = 0x5A827999u;
        } else if (i < 40) {
            f = b ^ c ^ d;
            k = 0x6ED9EBA1u;
        } else if (i < 60) {
            f = (b & c) | (b & d) | (c & d);
            k = 0x8F1BBCDCu;
        } else {
            f = b ^ c ^ d;
            k = 0xCA62C1D6u;
        }

        const uint32_t tmp = rotl(a, 5) + f + e + k + w[i];
        e = d;
        d = c;
        c = rotl(b, 30);
        b = a;
        a = tmp;
    }

    h_[0] += a;
    h_[1] += b;
    h_[2] += c;
    h_[3] += d;
    h_[4] += e;
}

void Sha1::update(const uint8_t* data, size_t len) {
    total_bits_ += static_cast<uint64_t>(len) * 8;

    // Complète le bloc partiel en attente.
    if (buf_len_ > 0) {
        const size_t need = 64 - buf_len_;
        const size_t take = len < need ? len : need;
        std::memcpy(buf_ + buf_len_, data, take);
        buf_len_ += take;
        data += take;
        len -= take;
        if (buf_len_ < 64) return;
        transform(buf_);
        buf_len_ = 0;
    }

    while (len >= 64) {
        transform(data);
        data += 64;
        len -= 64;
    }

    if (len > 0) {
        std::memcpy(buf_, data, len);
        buf_len_ = len;
    }
}

Hash160 Sha1::digest() {
    const uint64_t bits = total_bits_;

    // Padding : 0x80 puis des zéros jusqu'à 56 mod 64, puis la longueur en bits.
    uint8_t pad = 0x80;
    update(&pad, 1);
    total_bits_ = bits;  // update() a compté le padding, on rétablit

    pad = 0x00;
    while (buf_len_ != 56) {
        update(&pad, 1);
        total_bits_ = bits;
    }

    uint8_t len_be[8];
    wr_be64(len_be, bits);
    std::memcpy(buf_ + 56, len_be, 8);
    transform(buf_);
    buf_len_ = 0;

    Hash160 out{};
    for (int i = 0; i < 5; ++i) wr_be32(out.data() + i * 4, h_[i]);
    return out;
}

Hash160 Sha1::of(const uint8_t* data, size_t len) {
    Sha1 s;
    s.update(data, len);
    return s.digest();
}

Hash160 Sha1::of(const std::string& s) {
    return of(reinterpret_cast<const uint8_t*>(s.data()), s.size());
}

}  // namespace util
