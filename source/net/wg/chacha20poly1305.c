// ChaCha20-Poly1305 (RFC 8439) — chiffrement authentifié de WireGuard.
#include "net/wg/chacha20poly1305.h"

#include <string.h>

// ---------------------------------------------------------------------------
// ChaCha20
// ---------------------------------------------------------------------------

static uint32_t load32_le(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static void store32_le(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static uint32_t rotl32(uint32_t x, int n) {
    return (x << n) | (x >> (32 - n));
}

#define QUARTERROUND(a, b, c, d)     \
    do {                             \
        a += b; d ^= a; d = rotl32(d, 16); \
        c += d; b ^= c; b = rotl32(b, 12); \
        a += b; d ^= a; d = rotl32(d, 8);  \
        c += d; b ^= c; b = rotl32(b, 7);  \
    } while (0)

static void chacha20_block(const uint32_t state[16], uint8_t out[64]) {
    uint32_t x[16];
    memcpy(x, state, sizeof(x));

    for (int i = 0; i < 10; ++i) {
        QUARTERROUND(x[0], x[4], x[8], x[12]);
        QUARTERROUND(x[1], x[5], x[9], x[13]);
        QUARTERROUND(x[2], x[6], x[10], x[14]);
        QUARTERROUND(x[3], x[7], x[11], x[15]);
        QUARTERROUND(x[0], x[5], x[10], x[15]);
        QUARTERROUND(x[1], x[6], x[11], x[12]);
        QUARTERROUND(x[2], x[7], x[8], x[13]);
        QUARTERROUND(x[3], x[4], x[9], x[14]);
    }

    for (int i = 0; i < 16; ++i) store32_le(out + i * 4, x[i] + state[i]);
}

static void chacha20_init(uint32_t state[16], const uint8_t key[32], const uint8_t nonce[12],
                          uint32_t counter) {
    state[0] = 0x61707865;  // "expa"
    state[1] = 0x3320646e;  // "nd 3"
    state[2] = 0x79622d32;  // "2-by"
    state[3] = 0x6b206574;  // "te k"
    for (int i = 0; i < 8; ++i) state[4 + i] = load32_le(key + i * 4);
    state[12] = counter;
    for (int i = 0; i < 3; ++i) state[13 + i] = load32_le(nonce + i * 4);
}

void chacha20_xor(uint8_t* out, const uint8_t* in, size_t len, const uint8_t key[32],
                  const uint8_t nonce[12], uint32_t counter) {
    uint32_t state[16];
    uint8_t block[64];
    chacha20_init(state, key, nonce, counter);

    while (len > 0) {
        chacha20_block(state, block);
        ++state[12];

        const size_t take = len < 64 ? len : 64;
        for (size_t i = 0; i < take; ++i) out[i] = in[i] ^ block[i];
        out += take;
        in += take;
        len -= take;
    }

    memset(block, 0, sizeof(block));
    memset(state, 0, sizeof(state));
}

// ---------------------------------------------------------------------------
// Poly1305 (représentation en 5 membres de 26 bits)
// ---------------------------------------------------------------------------

typedef struct {
    uint32_t r[5];
    uint32_t h[5];
    uint32_t pad[4];
    size_t leftover;
    uint8_t buffer[16];
    uint8_t final;
} poly1305_ctx;

static void poly1305_init(poly1305_ctx* ctx, const uint8_t key[32]) {
    // r est bridé selon la spécification.
    ctx->r[0] = (load32_le(key + 0)) & 0x3ffffff;
    ctx->r[1] = (load32_le(key + 3) >> 2) & 0x3ffff03;
    ctx->r[2] = (load32_le(key + 6) >> 4) & 0x3ffc0ff;
    ctx->r[3] = (load32_le(key + 9) >> 6) & 0x3f03fff;
    ctx->r[4] = (load32_le(key + 12) >> 8) & 0x00fffff;

    for (int i = 0; i < 5; ++i) ctx->h[i] = 0;
    for (int i = 0; i < 4; ++i) ctx->pad[i] = load32_le(key + 16 + i * 4);

    ctx->leftover = 0;
    ctx->final = 0;
}

static void poly1305_blocks(poly1305_ctx* ctx, const uint8_t* m, size_t bytes) {
    const uint32_t hibit = ctx->final ? 0 : (1UL << 24);

    const uint32_t r0 = ctx->r[0], r1 = ctx->r[1], r2 = ctx->r[2], r3 = ctx->r[3],
                   r4 = ctx->r[4];
    const uint32_t s1 = r1 * 5, s2 = r2 * 5, s3 = r3 * 5, s4 = r4 * 5;

    uint32_t h0 = ctx->h[0], h1 = ctx->h[1], h2 = ctx->h[2], h3 = ctx->h[3], h4 = ctx->h[4];

    while (bytes >= 16) {
        h0 += (load32_le(m + 0)) & 0x3ffffff;
        h1 += (load32_le(m + 3) >> 2) & 0x3ffffff;
        h2 += (load32_le(m + 6) >> 4) & 0x3ffffff;
        h3 += (load32_le(m + 9) >> 6) & 0x3ffffff;
        h4 += (load32_le(m + 12) >> 8) | hibit;

        uint64_t d0 = (uint64_t)h0 * r0 + (uint64_t)h1 * s4 + (uint64_t)h2 * s3 +
                      (uint64_t)h3 * s2 + (uint64_t)h4 * s1;
        uint64_t d1 = (uint64_t)h0 * r1 + (uint64_t)h1 * r0 + (uint64_t)h2 * s4 +
                      (uint64_t)h3 * s3 + (uint64_t)h4 * s2;
        uint64_t d2 = (uint64_t)h0 * r2 + (uint64_t)h1 * r1 + (uint64_t)h2 * r0 +
                      (uint64_t)h3 * s4 + (uint64_t)h4 * s3;
        uint64_t d3 = (uint64_t)h0 * r3 + (uint64_t)h1 * r2 + (uint64_t)h2 * r1 +
                      (uint64_t)h3 * r0 + (uint64_t)h4 * s4;
        uint64_t d4 = (uint64_t)h0 * r4 + (uint64_t)h1 * r3 + (uint64_t)h2 * r2 +
                      (uint64_t)h3 * r1 + (uint64_t)h4 * r0;

        uint32_t c = (uint32_t)(d0 >> 26);
        h0 = (uint32_t)d0 & 0x3ffffff;
        d1 += c;
        c = (uint32_t)(d1 >> 26);
        h1 = (uint32_t)d1 & 0x3ffffff;
        d2 += c;
        c = (uint32_t)(d2 >> 26);
        h2 = (uint32_t)d2 & 0x3ffffff;
        d3 += c;
        c = (uint32_t)(d3 >> 26);
        h3 = (uint32_t)d3 & 0x3ffffff;
        d4 += c;
        c = (uint32_t)(d4 >> 26);
        h4 = (uint32_t)d4 & 0x3ffffff;
        h0 += c * 5;
        c = h0 >> 26;
        h0 &= 0x3ffffff;
        h1 += c;

        m += 16;
        bytes -= 16;
    }

    ctx->h[0] = h0;
    ctx->h[1] = h1;
    ctx->h[2] = h2;
    ctx->h[3] = h3;
    ctx->h[4] = h4;
}

static void poly1305_update(poly1305_ctx* ctx, const uint8_t* m, size_t bytes) {
    if (ctx->leftover > 0) {
        size_t want = 16 - ctx->leftover;
        if (want > bytes) want = bytes;
        memcpy(ctx->buffer + ctx->leftover, m, want);
        bytes -= want;
        m += want;
        ctx->leftover += want;
        if (ctx->leftover < 16) return;
        poly1305_blocks(ctx, ctx->buffer, 16);
        ctx->leftover = 0;
    }

    if (bytes >= 16) {
        const size_t want = bytes & ~((size_t)15);
        poly1305_blocks(ctx, m, want);
        m += want;
        bytes -= want;
    }

    if (bytes > 0) {
        memcpy(ctx->buffer + ctx->leftover, m, bytes);
        ctx->leftover += bytes;
    }
}

static void poly1305_finish(poly1305_ctx* ctx, uint8_t mac[16]) {
    if (ctx->leftover > 0) {
        ctx->buffer[ctx->leftover++] = 1;
        memset(ctx->buffer + ctx->leftover, 0, 16 - ctx->leftover);
        ctx->final = 1;
        poly1305_blocks(ctx, ctx->buffer, 16);
    }

    uint32_t h0 = ctx->h[0], h1 = ctx->h[1], h2 = ctx->h[2], h3 = ctx->h[3], h4 = ctx->h[4];

    uint32_t c = h1 >> 26;
    h1 &= 0x3ffffff;
    h2 += c;
    c = h2 >> 26;
    h2 &= 0x3ffffff;
    h3 += c;
    c = h3 >> 26;
    h3 &= 0x3ffffff;
    h4 += c;
    c = h4 >> 26;
    h4 &= 0x3ffffff;
    h0 += c * 5;
    c = h0 >> 26;
    h0 &= 0x3ffffff;
    h1 += c;

    // h - p, pour choisir la représentation réduite.
    uint32_t g0 = h0 + 5;
    c = g0 >> 26;
    g0 &= 0x3ffffff;
    uint32_t g1 = h1 + c;
    c = g1 >> 26;
    g1 &= 0x3ffffff;
    uint32_t g2 = h2 + c;
    c = g2 >> 26;
    g2 &= 0x3ffffff;
    uint32_t g3 = h3 + c;
    c = g3 >> 26;
    g3 &= 0x3ffffff;
    uint32_t g4 = h4 + c - (1UL << 26);

    uint32_t mask = (g4 >> ((sizeof(uint32_t) * 8) - 1)) - 1;
    g0 &= mask;
    g1 &= mask;
    g2 &= mask;
    g3 &= mask;
    g4 &= mask;
    mask = ~mask;
    h0 = (h0 & mask) | g0;
    h1 = (h1 & mask) | g1;
    h2 = (h2 & mask) | g2;
    h3 = (h3 & mask) | g3;
    h4 = (h4 & mask) | g4;

    // Retour à quatre mots de 32 bits.
    h0 = (h0 | (h1 << 26)) & 0xffffffff;
    h1 = ((h1 >> 6) | (h2 << 20)) & 0xffffffff;
    h2 = ((h2 >> 12) | (h3 << 14)) & 0xffffffff;
    h3 = ((h3 >> 18) | (h4 << 8)) & 0xffffffff;

    uint64_t f = (uint64_t)h0 + ctx->pad[0];
    h0 = (uint32_t)f;
    f = (uint64_t)h1 + ctx->pad[1] + (f >> 32);
    h1 = (uint32_t)f;
    f = (uint64_t)h2 + ctx->pad[2] + (f >> 32);
    h2 = (uint32_t)f;
    f = (uint64_t)h3 + ctx->pad[3] + (f >> 32);
    h3 = (uint32_t)f;

    store32_le(mac + 0, h0);
    store32_le(mac + 4, h1);
    store32_le(mac + 8, h2);
    store32_le(mac + 12, h3);

    memset(ctx, 0, sizeof(*ctx));
}

// ---------------------------------------------------------------------------
// AEAD
// ---------------------------------------------------------------------------

static void poly1305_pad16(poly1305_ctx* ctx, size_t len) {
    static const uint8_t zeros[16] = {0};
    const size_t rem = len % 16;
    if (rem != 0) poly1305_update(ctx, zeros, 16 - rem);
}

static void poly1305_length(poly1305_ctx* ctx, uint64_t aad_len, uint64_t data_len) {
    uint8_t tail[16];
    for (int i = 0; i < 8; ++i) tail[i] = (uint8_t)(aad_len >> (8 * i));
    for (int i = 0; i < 8; ++i) tail[8 + i] = (uint8_t)(data_len >> (8 * i));
    poly1305_update(ctx, tail, sizeof(tail));
}

void chacha20poly1305_encrypt(uint8_t* ciphertext, uint8_t tag[16], const uint8_t* plaintext,
                              size_t plaintext_len, const uint8_t* aad, size_t aad_len,
                              const uint8_t key[32], const uint8_t nonce[12]) {
    // Le bloc 0 du flux fournit la clé Poly1305 ; les données commencent au bloc 1.
    uint8_t poly_key[64];
    uint8_t zeros[64];
    memset(zeros, 0, sizeof(zeros));
    chacha20_xor(poly_key, zeros, sizeof(poly_key), key, nonce, 0);

    chacha20_xor(ciphertext, plaintext, plaintext_len, key, nonce, 1);

    poly1305_ctx ctx;
    poly1305_init(&ctx, poly_key);
    if (aad_len > 0) poly1305_update(&ctx, aad, aad_len);
    poly1305_pad16(&ctx, aad_len);
    if (plaintext_len > 0) poly1305_update(&ctx, ciphertext, plaintext_len);
    poly1305_pad16(&ctx, plaintext_len);
    poly1305_length(&ctx, aad_len, plaintext_len);
    poly1305_finish(&ctx, tag);

    memset(poly_key, 0, sizeof(poly_key));
}

int chacha20poly1305_decrypt(uint8_t* plaintext, const uint8_t* ciphertext, size_t ciphertext_len,
                             const uint8_t tag[16], const uint8_t* aad, size_t aad_len,
                             const uint8_t key[32], const uint8_t nonce[12]) {
    uint8_t poly_key[64];
    uint8_t zeros[64];
    memset(zeros, 0, sizeof(zeros));
    chacha20_xor(poly_key, zeros, sizeof(poly_key), key, nonce, 0);

    poly1305_ctx ctx;
    poly1305_init(&ctx, poly_key);
    if (aad_len > 0) poly1305_update(&ctx, aad, aad_len);
    poly1305_pad16(&ctx, aad_len);
    if (ciphertext_len > 0) poly1305_update(&ctx, ciphertext, ciphertext_len);
    poly1305_pad16(&ctx, ciphertext_len);
    poly1305_length(&ctx, aad_len, ciphertext_len);

    uint8_t expected[16];
    poly1305_finish(&ctx, expected);
    memset(poly_key, 0, sizeof(poly_key));

    // Comparaison en temps constant : sortir tôt renseignerait un attaquant.
    uint8_t diff = 0;
    for (int i = 0; i < 16; ++i) diff |= (uint8_t)(expected[i] ^ tag[i]);
    if (diff != 0) return 0;

    chacha20_xor(plaintext, ciphertext, ciphertext_len, key, nonce, 1);
    return 1;
}

void chacha20poly1305_nonce(uint8_t nonce[12], uint64_t counter) {
    // WireGuard : 4 octets nuls puis le compteur en petit-boutiste.
    memset(nonce, 0, 4);
    for (int i = 0; i < 8; ++i) nonce[4 + i] = (uint8_t)(counter >> (8 * i));
}
