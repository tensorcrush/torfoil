#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void chacha20_xor(uint8_t* out, const uint8_t* in, size_t len, const uint8_t key[32],
                  const uint8_t nonce[12], uint32_t counter);

void chacha20poly1305_encrypt(uint8_t* ciphertext, uint8_t tag[16], const uint8_t* plaintext,
                              size_t plaintext_len, const uint8_t* aad, size_t aad_len,
                              const uint8_t key[32], const uint8_t nonce[12]);

// Renvoie 1 si l'authentification passe, 0 sinon (aucune écriture dans ce cas).
int chacha20poly1305_decrypt(uint8_t* plaintext, const uint8_t* ciphertext, size_t ciphertext_len,
                             const uint8_t tag[16], const uint8_t* aad, size_t aad_len,
                             const uint8_t key[32], const uint8_t nonce[12]);

// Nonce WireGuard : 4 octets nuls suivis du compteur 64 bits petit-boutiste.
void chacha20poly1305_nonce(uint8_t nonce[12], uint64_t counter);

#ifdef __cplusplus
}
#endif
