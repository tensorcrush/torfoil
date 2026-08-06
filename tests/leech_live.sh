#!/bin/bash
# Téléchargement réel de bout en bout, sur PC, avec le moteur embarqué.
#
#   bash tests/leech_live.sh <lien magnet | fichier .torrent> [secondes]
set -eo pipefail

SRC="$(cd "$(dirname "$0")/.." && pwd)"
BIN="${TMPDIR:-/tmp}/torfoil_leech_live"

if [ $# -lt 1 ]; then
    echo "usage: bash tests/leech_live.sh <lien magnet | fichier .torrent> [secondes]" >&2
    exit 2
fi

if [ ! -f /usr/include/mbedtls/ssl.h ]; then
    echo "mbedtls manquant : sudo apt-get install -y libmbedtls-dev" >&2
    exit 1
fi

cd "$SRC"
echo "compilation…"
if ! g++ -std=gnu++17 -O1 -g -Wall -Wextra -Wno-unused-parameter \
    -Iinclude \
    tests/leech_live.cpp \
    source/bt/session.cpp source/bt/peer.cpp source/bt/piece_picker.cpp source/bt/piece_writer.cpp \
    source/bt/storage.cpp source/bt/tracker.cpp source/bt/metainfo.cpp \
    source/bt/bencode.cpp source/bt/magnet.cpp source/bt/dht.cpp \
    source/net/bsd_transport.cpp source/net/io.cpp source/net/json.cpp source/net/tls.cpp \
    source/util/bytes.cpp source/util/sha1.cpp source/util/log.cpp \
    -lmbedtls -lmbedx509 -lmbedcrypto -lpthread \
    -o "$BIN" 2>&1; then
    echo "ÉCHEC de la compilation (voir ci-dessus)."
    exit 1
fi

echo
exec "$BIN" "$@"
