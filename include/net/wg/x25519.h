#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void x25519_scalarmult(uint8_t out[32], const uint8_t scalar[32], const uint8_t point[32]);
void x25519_public_key(uint8_t out[32], const uint8_t secret[32]);

// Un secret partagé nul signale un point d'ordre faible : la RFC 7748 impose de
// rejeter l'échange dans ce cas.
int x25519_is_zero(const uint8_t value[32]);

#ifdef __cplusplus
}
#endif
