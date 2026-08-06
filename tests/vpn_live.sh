#!/bin/bash
# Compile et lance le test VPN « en vrai » sur PC (Linux / WSL).
#
#   bash tests/vpn_live.sh 1234567890123456 [pays]
#
# Le numéro de compte n'est passé qu'en argument au binaire : il n'est ni
# journalisé, ni écrit, ni affiché en entier.
set -eo pipefail

SRC="$(cd "$(dirname "$0")/.." && pwd)"
BIN="${TMPDIR:-/tmp}/torfoil_vpn_live"

if [ $# -lt 1 ]; then
    echo "usage: bash tests/vpn_live.sh <numéro_de_compte_mullvad> [pays]" >&2
    exit 2
fi

if [ ! -f /usr/include/mbedtls/ssl.h ]; then
    echo "mbedtls manquant. Installe-le :" >&2
    echo "  sudo apt-get install -y libmbedtls-dev build-essential" >&2
    exit 1
fi

cd "$SRC"
echo "compilation (lwIP à compiler, compter une minute)…"

OBJ="${TMPDIR:-/tmp}/torfoil_lwip"
mkdir -p "$OBJ"

# lwIP est du C : le compiler avec gcc évite une avalanche d'avertissements C++
# sur du code tiers qu'on ne touchera pas.
for src in third_party/lwip/src/core/*.c third_party/lwip/src/core/ipv4/*.c; do
    out="$OBJ/$(basename "$src" .c).o"
    [ "$out" -nt "$src" ] || gcc -std=gnu11 -O1 -g -w \
        -Iinclude -Ithird_party/lwip/src/include -c "$src" -o "$out"
done

g++ -std=gnu++17 -O1 -g -Wall -Wextra -Wno-unused-parameter \
    -Iinclude -Ithird_party/lwip/src/include \
    tests/vpn_live.cpp \
    source/util/bytes.cpp \
    source/net/bsd_transport.cpp source/net/io.cpp source/net/json.cpp \
    source/net/tls.cpp source/net/mullvad.cpp \
    source/net/wg/blake2s.c source/net/wg/x25519.c source/net/wg/chacha20poly1305.c \
    source/net/wg/wireguard.cpp source/net/wg/tunnel.cpp \
    "$OBJ"/*.o \
    -lmbedtls -lmbedx509 -lmbedcrypto -lpthread \
    -o "$BIN"

echo
exec "$BIN" "$@"
