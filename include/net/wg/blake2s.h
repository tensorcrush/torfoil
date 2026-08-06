#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BLAKE2S_BLOCK_SIZE 64
#define BLAKE2S_OUT_SIZE 32

typedef struct {
    uint32_t h[8];
    uint32_t t[2];
    uint32_t f[2];
    uint8_t buf[BLAKE2S_BLOCK_SIZE];
    size_t buf_len;
    size_t out_len;
} blake2s_state;

void blake2s_init(blake2s_state* s, size_t out_len);
void blake2s_init_key(blake2s_state* s, size_t out_len, const void* key, size_t key_len);
void blake2s_update(blake2s_state* s, const void* data, size_t len);
void blake2s_final(blake2s_state* s, void* out);

void blake2s(void* out, size_t out_len, const void* key, size_t key_len, const void* data,
             size_t len);

// HMAC construit sur BLAKE2s — c'est ce que WireGuard utilise pour son KDF,
// et non le mode clé natif de BLAKE2s.
void blake2s_hmac(uint8_t out[BLAKE2S_OUT_SIZE], const void* key, size_t key_len,
                  const void* data, size_t len);

#ifdef __cplusplus
}
#endif
