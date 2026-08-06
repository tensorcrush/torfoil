// X25519 (RFC 7748) — échange de clés de WireGuard.
//
// Implémentation dans l'esprit de TweetNaCl : arithmétique sur 16 membres de
// 16 bits, échelle de Montgomery en temps constant. Lente comparée aux
// implémentations 64 bits, mais une poignée de millisecondes toutes les deux
// minutes ne se voit pas, et le code tient en une page vérifiable.
#include "net/wg/x25519.h"

#include <string.h>

typedef int64_t gf[16];

static const gf k121665 = {0xDB41, 1};

static void car25519(gf o) {
    for (int i = 0; i < 16; ++i) {
        o[i] += (int64_t)1 << 16;
        const int64_t c = o[i] >> 16;
        // Le report du dernier membre revient dans le premier, multiplié par 38
        // (2^256 ≡ 38 mod 2^255-19).
        o[(i + 1) * (i < 15)] += c - 1 + 37 * (c - 1) * (i == 15);
        o[i] -= c << 16;
    }
}

// Échange conditionnel sans branchement : la valeur du bit de scalaire ne doit
// pas influencer le temps d'exécution.
static void sel25519(gf p, gf q, int b) {
    const int64_t mask = ~((int64_t)b - 1);
    for (int i = 0; i < 16; ++i) {
        const int64_t t = mask & (p[i] ^ q[i]);
        p[i] ^= t;
        q[i] ^= t;
    }
}

static void pack25519(uint8_t* o, const gf n) {
    gf m, t;
    for (int i = 0; i < 16; ++i) t[i] = n[i];
    car25519(t);
    car25519(t);
    car25519(t);

    for (int j = 0; j < 2; ++j) {
        m[0] = t[0] - 0xffed;
        for (int i = 1; i < 15; ++i) {
            m[i] = t[i] - 0xffff - ((m[i - 1] >> 16) & 1);
            m[i - 1] &= 0xffff;
        }
        m[15] = t[15] - 0x7fff - ((m[14] >> 16) & 1);
        const int b = (int)((m[15] >> 16) & 1);
        m[14] &= 0xffff;
        sel25519(t, m, 1 - b);
    }

    for (int i = 0; i < 16; ++i) {
        o[2 * i] = (uint8_t)(t[i] & 0xff);
        o[2 * i + 1] = (uint8_t)(t[i] >> 8);
    }
}

static void unpack25519(gf o, const uint8_t* n) {
    for (int i = 0; i < 16; ++i) o[i] = n[2 * i] + ((int64_t)n[2 * i + 1] << 8);
    o[15] &= 0x7fff;
}

static void fadd(gf o, const gf a, const gf b) {
    for (int i = 0; i < 16; ++i) o[i] = a[i] + b[i];
}

static void fsub(gf o, const gf a, const gf b) {
    for (int i = 0; i < 16; ++i) o[i] = a[i] - b[i];
}

static void fmul(gf o, const gf a, const gf b) {
    int64_t t[31];
    for (int i = 0; i < 31; ++i) t[i] = 0;
    for (int i = 0; i < 16; ++i) {
        for (int j = 0; j < 16; ++j) t[i + j] += a[i] * b[j];
    }
    for (int i = 0; i < 15; ++i) t[i] += 38 * t[i + 16];
    for (int i = 0; i < 16; ++i) o[i] = t[i];
    car25519(o);
    car25519(o);
}

static void fsquare(gf o, const gf a) {
    fmul(o, a, a);
}

// Inverse modulaire par exponentiation à p-2, chaîne d'addition fixe.
static void finverse(gf o, const gf i) {
    gf c;
    for (int a = 0; a < 16; ++a) c[a] = i[a];
    for (int a = 253; a >= 0; --a) {
        fsquare(c, c);
        if (a != 2 && a != 4) fmul(c, c, i);
    }
    for (int a = 0; a < 16; ++a) o[a] = c[a];
}

void x25519_scalarmult(uint8_t out[32], const uint8_t scalar[32], const uint8_t point[32]) {
    uint8_t z[32];
    gf x, a, b, c, d, e, f;

    for (int i = 0; i < 31; ++i) z[i] = scalar[i];
    // Bridage RFC 7748 : cofacteur purgé, bit de poids fort fixé.
    z[31] = (scalar[31] & 127) | 64;
    z[0] &= 248;

    unpack25519(x, point);
    for (int i = 0; i < 16; ++i) {
        b[i] = x[i];
        d[i] = a[i] = c[i] = 0;
    }
    a[0] = d[0] = 1;

    for (int i = 254; i >= 0; --i) {
        const int r = (z[i >> 3] >> (i & 7)) & 1;
        sel25519(a, b, r);
        sel25519(c, d, r);
        fadd(e, a, c);
        fsub(a, a, c);
        fadd(c, b, d);
        fsub(b, b, d);
        fsquare(d, e);
        fsquare(f, a);
        fmul(a, c, a);
        fmul(c, b, e);
        fadd(e, a, c);
        fsub(a, a, c);
        fsquare(b, a);
        fsub(c, d, f);
        fmul(a, c, k121665);
        fadd(a, a, d);
        fmul(c, c, a);
        fmul(a, d, f);
        fmul(d, b, x);
        fsquare(b, e);
        sel25519(a, b, r);
        sel25519(c, d, r);
    }

    finverse(c, c);
    fmul(a, a, c);
    pack25519(out, a);

    memset(z, 0, sizeof(z));
}

void x25519_public_key(uint8_t out[32], const uint8_t secret[32]) {
    static const uint8_t base[32] = {9};
    x25519_scalarmult(out, secret, base);
}

int x25519_is_zero(const uint8_t value[32]) {
    uint8_t acc = 0;
    for (int i = 0; i < 32; ++i) acc |= value[i];
    return acc == 0;
}
