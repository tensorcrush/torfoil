#!/bin/bash
# Compile et exécute les tests hôte (g++ natif, aucune dépendance à libnx).
set -eo pipefail

cd "$(cd "$(dirname "$0")/.." && pwd)"

# miniz est du C : le passer a g++ le compilerait en C++, ou ses definitions
# provisoires deviennent des redefinitions.
MINIZ_O="${TMPDIR:-/tmp}/torfoil-miniz.o"
cc -O2 -std=gnu11 -w -c third_party/miniz/miniz.c -o "$MINIZ_O"

g++ -std=gnu++17 -O1 -g -Wall -Wextra -Wno-unused-parameter \
    -fsanitize=address,undefined \
    -Iinclude -Ithird_party/miniz \
    tests/host_tests.cpp \
    source/util/bytes.cpp source/util/sha1.cpp source/util/log.cpp \
    source/bt/bencode.cpp source/bt/magnet.cpp source/bt/metainfo.cpp \
    source/bt/piece_picker.cpp source/bt/storage.cpp \
    source/util/qr.cpp source/net/http_parse.cpp source/net/http_server.cpp \
    source/ui/lang.cpp source/util/archive.cpp source/bt/search.cpp source/net/json.cpp "$MINIZ_O" \
    source/net/wg/blake2s.c source/net/wg/x25519.c source/net/wg/chacha20poly1305.c \
    source/net/wg/wireguard.cpp \
    -o "${TMPDIR:-/tmp}/torfoil_tests"

exec "${TMPDIR:-/tmp}/torfoil_tests"
