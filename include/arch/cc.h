// Couche d'adaptation lwIP pour Horizon OS / newlib.
#pragma once

#include <inttypes.h>
#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <unistd.h>

// newlib fournit déjà ssize_t. Sans SSIZE_MAX, lwIP le redéfinirait en « int »
// et entrerait en conflit.
#ifndef SSIZE_MAX
#define SSIZE_MAX LONG_MAX
#endif
#define LWIP_NO_UNISTD_H 0

// newlib fournit errno et les types entiers standards.
#define LWIP_ERRNO_STDINCLUDE 1
#define LWIP_NO_INTTYPES_H 0
#define LWIP_NO_LIMITS_H 0
#define LWIP_NO_STDDEF_H 0
#define LWIP_NO_STDINT_H 0

// aarch64 sur Switch : petit-boutiste.
#ifndef BYTE_ORDER
#define BYTE_ORDER LITTLE_ENDIAN
#endif

// Les diagnostics de lwIP partent dans le vide : sur console il n'y a pas de
// sortie standard utile, et l'IHM a ses propres messages.
#define LWIP_PLATFORM_DIAG(x) \
    do {                      \
    } while (0)

#define LWIP_PLATFORM_ASSERT(x) \
    do {                        \
    } while (0)

#define LWIP_RAND() ((u32_t)rand())
