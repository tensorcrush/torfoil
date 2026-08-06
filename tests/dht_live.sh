#!/bin/bash
# Cherche des pairs sur le vrai DHT depuis un PC, avec le code embarqué.
#
#   bash tests/dht_live.sh "magnet:?xt=urn:btih:..."
#   bash tests/dht_live.sh dd8255ecdc7ca55fb0bbf81323d87062db1f6d1c
#
# Compile directement depuis l'arborescence : 4 s, et rien à synchroniser.
set -eo pipefail

SRC="$(cd "$(dirname "$0")/.." && pwd)"
BIN="${TMPDIR:-/tmp}/torfoil_dht_live"

if [ $# -lt 1 ]; then
    echo "usage: bash tests/dht_live.sh <lien magnet | info_hash>" >&2
    echo "       (pense aux guillemets : un magnet contient des & )" >&2
    exit 2
fi

cd "$SRC"
echo "compilation…"

# Sans cette redirection, une erreur de compilation part sur stderr, que
# PowerShell n'affiche pas toujours : le script semblerait s'arrêter sans raison.
if ! g++ -std=gnu++17 -O1 -g -Wall -Wextra -Wno-unused-parameter \
    -Iinclude \
    tests/dht_live.cpp \
    source/bt/dht.cpp source/bt/bencode.cpp source/bt/magnet.cpp \
    source/util/bytes.cpp source/util/sha1.cpp source/net/bsd_transport.cpp \
    -o "$BIN" 2>&1; then
    echo "ÉCHEC de la compilation (voir ci-dessus)."
    exit 1
fi

echo "recherche en cours (60 s), un point d'étape toutes les 5 s."
echo "Les 10 à 20 premières secondes restent à zéro : la résolution des nœuds"
echo "d'amorçage est bloquante. C'est normal, ça décolle ensuite d'un coup."
echo
exec "$BIN" "$@"
