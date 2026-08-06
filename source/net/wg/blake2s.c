// BLAKE2s (RFC 7693) — fonction de hachage de WireGuard.
//
// Implémentation portable volontaire : le même code tourne sur la console et
// sur PC, ce qui permet de le confronter aux vecteurs de la RFC avant de
// l'embarquer.
#include "net/wg/blake2s.h"

#include <string.h>

static const uint32_t kIv[8] = {
    0x6A09E667UL, 0xBB67AE85UL, 0x3C6EF372UL, 0xA54FF53AUL,
    0x510E527FUL, 0x9B05688CUL, 0x1F83D9ABUL, 0x5BE0CD19UL,
};

static const uint8_t kSigma[10][16] = {
    {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15},
    {14, 10, 4, 8, 9, 15, 13, 6, 1, 12, 0, 2, 11, 7, 5, 3},
    {11, 8, 12, 0, 5, 2, 15, 13, 10, 14, 3, 6, 7, 1, 9, 4},
    {7, 9, 3, 1, 13, 12, 11, 14, 2, 6, 5, 10, 4, 0, 15, 8},
    {9, 0, 5, 7, 2, 4, 10, 15, 14, 1, 11, 12, 6, 8, 3, 13},
    {2, 12, 6, 10, 0, 11, 8, 3, 4, 13, 7, 5, 15, 14, 1, 9},
    {12, 5, 1, 15, 14, 13, 4, 10, 0, 7, 6, 3, 9, 2, 8, 11},
    {13, 11, 7, 14, 12, 1, 3, 9, 5, 0, 15, 4, 8, 6, 2, 10},
    {6, 15, 14, 9, 11, 3, 0, 8, 12, 2, 13, 7, 1, 4, 10, 5},
    {10, 2, 8, 4, 7, 6, 1, 5, 15, 11, 9, 14, 3, 12, 13, 0},
};

static uint32_t load32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static void store32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static uint32_t rotr32(uint32_t x, int n) {
    return (x >> n) | (x << (32 - n));
}

static void increment_counter(blake2s_state* s, uint32_t inc) {
    s->t[0] += inc;
    if (s->t[0] < inc) s->t[1]++;
}

static void compress(blake2s_state* s, const uint8_t block[BLAKE2S_BLOCK_SIZE]) {
    uint32_t m[16];
    uint32_t v[16];

    for (int i = 0; i < 16; ++i) m[i] = load32(block + i * 4);
    for (int i = 0; i < 8; ++i) v[i] = s->h[i];

    v[8] = kIv[0];
    v[9] = kIv[1];
    v[10] = kIv[2];
    v[11] = kIv[3];
    v[12] = s->t[0] ^ kIv[4];
    v[13] = s->t[1] ^ kIv[5];
    v[14] = s->f[0] ^ kIv[6];
    v[15] = s->f[1] ^ kIv[7];

#define G(r, i, a, b, c, d)                     \
    do {                                        \
        a = a + b + m[kSigma[r][2 * i + 0]];    \
        d = rotr32(d ^ a, 16);                  \
        c = c + d;                              \
        b = rotr32(b ^ c, 12);                  \
        a = a + b + m[kSigma[r][2 * i + 1]];    \
        d = rotr32(d ^ a, 8);                   \
        c = c + d;                              \
        b = rotr32(b ^ c, 7);                   \
    } while (0)

    for (int r = 0; r < 10; ++r) {
        G(r, 0, v[0], v[4], v[8], v[12]);
        G(r, 1, v[1], v[5], v[9], v[13]);
        G(r, 2, v[2], v[6], v[10], v[14]);
        G(r, 3, v[3], v[7], v[11], v[15]);
        G(r, 4, v[0], v[5], v[10], v[15]);
        G(r, 5, v[1], v[6], v[11], v[12]);
        G(r, 6, v[2], v[7], v[8], v[13]);
        G(r, 7, v[3], v[4], v[9], v[14]);
    }

#undef G

    for (int i = 0; i < 8; ++i) s->h[i] ^= v[i] ^ v[i + 8];
}

void blake2s_init(blake2s_state* s, size_t out_len) {
    blake2s_init_key(s, out_len, NULL, 0);
}

void blake2s_init_key(blake2s_state* s, size_t out_len, const void* key, size_t key_len) {
    memset(s, 0, sizeof(*s));
    for (int i = 0; i < 8; ++i) s->h[i] = kIv[i];

    // Bloc de paramètres : longueur de sortie, longueur de clé, fanout 1,
    // profondeur 1 (mode séquentiel).
    s->h[0] ^= 0x01010000UL ^ ((uint32_t)key_len << 8) ^ (uint32_t)out_len;
    s->out_len = out_len;

    if (key_len > 0) {
        uint8_t block[BLAKE2S_BLOCK_SIZE];
        memset(block, 0, sizeof(block));
        memcpy(block, key, key_len);
        blake2s_update(s, block, sizeof(block));
        memset(block, 0, sizeof(block));
    }
}

void blake2s_update(blake2s_state* s, const void* data, size_t len) {
    const uint8_t* in = (const uint8_t*)data;
    if (len == 0) return;

    size_t left = s->buf_len;
    const size_t fill = BLAKE2S_BLOCK_SIZE - left;

    if (len > fill) {
        s->buf_len = 0;
        memcpy(s->buf + left, in, fill);
        increment_counter(s, BLAKE2S_BLOCK_SIZE);
        compress(s, s->buf);
        in += fill;
        len -= fill;

        // On garde toujours au moins un octet en réserve : le dernier bloc doit
        // être compressé avec le drapeau de finalisation.
        while (len > BLAKE2S_BLOCK_SIZE) {
            increment_counter(s, BLAKE2S_BLOCK_SIZE);
            compress(s, in);
            in += BLAKE2S_BLOCK_SIZE;
            len -= BLAKE2S_BLOCK_SIZE;
        }
        left = 0;
    }

    memcpy(s->buf + left, in, len);
    s->buf_len = left + len;
}

void blake2s_final(blake2s_state* s, void* out) {
    increment_counter(s, (uint32_t)s->buf_len);
    s->f[0] = 0xFFFFFFFFUL;
    memset(s->buf + s->buf_len, 0, BLAKE2S_BLOCK_SIZE - s->buf_len);
    compress(s, s->buf);

    uint8_t buffer[BLAKE2S_OUT_SIZE];
    for (int i = 0; i < 8; ++i) store32(buffer + i * 4, s->h[i]);
    memcpy(out, buffer, s->out_len);
    memset(buffer, 0, sizeof(buffer));
}

void blake2s(void* out, size_t out_len, const void* key, size_t key_len, const void* data,
             size_t len) {
    blake2s_state s;
    blake2s_init_key(&s, out_len, key, key_len);
    blake2s_update(&s, data, len);
    blake2s_final(&s, out);
}

void blake2s_hmac(uint8_t out[BLAKE2S_OUT_SIZE], const void* key, size_t key_len,
                  const void* data, size_t len) {
    uint8_t block[BLAKE2S_BLOCK_SIZE];
    uint8_t inner[BLAKE2S_OUT_SIZE];

    memset(block, 0, sizeof(block));
    if (key_len > BLAKE2S_BLOCK_SIZE) {
        blake2s(block, BLAKE2S_OUT_SIZE, NULL, 0, key, key_len);
    } else {
        memcpy(block, key, key_len);
    }

    for (size_t i = 0; i < sizeof(block); ++i) block[i] ^= 0x36;

    blake2s_state s;
    blake2s_init(&s, BLAKE2S_OUT_SIZE);
    blake2s_update(&s, block, sizeof(block));
    blake2s_update(&s, data, len);
    blake2s_final(&s, inner);

    for (size_t i = 0; i < sizeof(block); ++i) block[i] ^= 0x36 ^ 0x5c;

    blake2s_init(&s, BLAKE2S_OUT_SIZE);
    blake2s_update(&s, block, sizeof(block));
    blake2s_update(&s, inner, sizeof(inner));
    blake2s_final(&s, out);

    memset(block, 0, sizeof(block));
    memset(inner, 0, sizeof(inner));
}
